#include "wifi.h"
#include "../app_config.h"
#include "../model.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "wifi";

static void set_state(bool up, const char *ip)
{
    Model *m = model_lock();
    m->net.wifi = up;
    strlcpy(m->net.ip, ip ? ip : "", sizeof m->net.ip);
    if (!up) m->net.rssi = 0;
    model_unlock();
}

static void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        set_state(false, nullptr);
        // Ohne Pause laeuft der Treiber bei falschem Passwort in eine
        // Dauerschleife und frisst die CPU, die der BMS-Takt braucht.
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "verbunden, IP %s", ip);
        set_state(true, ip);
    }
}

// RSSI nachfuehren — nur fuers Display, deshalb gemuetliche 5 s.
static void rssi_task(void *)
{
    for (;;) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            Model *m = model_lock();
            m->net.rssi = ap.rssi;
            strlcpy(m->net.ssid, (const char *)ap.ssid, sizeof m->net.ssid);
            model_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wifi_start()
{
    const AppConfig &cfg = config_get();
    if (!cfg.wifi_ssid[0]) {
        ESP_LOGW(TAG, "keine SSID konfiguriert — WLAN bleibt aus");
        return;
    }

    // esp_netif_init()/esp_event_loop_create_default() macht app_main — sie
    // muessen auch dann laufen, wenn hier oben mangels SSID abgebrochen wurde.
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, nullptr, nullptr));

    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, cfg.wifi_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, cfg.wifi_pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // auch offene Netze zulassen

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    // Kein Stromsparen: der Adapter haengt an Dauerstrom, und Modem-Sleep
    // verzoegert MQTT-Kommandos um bis zu eine DTIM-Periode.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    {
        Model *m = model_lock();
        strlcpy(m->net.ssid, cfg.wifi_ssid, sizeof m->net.ssid);
        model_unlock();
    }
    xTaskCreate(rssi_task, "rssi", 2560, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "verbinde mit '%s'", cfg.wifi_ssid);
}
