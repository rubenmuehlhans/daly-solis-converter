#include "bms_task.h"
#include "daly.h"
#include "control.h"
#include "../app_config.h"
#include "../model.h"
#include "../can/mcp2515.h"
#include "../can/solis.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "bms";

static inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- Kommandos ------------------------------------------------------------

static void apply_mos(bool on, Limits &lim, const BmsData &bms)
{
    if (on) {
        daly_mos(true);
        // Erst nach dem Einschalten wieder Strom freigeben — mit Initialwert,
        // damit nicht sofort der volle Ladestrom auf eine gerade zugeschaltete
        // Batterie geht.
        lim.charge_ma    = CHARGE_CURRENT_INIT_MA;
        lim.discharge_ma = lim.discharge_max_ma;
        model_notice("MOSFET eingeschaltet");
    } else {
        // Reihenfolge ist sicherheitsrelevant: erst dem Wechselrichter sagen,
        // dass er 0 A nehmen soll, ihm zwei Sekunden Zeit lassen, DANN die
        // MOSFETs oeffnen. Andersherum trennt man einen laufenden Strom.
        lim.charge_ma = lim.discharge_ma = 0;
        solis_send(bms, lim);
        vTaskDelay(pdMS_TO_TICKS(2000));
        daly_mos(false);
        model_notice("MOSFET ausgeschaltet");
    }
}

static void handle_commands(Limits &lim, const BmsData &bms)
{
    Cmd c;
    while (cmd_take(&c, 0)) {
        switch (c.type) {
            case CmdType::mos_on:  apply_mos(true,  lim, bms); break;
            case CmdType::mos_off: apply_mos(false, lim, bms); break;

            case CmdType::set_soc: {
                uint8_t p = (uint8_t)(c.arg < 0 ? 0 : c.arg > 100 ? 100 : c.arg);
                daly_set_soc(p);
                char m[48];
                snprintf(m, sizeof m, "SOC auf %u %% gesetzt", (unsigned)p);
                model_notice(m);
                break;
            }
            case CmdType::set_charge_max:
                // Wie im Original sofort auf den neuen Wert springen, nicht erst
                // ueber die Rampe (die braucht von 5 A auf 90 A gut vier
                // Minuten). Nur wirksam, wenn der Lade-MOSFET an ist.
                lim.charge_max_ma = (uint32_t)c.arg * 1000;
                if (bms.charge_mos || lim.charge_max_ma < lim.charge_ma)
                    lim.charge_ma = lim.charge_max_ma;
                model_notice("Ladestromgrenze geändert");
                break;

            case CmdType::set_discharge_max:
                lim.discharge_max_ma = (uint32_t)c.arg * 1000;
                if (lim.discharge_ma > lim.discharge_max_ma) lim.discharge_ma = lim.discharge_max_ma;
                model_notice("Entladestromgrenze geändert");
                break;

            case CmdType::restart:
                model_notice("Neustart ...");
                vTaskDelay(pdMS_TO_TICKS(1500));   // Meldung soll lesbar sein
                esp_restart();
                break;
        }
    }
}

// ---- Takt -----------------------------------------------------------------

