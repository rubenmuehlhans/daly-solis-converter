// SolisM5 — DALY-BMS zu SOLIS-Wechselrichter, M5Stack Core Basic.
//
// Aufgabenverteilung:
//   bms   (Prio 6)  UART zum BMS, Regelung, CAN zum Wechselrichter — 1 Hz
//   ha    (Prio 3)  MQTT/Home Assistant — 1 Hz
//   espota(Prio 4)  wartet auf ein Firmware-Update
//   main  (Prio 1)  Anzeige und Tasten — 50 Hz
// Alles teilt sich genau einen Zustand (model.h), gelesen als Kopie.
//
// Wichtig fuer die Reihenfolge in app_main: M5.begin() richtet den SPI-Bus ein.
// SD-Karte und CAN haengen am selben Bus und duerfen sich erst DANACH anmelden.
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "model.h"
#include "bms/bms_task.h"
#include "can/mcp2515.h"
#include "net/wifi.h"
#include "net/ha_mqtt.h"
#include "net/ota.h"
#include "ui/theme.h"
#include "ui/tiles.h"
#include "ui/pages.h"
#include "ui/screens.h"
#if SOLIS_SELFTEST
#include "selftest.h"
#endif

static const char *TAG = "solis";

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "[%s] intern frei: %u B (min %u)", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "SolisM5 %s", esp_app_get_description()->version);
    log_heap("boot");

    auto cfg = M5.config();
    cfg.internal_spk = true;      // Piezo fuer die Fehlermeldung
    cfg.clear_display = false;    // wir malen selbst, kein weisses Aufblitzen
    M5.begin(cfg);
    ESP_LOGI(TAG, "board=%d display=%dx%d", (int)M5.getBoard(),
             (int)M5.Display.width(), (int)M5.Display.height());

    ui_fonts_init();
    tiles_init();
    theme_set(ThemeMode::day);
    screen_boot("Start");
    log_heap("nach UI-Init");

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nv = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nv);

    model_init();
    cmd_init();

    screen_boot("Konfiguration lesen");
    config_load();

    // CAN. Faellt der Baustein aus, laeuft der Rest trotzdem weiter: das Panel
    // soll die Batteriewerte zeigen und melden koennen, DASS der Bus fehlt —
    // ein 60-Sekunden-Stillstand wie im Original hilft niemandem.
    screen_boot("CAN-Bus");
    const bool can_ok = mcp2515_init();
    {
        Model *m = model_lock();
        m->diag.can_ok = can_ok;
        if (!can_ok) strlcpy(m->diag.last_error, "CAN-Modul antwortet nicht",
                             sizeof m->diag.last_error);
        model_unlock();
    }
    if (!can_ok) {
        screen_boot("CAN-Modul antwortet nicht", true);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // Im Selbsttest bleibt der BMS-Task aus: er wuerde die eingespielten
    // Demowerte im Sekundentakt wieder ueberschreiben.
#if !SOLIS_SELFTEST
    screen_boot("BMS starten");
    bms_task_start();
#endif

    // ⚠️ Netzwerkstack IMMER hochziehen, auch ohne konfiguriertes WLAN.
    // esp_netif_init() ist die einzige Stelle, die tcpip_init() aufruft; ohne
    // sie stuerzt schon das erste socket() im espota-Task ab (lwip greift dann
    // auf eine NULL-Mailbox zu) — genau auf dem frisch ausgepackten Geraet ohne
    // SD-Karte, das laut Konfigurationslogik hochkommen soll.
    screen_boot("Netzwerk");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_start();
    ha_mqtt_start();
    // Ohne WLAN gibt es niemanden, der ein Update schicken koennte — dann auch
    // keinen Lauscher starten.
    if (config_get().wifi_ssid[0]) ota_start();

    log_heap("nach Netz-Start");

    // Erster Lesedurchlauf braucht ~1 s; solange stuenden ueberall Striche.
    vTaskDelay(pdMS_TO_TICKS(1200));

    const int64_t t0 = esp_timer_get_time();
    pages_init();
    pages_show(0);
    ESP_LOGI(TAG, "Vollaufbau Seite: %lld us", esp_timer_get_time() - t0);
    log_heap("nach erster Seite");

#if SOLIS_SELFTEST
    selftest_run();
#endif

    uint32_t last_err = 0;
    for (;;) {
        M5.update();
        const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        pages_tick(ms);

        // Signalton bei NEUEN Lesefehlern — nicht bei jedem Takt, in dem der
        // Zaehler schon hochstand.
        Model m;
        model_read(&m);
        if (m.diag.err_count != last_err) {
            if (m.diag.beep && m.diag.err_count > last_err) M5.Speaker.tone(880, 120);
            last_err = m.diag.err_count;
        }

        vTaskDelay(pdMS_TO_TICKS(20));   // 50 Hz Tastenabfrage
    }
}
