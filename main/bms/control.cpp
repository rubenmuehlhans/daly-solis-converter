#include "control.h"
#include "daly.h"
#include "../app_config.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>

static const char *TAG = "control";

static inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Drei-Messungen-Mittel. Der Zaehler laeuft 1,2,3 und faengt danach wieder bei 1
// an — beim Ruecksprung wird die Summe auf den aktuellen Wert zurueckgesetzt,
// sonst mischte sich der alte Block in den neuen.
static uint16_t s_min_sum, s_max_sum;
static uint8_t  s_samples;
static uint16_t s_min_avg, s_max_avg;

// Zustand ueber die Zyklen hinweg.
static bool     s_discharge_block;   // untere Zellgrenze erreicht
static bool     s_hv_block;          // Batterie voll -> kein Ladestrom
static bool     s_ramp_hold;         // Erhoehen gesperrt
static bool     s_ramp_hold_by_soc;  // Sperre kam vom SOC, nicht von der Spannung
static uint32_t s_last_ramp_ms;      // Takt der Anpassung
static uint32_t s_above_free_ms;     // zuletzt oberhalb CELL_RAMP_FREE_MV

void control_init()
{
    s_min_sum = s_max_sum = 0;
    s_samples = 0;
    s_min_avg = s_max_avg = 0;
    s_discharge_block = s_hv_block = s_ramp_hold = s_ramp_hold_by_soc = false;
    s_last_ramp_ms = s_above_free_ms = now_ms();
}

void control_note_cells(uint16_t cell_min_mv, uint16_t cell_max_mv)
{
    if (s_samples < 3) {
        s_samples++;
        s_min_sum += cell_min_mv;
        s_max_sum += cell_max_mv;
    } else {
        s_samples = 1;
        s_min_sum = cell_min_mv;
        s_max_sum = cell_max_mv;
    }
    if (s_samples == 3) {
        s_min_avg = s_min_sum / 3;
        s_max_avg = s_max_sum / 3;
    }
    // Vor der ersten vollen Mittelung mit dem Momentanwert arbeiten — sonst
    // stuenden die ersten Sekunden nach dem Start auf 0 mV und die Regelung
    // haette "leere Zellen" gesehen.
    if (!s_min_avg) s_min_avg = cell_min_mv;
    if (!s_max_avg) s_max_avg = cell_max_mv;
}

uint16_t control_cell_min_avg() { return s_min_avg; }
uint16_t control_cell_max_avg() { return s_max_avg; }

// SOC im BMS nachkalibrieren UND sofort lokal uebernehmen.
//
// ⚠️ Das lokale Setzen ist der eigentliche Punkt, nicht der Schreibbefehl. Zwei
// Stellen haengen unmittelbar daran:
//  1. Die Ladeabschaltung. Sie wird gesetzt, wenn die Zellen ueber 3460 mV
//     gehen, und wieder aufgehoben, sobald soc_pm unter 980 faellt. Bliebe
//     soc_pm auf dem (zu tiefen, gerade deshalb kalibrierten) BMS-Wert, hoebe
//     die Regelung ihre eigene Abschaltung in derselben Runde wieder auf und
//     wuerde weiterladen — bis zur Hartabschaltung bei 3480 mV.
//  2. Der SOC im CAN-Rahmen 0x355. Am Entladeende ist der gemeldete SOC die
//     EINZIGE Rueckmeldung an den Wechselrichter (der Entladestrom wird
//     bewusst nicht auf 0 gezogen). Ohne die lokale Uebernahme bekaeme der
//     SOLIS weiter den zu hohen BMS-Wert und liesse die Zellen leerlaufen.
// Im Original steht dafuer `bat_soc = SOC * 10;` am Ende von bms_SOC_adjust().
static void calibrate_soc(BmsData &bms, uint8_t percent)
{
    daly_set_soc(percent);
    bms.soc_pm = (uint16_t)percent * 10;
}

// Ladestrom setzen. Wie im Original nur wirksam, wenn der Lade-MOSFET an ist —
// bei ausgeschaltetem MOSFET bleibt die Meldung an den SOLIS bei 0, sonst wuerde
// er gegen eine getrennte Batterie regeln.
static void set_charge(const BmsData &bms, Limits &lim, uint32_t ma)
{
    if (ma > 0 && !bms.charge_mos) return;
    if (ma > lim.charge_max_ma) ma = lim.charge_max_ma;
    lim.charge_ma = ma;
}

static void set_discharge(const BmsData &bms, Limits &lim, uint32_t ma)
{
    if (ma > 0 && !bms.discharge_mos) return;
    if (ma > lim.discharge_max_ma) ma = lim.discharge_max_ma;
    lim.discharge_ma = ma;
}