static void bms_task(void *)
{
    daly_init();
    control_init();

    BmsData sample = {};
    Limits  lim;
    {
        Model *m = model_lock();
        lim = m->lim;
        model_unlock();
    }

    int  cell_countdown = 0;     // 0 = in diesem Takt Einzelzellen mitlesen
    char err[80]  = "no message";
    char note[80] = "";

    for (;;) {
        const uint32_t t_start = now_ms();

        handle_commands(lim, sample);

        const bool with_cells = (cell_countdown <= 0);
        if (with_cells) cell_countdown = CELL_READ_EVERY;
        cell_countdown--;

        BmsData fresh = sample;      // Zellwerte des letzten Laufs behalten
        err[0] = '\0';
        bool got_minmax = false;
        const bool ok = daly_read(&fresh, with_cells, err, sizeof err, &got_minmax);

        // Mittelung fortschreiben, sobald 0x91 durchkam — auch wenn ein
        // spaeteres Kommando gescheitert ist. Sonst friert der Mittelwert bei
        // wiederkehrenden Fehlern auf einem alten (womoeglich zu tiefen) Wert
        // ein, und die Regelung drosselt den Ladestrom zu spaet.
        if (got_minmax) control_note_cells(fresh.cell_min_mv, fresh.cell_max_mv);
        // ok && err gesetzt = die Einzelzellen kamen nicht durch. Kein Grund,
        // den Durchlauf zu verwerfen (der SOLIS braucht sie nicht), aber es
        // gehoert gemeldet — im Original zaehlte das als Lesefehler mit.
        const bool cell_hiccup = ok && err[0];

        note[0] = '\0';
        bool tried_send = false, sent = false, implausible = false;

        if (ok) {
            control_step(fresh, lim, note, sizeof note);

            // Plausibilitaet: lieber gar nichts senden als Unsinn. Ein
            // verstuemmelter Stromwert wuerde den SOLIS zu einer falschen
            // Leistungsanforderung bringen.
            const bool plausible =
                fresh.pack_ci >= PLAUS_CURRENT_MIN_CI &&
                fresh.pack_ci <= PLAUS_CURRENT_MAX_CI &&
                fresh.pack_dv >= PLAUS_PACK_MIN_DV &&
                fresh.pack_dv <= PLAUS_PACK_MAX_DV;

            if (!plausible) {
                implausible = true;
                snprintf(err, sizeof err, "unplausibel: %ld dA / %u dV",
                         (long)fresh.pack_ci, (unsigned)fresh.pack_dv);
            } else {
                tried_send = true;
                sent = solis_send(fresh, lim);
                fresh.updated_ms = now_ms();
                fresh.fresh      = Fresh::fresh;
                sample = fresh;
            }
        }

        // ⚠️ VOR dem Sperren abfragen: das ist eine SPI-Transaktion und kann auf
        // den Bus warten, wenn das Display gerade zeichnet. Unter der Sperre
        // wuerde der Anzeige-Task so lange an model_read haengen.
        bool can_ok = mcp2515_alive();

        // Antwortet der Baustein nicht, alle 10 s neu initialisieren. Das
        // Original hat CAN genau einmal beim Start aufgesetzt und blieb danach
        // stumm, wenn das COMMU-Modul spaeter kam oder sich verschluckte — und
        // das Modul laesst sich laut Herstellerhinweis ohnehin nur durch einen
        // Kaltstart zuruecksetzen, nicht durch ESP.restart(). Ein Konverter,
        // der den Wechselrichter im Sekundentakt bedienen muss, darf sich nicht
        // bis zum naechsten Netzstecker abmelden.
        if (!can_ok) {
            static uint32_t last_try;
            if (now_ms() - last_try > 10000) {
                last_try = now_ms();
                can_ok = mcp2515_init();
                if (can_ok) ESP_LOGI(TAG, "CAN-Modul ist wieder da");
            }
        }

        // ---- Modell fortschreiben
        {
            Model *m = model_lock();
            const uint32_t age = now_ms() - sample.updated_ms;
            m->bms = sample;
            m->bms.fresh = !sample.updated_ms      ? Fresh::offline
                         : age > BMS_OFFLINE_MS    ? Fresh::offline
                         : age > BMS_STALE_MS      ? Fresh::stale
                                                   : Fresh::fresh;
            m->lim = lim;
            // Reihenfolge zaehlt: erst die Regel-Begruendung, dann ein echter
            // Lesefehler — der Fehler ist das Wichtigere und darf nicht von
            // "Ladestrom 0: Batterie voll" ueberschrieben werden.
            if (note[0]) strlcpy(m->diag.last_error, note, sizeof m->diag.last_error);
            if (!ok || cell_hiccup || implausible) {
                m->diag.err_count++;
                strlcpy(m->diag.last_error, err, sizeof m->diag.last_error);
            }
            // can_err zaehlt nur echte Sendefehler. Ohne gueltige BMS-Daten wird
            // gar nicht erst gesendet — das ist kein CAN-Problem.
            if (tried_send && !sent) m->diag.can_err++;
            else if (sent)           m->diag.can_tx += 5;   // fuenf Rahmen je Takt
            m->diag.can_ok   = can_ok;
            m->diag.cycle_ms = now_ms() - t_start;
            m->diag.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
            model_unlock();
        }

        if (!ok || cell_hiccup || implausible) ESP_LOGW(TAG, "Lesefehler: %s", err);

        // Auf den Sekundentakt auffuellen. Ein Durchlauf mit Zellen-Lesen dauert
        // laenger als eine Sekunde — dann gibt es hier keine Pause, und der
        // naechste Takt startet sofort (so war es auch im Original).
        const uint32_t spent = now_ms() - t_start;
        if (spent < 1000) vTaskDelay(pdMS_TO_TICKS(1000 - spent));
        else              taskYIELD();
    }
}

void bms_task_start()
{
    // Eigener Task, damit die 50-Hz-Anzeige nicht an den UART-Wartezeiten
    // haengt. Prioritaet ueber der UI: der Wechselrichter wartet auf seinen
    // Sekundentakt, das Display darf ein Bild spaeter kommen.
    xTaskCreatePinnedToCore(bms_task, "bms", 4096, nullptr, 6, nullptr, APP_CPU_NUM);
}
