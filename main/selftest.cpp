#include "selftest.h"
#include "model.h"
#include "ui/theme.h"
#include "ui/pages.h"
#include "ui/screens.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Zeilenweise lesen und sofort ausgeben — kein 150-KB-Puffer noetig.
static void dump(const char *name)
{
    static uint16_t line[lay::W];

    // ⚠️ Logging waehrend des Dumps AUS: ESP_LOGx schreibt asynchron in
    // denselben stdout und landete sonst mitten in einer Base64-Zeile.
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("\n@@SHOT %s %d %d\n", name, lay::W, lay::H);
    for (int y = 0; y < lay::H; y++) {
        M5.Display.readRect(0, y, lay::W, 1, line);
        char out[(lay::W * 2 + 2) / 3 * 4 + 8];
        int oi = 0;
        const uint8_t *src = (const uint8_t *)line;
        const int n = lay::W * 2;
        for (int i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)src[i] << 16;
            if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
            if (i + 2 < n) v |= src[i + 2];
            out[oi++] = B64[(v >> 18) & 63];
            out[oi++] = B64[(v >> 12) & 63];
            out[oi++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
            out[oi++] = (i + 2 < n) ? B64[v & 63] : '=';
        }
        out[oi] = 0;
        // ⚠️ Zeilennummer MUSS mit: auf der Strecke gehen vereinzelt Bytes
        // verloren. Ohne Nummer verwirft der Host den ganzen Screenshot, mit
        // Nummer kostet ein Ausfall nur diese eine Bildzeile.
        printf("@@L%03d %s\n", y, out);
        fflush(stdout);
        if ((y & 15) == 15) vTaskDelay(1);   // UART Luft zum Leerlaufen geben
    }
    printf("@@END %s\n", name);
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

// Werte einspielen, die es am Schreibtisch nicht gibt. Bewusst KEIN Happy Path:
// eine Zelle laeuft weg, damit die kritische Kachel und der Ausreisser im
// Balkenbild mit im Abzug stehen.
static void inject()
{
    Model *m = model_lock();
    BmsData &b = m->bms;
    b.fresh        = Fresh::fresh;
    b.updated_ms   = (uint32_t)(esp_timer_get_time() / 1000);
    b.pack_dv      = 5324;          // 53,24 V
    b.pack_ci      = -124;          // -12,4 A
    b.soc_pm       = 824;           // 82,4 %
    b.temp_c       = 23;
    b.flow         = Flow::discharge;
    b.charge_mos   = true;
    b.discharge_mos = true;
    b.rest_mah     = 47200;
    b.cycles       = 312;
    b.cells_valid  = true;
    static const uint16_t mv[CELL_COUNT] = {
        3284, 3301, 3412, 3298, 3305, 3299, 3302, 3288,
        3295, 3300, 3286, 3297, 3303, 3299, 3301, 3296 };
    memcpy(b.cell_mv, mv, sizeof mv);
    b.cell_max_mv = 3412; b.cell_max_no = 3;
    b.cell_min_mv = 3284; b.cell_min_no = 1;

    m->lim.charge_ma       = 42000;
    m->lim.discharge_ma    = 100000;
    m->diag.can_ok         = true;
    m->diag.err_count      = 0;
    m->diag.uptime_s       = 6 * 3600 + 14 * 60;
    m->net.wifi = m->net.mqtt = true;
    strlcpy(m->net.ip, "192.168.1.225", sizeof m->net.ip);
    strlcpy(m->diag.last_error, "no message", sizeof m->diag.last_error);
    model_unlock();
}

void selftest_run()
{
    inject();

    // Byte-Reihenfolge des Readbacks EINMAL protokollieren.
    // ⚠️ Aus diesem Wert allein NICHT auf die Deutung schliessen — es sind zwei
    // Vertauschungen, die sich aufheben. Belastbar ist nur der End-zu-End-Test
    // im Host-Skript (check_sanity).
    M5.Display.fillRect(0, 0, 4, 4, 0xF800);
    uint16_t probe = 0;
    M5.Display.readRect(1, 1, 1, 1, &probe);
    printf("@@PROBE wrote=F800 read=%04X\n", probe);

    static const char *names[PAGE_COUNT] = {
        "page0_energie", "page1_batterie", "page2_zellen", "page3_system" };
    for (int i = 0; i < PAGE_COUNT; i++) {
        pages_show(i);
        vTaskDelay(pdMS_TO_TICKS(150));
        dump(names[i]);
    }

    // Halte-Bestaetigung bei halbem Fortschritt
    pages_show(0);
    screen_confirm_mos(false, 750, 1500);
    vTaskDelay(pdMS_TO_TICKS(150));
    dump("confirm_mos");

    screens_invalidate();
    screen_ota(63);
    vTaskDelay(pdMS_TO_TICKS(150));
    dump("ota");

    theme_set(ThemeMode::night);
    pages_show(0);
    vTaskDelay(pdMS_TO_TICKS(150));
    dump("page0_nacht");
    pages_show(1);
    vTaskDelay(pdMS_TO_TICKS(150));
    dump("page1_nacht");

    theme_set(ThemeMode::day);
    pages_show(0);
    printf("@@DONE\n");
    fflush(stdout);
}