void control_step(BmsData &bms, Limits &lim, char *note, size_t note_cap)
{
    const uint32_t t = now_ms();

    // ---------------------------------------------------------- Entladepfad
    if (s_min_avg < CELL_MIN_ALLOWED_MV) {
        s_discharge_block = true;
        // Der SOC des DALY laeuft bei tiefen Zellen gern nach oben weg. Auf 10 %
        // zurueckziehen gibt dem SOLIS einen ehrlichen Wert; steht dessen
        // Zwangsladung auf 9 %, laedt er ab hier aus dem Netz nach.
        if (bms.soc_pm > 100) calibrate_soc(bms, 10);
    }
    if (s_min_avg > CELL_DISCHARGE_OK_MV) s_discharge_block = false;

    // ⚠️ Bewusst KEIN set_discharge(0) im gesperrten Fall: das Abschalten des
    // Entladepfads uebernehmen BMS und Wechselrichter. Der Adapter wuerde sonst
    // gegen deren eigene Abschaltung arbeiten (so auch im Original, dort
    // auskommentiert).
    if (!s_discharge_block) set_discharge(bms, lim, lim.discharge_max_ma);

    // ------------------------------------------------------------ Ladepfad
    if (s_max_avg > CELL_FULL_MV) {
        s_hv_block = true;
        calibrate_soc(bms, 100);
    }
    if (bms.soc_pm > 1000) {      // > 100 % kann nicht sein -> nachkalibrieren
        s_hv_block = true;
        calibrate_soc(bms, 100);
    }
    if (s_max_avg < CELL_RESUME_MV || bms.soc_pm < 980) s_hv_block = false;

    if (s_hv_block) {
        set_charge(bms, lim, 0);
        snprintf(note, note_cap,
                 "Ladestrom 0: Batterie voll (Zelle max %u mV, SOC %u.%u %%)",
                 (unsigned)s_max_avg, (unsigned)(bms.soc_pm / 10),
                 (unsigned)(bms.soc_pm % 10));
    } else if (bms.flow == Flow::charge) {
        // Ab bestimmten SOC-Marken wird hart gedeckelt. Das ist kein Ersatz fuer
        // die Spannungsregelung darunter, sondern ein zweites Netz: der SOC
        // reagiert traeger, faengt dafuer den Fall ab, dass die Zellspannungen
        // erst spaet anziehen.
        if (bms.soc_pm > 950 && lim.charge_ma > 50000) {
            set_charge(bms, lim, 50000);
            s_ramp_hold = s_ramp_hold_by_soc = true;
        }
        if (bms.soc_pm > 990 && lim.charge_ma > 20000) {
            set_charge(bms, lim, 20000);
            s_ramp_hold = s_ramp_hold_by_soc = true;
        }
        if (bms.soc_pm > 995 && lim.charge_ma > 5000) {
            set_charge(bms, lim, 5000);
            s_ramp_hold = s_ramp_hold_by_soc = true;
        }

        if (t - s_last_ramp_ms > RAMP_INTERVAL_MS) {
            s_last_ramp_ms = t;

            if (s_max_avg > CELL_RAMP_20A_MV && lim.charge_ma > 20000) {
                set_charge(bms, lim, 20000);
                s_ramp_hold = true;
            }
            if (s_max_avg > CELL_RAMP_DOWN_MV) {
                s_ramp_hold = true;
                if (lim.charge_ma > 5000) set_charge(bms, lim, lim.charge_ma - 1000);
            }
            // Erhoehen nur, wenn nichts dagegen spricht. Die Schrittweite
            // waechst mit dem Strom, damit der Wiederanstieg nach einer Wolke
            // nicht zehn Minuten dauert.
            //
            // ⚠️ ABWEICHUNG vom Original. Dort stand hier die Bedingung
            // `mos.getCurrentState()` — der Zustand des Home-Assistant-Schalters.
            // Der bleibt aber auf false, solange kein MQTT-Broker erreichbar ist
            // (ArduinoHA uebernimmt den Zustand nur bei erfolgreichem Publish).
            // Ohne Netz laedt das Original deshalb dauerhaft nur mit dem
            // Initialstrom von 5 A. Hier wird stattdessen der ECHTE
            // MOSFET-Zustand aus dem BMS gelesen — das war offensichtlich
            // gemeint. Folge: bei WLAN-Ausfall rampt diese Fassung normal hoch.
            if (lim.charge_ma < lim.charge_max_ma && !s_ramp_hold &&
                bms.charge_mos && bms.discharge_mos) {
                uint32_t step = lim.charge_ma <= 30000 ? 1000
                              : lim.charge_ma <= 50000 ? 2000 : 3000;
                set_charge(bms, lim, lim.charge_ma + step);
            }
        }

        // Sperre aufheben — aber erst, wenn die Zellen RAMP_HOLD_S lang unter
        // CELL_RAMP_FREE_MV geblieben sind. Eine SOC-bedingte Sperre bleibt
        // bestehen: die Batterie ist dann wirklich fast voll.
        if (s_max_avg < CELL_RAMP_FREE_MV) {
            if (s_ramp_hold && (t - s_above_free_ms) > RAMP_HOLD_S * 1000 &&
                !s_ramp_hold_by_soc) {
                s_ramp_hold = false;
            }
        } else {
            s_above_free_ms   = t;
            s_ramp_hold_by_soc = false;
        }
    }

    // Nach dem Vollladen wieder anfahren: sobald die Zellen abgesackt sind, mit
    // dem Initialstrom neu beginnen statt bei 0 stehen zu bleiben.
    if (lim.charge_ma == 0 && s_max_avg < CELL_RESUME_MV && !s_hv_block) {
        set_charge(bms, lim, CHARGE_CURRENT_INIT_MA);
    }

    // Harte Grenze zuletzt — sie ueberstimmt jede Rampe darueber.
    if (s_max_avg > CELL_HARD_STOP_MV) {
        set_charge(bms, lim, 0);
        snprintf(note, note_cap, "Ladestrom 0: Zelle max %u mV ueber Grenze %u mV",
                 (unsigned)s_max_avg, (unsigned)CELL_HARD_STOP_MV);
    }

    lim.hv_block  = s_hv_block;
    lim.lv_block  = s_discharge_block;
    lim.ramp_hold = s_ramp_hold;

    ESP_LOGD(TAG, "min%u max%u laden%lu entladen%lu hv%d lv%d hold%d",
             (unsigned)s_min_avg, (unsigned)s_max_avg,
             (unsigned long)lim.charge_ma, (unsigned long)lim.discharge_ma,
             (int)s_hv_block, (int)s_discharge_block, (int)s_ramp_hold);
}
