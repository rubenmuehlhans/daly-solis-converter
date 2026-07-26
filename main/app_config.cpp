#include "app_config.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdspi_host.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "config";
static AppConfig   s_cfg;

const AppConfig &config_get() { return s_cfg; }

// ---- NVS ------------------------------------------------------------------
static void nvs_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

static bool load_from_nvs()
{
    nvs_handle_t h;
    if (nvs_open("solis", NVS_READONLY, &h) != ESP_OK) return false;
    nvs_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid);
    nvs_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof s_cfg.wifi_pass);
    nvs_str(h, "mqtt_host", s_cfg.mqtt_host, sizeof s_cfg.mqtt_host);
    nvs_str(h, "mqtt_user", s_cfg.mqtt_user, sizeof s_cfg.mqtt_user);
    nvs_str(h, "mqtt_pass", s_cfg.mqtt_pass, sizeof s_cfg.mqtt_pass);
    uint16_t port = 1883;
    nvs_get_u16(h, "mqtt_port", &port);
    s_cfg.mqtt_port = port;
    nvs_close(h);
    return s_cfg.wifi_ssid[0] != '\0';
}

static void save_to_nvs()
{
    nvs_handle_t h;
    if (nvs_open("solis", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass);
    nvs_set_str(h, "mqtt_host", s_cfg.mqtt_host);
    nvs_set_str(h, "mqtt_user", s_cfg.mqtt_user);
    nvs_set_str(h, "mqtt_pass", s_cfg.mqtt_pass);
    nvs_set_u16(h, "mqtt_port", s_cfg.mqtt_port);
    nvs_commit(h);
    nvs_close(h);
}

// ---- SD -------------------------------------------------------------------
static void copy_trimmed(char *dst, size_t cap, const char *src)
{
    while (*src == ' ' || *src == '\t') src++;
    size_t n = strlen(src);
    while (n && (src[n - 1] == '\r' || src[n - 1] == '\n' ||
                 src[n - 1] == ' '  || src[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool starts_with(const char *line, const char *key, const char **val)
{
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0) return false;
    *val = line + k;
    return true;
}

// Mountet die Karte am SCHON initialisierten SPI-Bus — M5.begin() hat ihn fuer
// das Display eingerichtet. Deshalb host.slot = SPI_BUS und KEIN eigenes
// spi_bus_initialize: ein zweites gaebe ESP_ERR_INVALID_STATE und liesse die
// Karte tot aussehen, obwohl nur der Bus schon da war.
static bool load_from_sd()
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_BUS;
    host.max_freq_khz = 10000;      // die Karte teilt sich den Bus mit dem Display

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI_BUS;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files = 2;
    mnt.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mnt, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keine SD-Karte (%s) — nehme NVS", esp_err_to_name(err));
        return false;
    }

    FILE *f = fopen("/sd/config.txt", "r");
    bool got = false;
    if (!f) {
        ESP_LOGW(TAG, "SD ohne /config.txt");
    } else {
        char line[160];
        const char *v;
        while (fgets(line, sizeof line, f)) {
            if (starts_with(line, "wifi_ssid=", &v))     copy_trimmed(s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid, v);
            else if (starts_with(line, "wifi_password=", &v)) copy_trimmed(s_cfg.wifi_pass, sizeof s_cfg.wifi_pass, v);
            else if (starts_with(line, "mqtt_ip=", &v))  copy_trimmed(s_cfg.mqtt_host, sizeof s_cfg.mqtt_host, v);
            else if (starts_with(line, "mqtt_user=", &v)) copy_trimmed(s_cfg.mqtt_user, sizeof s_cfg.mqtt_user, v);
            else if (starts_with(line, "mqtt_password=", &v)) copy_trimmed(s_cfg.mqtt_pass, sizeof s_cfg.mqtt_pass, v);
            else if (starts_with(line, "mqtt_port=", &v)) {
                char buf[8];
                copy_trimmed(buf, sizeof buf, v);
                int p = atoi(buf);
                if (p > 0 && p < 65536) s_cfg.mqtt_port = (uint16_t)p;
            }
        }
        fclose(f);
        got = s_cfg.wifi_ssid[0] != '\0';
    }

    // Karte wieder loslassen: sie wird nur beim Start gelesen, und ein
    // gemounteter FAT-Treiber wuerde sonst bei jedem Display-Push um den Bus
    // konkurrieren. ⚠️ Das SPI-Device der Karte muss mit weg, sonst bleibt ein
    // Slot des Hosts belegt.
    esp_vfs_fat_sdcard_unmount("/sd", card);
    return got;
}

void config_load()
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.mqtt_port = 1883;

    if (load_from_sd()) {
        s_cfg.from_sd = true;
        s_cfg.valid   = true;
        save_to_nvs();
        ESP_LOGI(TAG, "Konfiguration von SD: ssid='%s' mqtt=%s:%u",
                 s_cfg.wifi_ssid, s_cfg.mqtt_host, (unsigned)s_cfg.mqtt_port);
        return;
    }

    // SD hatte nichts Brauchbares — was von der Karte kam, ist jetzt Fragment.
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.mqtt_port = 1883;
    s_cfg.valid = load_from_nvs();
    ESP_LOGI(TAG, "Konfiguration aus NVS: %s (ssid='%s')",
             s_cfg.valid ? "ok" : "leer", s_cfg.wifi_ssid);
}
