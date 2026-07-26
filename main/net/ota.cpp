#include "ota.h"
#include "../model.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_rom_md5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ota";
static constexpr uint16_t OTA_UDP_PORT = 3232;

static volatile bool s_active;
static volatile int  s_percent;

bool ota_active()  { return s_active; }
int  ota_percent() { return s_percent; }

static void md5_hex(const uint8_t d[16], char out[33])
{
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

// Holt das Image beim Absender ab und schreibt es in den freien OTA-Slot.
static bool run_update(const ip_addr_t *peer_ip, uint16_t peer_port, size_t size,
                       const char *want_md5)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { ESP_LOGE(TAG, "kein freier OTA-Slot"); return false; }
    if (size > part->size) {
        ESP_LOGE(TAG, "Image %u B passt nicht in %u B", (unsigned)size, (unsigned)part->size);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(peer_port);
    dst.sin_addr.s_addr = peer_ip->u_addr.ip4.addr;

    timeval tv = {.tv_sec = 15, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(sock, (sockaddr *)&dst, sizeof dst) != 0) {
        ESP_LOGE(TAG, "TCP zu %s:%u fehlgeschlagen", ipaddr_ntoa(peer_ip), (unsigned)peer_port);
        close(sock);
        return false;
    }

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, size, &h) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fehlgeschlagen");
        close(sock);
        return false;
    }

    md5_context_t md5;
    esp_rom_md5_init(&md5);

    static uint8_t buf[1460];        // static: der Task-Stack ist knapp
    size_t got = 0;
    bool   ok  = true;
    s_active = true;
    s_percent = 0;

    while (got < size) {
        const size_t want = (size - got) < sizeof buf ? (size - got) : sizeof buf;
        const int n = recv(sock, buf, want, 0);
        if (n <= 0) { ESP_LOGE(TAG, "Abbruch nach %u B", (unsigned)got); ok = false; break; }
        if (esp_ota_write(h, buf, n) != ESP_OK) { ESP_LOGE(TAG, "Flash-Fehler"); ok = false; break; }
        esp_rom_md5_update(&md5, buf, n);
        got += n;
        s_percent = (int)(got * 100 / size);

        // Quittung: espota sendet 1024-B-Bloecke und wartet nach JEDEM Block
        // auf eine Antwort, bevor es den naechsten schickt (espota.py:174 ff.).
        // Geantwortet wird mit der Anzahl geschriebener Bytes als Dezimalzahl —
        // genau so macht es ArduinoOTA. Kommt ein Block in zwei Teilen an, gibt
        // es zwei Quittungen; espota liest bis zu 10 Byte je Runde und holt den
        // Ueberhang damit wieder ein. Erst die abschliessende Antwort muss "OK"
        // enthalten, danach sucht espota noch in bis zu fuenf Antworten danach.
        char ack[12];
        const int a = snprintf(ack, sizeof ack, "%d", n);
        if (send(sock, ack, a, 0) != a) { ok = false; break; }
    }

    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    char have[33];
    md5_hex(digest, have);

    if (ok && want_md5[0] && strcasecmp(have, want_md5) != 0) {
        ESP_LOGE(TAG, "MD5 passt nicht: %s statt %s", have, want_md5);
        ok = false;
    }
    if (ok && esp_ota_end(h) != ESP_OK)                 { ESP_LOGE(TAG, "esp_ota_end"); ok = false; }
    else if (!ok)                                        { esp_ota_abort(h); }
    if (ok && esp_ota_set_boot_partition(part) != ESP_OK) { ESP_LOGE(TAG, "set_boot"); ok = false; }

    if (ok) {
        send(sock, "OK", 2, 0);
        close(sock);
        ESP_LOGI(TAG, "Update fertig (%u B) — Neustart", (unsigned)got);
        model_notice("Update fertig — Neustart");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }

    close(sock);
    s_active = false;
    s_percent = 0;
    return ok;
}

static void ota_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me = {};
    me.sin_family      = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port        = htons(OTA_UDP_PORT);
    if (sock < 0 || bind(sock, (sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "UDP-Port %u nicht verfuegbar", (unsigned)OTA_UDP_PORT);
        if (sock >= 0) close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "espota lauscht auf UDP %u", (unsigned)OTA_UDP_PORT);

    for (;;) {
        char pkt[128];
        sockaddr_in from = {};
        socklen_t   flen = sizeof from;
        const int n = recvfrom(sock, pkt, sizeof pkt - 1, 0, (sockaddr *)&from, &flen);
        if (n <= 0) continue;
        pkt[n] = '\0';

        // "<cmd> <port> <groesse> <md5>\n" — cmd 0 = Anwendung, 100 = Dateisystem.
        // Wir haben kein Dateisystem, alles andere als 0 wird abgelehnt.
        int cmd = 0, port = 0;
        unsigned size = 0;
        char md5[40] = "";
        if (sscanf(pkt, "%d %d %u %39s", &cmd, &port, &size, md5) < 3) continue;
        if (cmd != 0) {
            ESP_LOGW(TAG, "Kommando %d wird nicht unterstuetzt (nur Anwendung)", cmd);
            sendto(sock, "ERR: nur Anwendung", 18, 0, (sockaddr *)&from, flen);
            continue;
        }

        ip_addr_t peer;
        ip_addr_set_ip4_u32(&peer, from.sin_addr.s_addr);
        ESP_LOGI(TAG, "Update von %s, %u B", ipaddr_ntoa(&peer), size);

        // "OK" quittiert die Anfrage; erst danach baut das Geraet die
        // TCP-Verbindung auf — espota wartet in genau dieser Reihenfolge.
        sendto(sock, "OK", 2, 0, (sockaddr *)&from, flen);
        model_notice("Update läuft ...");
        run_update(&peer, (uint16_t)port, size, md5);
    }
}

void ota_start()
{
    // Erster Start nach einem Update: das Image als brauchbar markieren, sonst
    // faellt der Bootloader beim naechsten Start auf den alten Slot zurueck
    // (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "neues Image bestaetigt");
    }
    xTaskCreate(ota_task, "espota", 4096, nullptr, 4, nullptr);
}
