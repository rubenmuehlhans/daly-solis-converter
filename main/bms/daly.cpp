#include "daly.h"
#include "../app_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "daly";

// Ein geteilter Puffer wie im Original — der UART gehoert genau einem Task.
static uint8_t mbuf[13];

static inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

void daly_init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = BMS_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(BMS_UART, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(BMS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BMS_UART, BMS_PIN_TX, BMS_PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(BMS_UART);
    ESP_LOGI(TAG, "UART%d @%d (RX=%d TX=%d)", (int)BMS_UART, BMS_BAUD,
             (int)BMS_PIN_RX, (int)BMS_PIN_TX);
}

// ---- Rahmen ---------------------------------------------------------------

static void tx(uint8_t cmd)
{
    memset(mbuf, 0, sizeof mbuf);
    mbuf[0] = 0xA5;
    mbuf[1] = BMS_HOST_ADDR;
    mbuf[2] = cmd;
    mbuf[3] = 0x08;
    uint8_t cs = 0;
    for (int i = 0; i <= 11; i++) cs += mbuf[i];
    mbuf[12] = cs;
    uart_write_bytes(BMS_UART, mbuf, sizeof mbuf);
    uart_wait_tx_done(BMS_UART, pdMS_TO_TICKS(200));
}

// Antwort einlesen. settle_ms ist die Pause VOR dem Lesen (das BMS braucht sie),
// danach wird byteweise resynchronisiert: passt ein erwartetes Kopfbyte nicht,
// faengt der Rahmen von vorn an. So faellt ein halber Frame im Puffer nicht auf
// die naechste Antwort durch.
static bool rx(uint8_t cmd, uint32_t settle_ms = 100)
{
    // ⚠️ Die Uhr laeuft VOR der Wartezeit los — genau wie im Original, wo
    // start_time vor dem delay(rxdelay) gesetzt wird. Das effektive Lesefenster
    // ist damit BMS_RX_TIMEOUT_MS minus settle_ms. Startete sie erst danach,
    // dauerte eine stumme 0x95-Runde 6 x 2,2 s statt 6 x 2,0 s — so lange
    // bekaeme der Wechselrichter keinen einzigen Rahmen.
    const uint32_t t0 = now_ms();
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    uint8_t cs = 0;
    int i = 0;
    memset(mbuf, 0, sizeof mbuf);

    while (i <= 12) {
        uint8_t b;
        if (uart_read_bytes(BMS_UART, &b, 1, pdMS_TO_TICKS(20)) == 1) {
            mbuf[i] = b;
            const bool bad =
                (i == 0  && b != 0xA5)            ||   // Sync
                (i == 1  && b != BMS_SLAVE_ADDR)  ||   // Absender
                (i == 2  && b != cmd)             ||   // Kommando-Echo
                (i == 3  && b != 0x08)            ||   // feste Laenge
                (i == 12 && b != cs);                  // Pruefsumme
            if (bad) { i = 0; cs = 0; }
            else     { cs += b; i++; }
        }
        if (now_ms() - t0 > BMS_RX_TIMEOUT_MS) return false;
    }
    return true;
}

static inline uint16_t d_byte(int i) { return mbuf[i + 4]; }
static inline uint16_t d_word(int i)
{
    return (uint16_t)((mbuf[i + 4] << 8) | mbuf[i + 5]);
}

// ---- Lesedurchlauf --------------------------------------------------------

#define FAIL(fmt, ...)                                                         \
    do {                                                                       \
        snprintf(err, err_cap, fmt, ##__VA_ARGS__);                            \
        return false;                                                          \
    } while (0)

bool daly_read(BmsData *out, bool with_cells, char *err, size_t err_cap,
               bool *got_minmax)
{
    err[0] = '\0';
    if (got_minmax) *got_minmax = false;

    // 0x90 — Spannung, Strom, SOC
    tx(0x90);
    if (!rx(0x90)) FAIL("at send CMD 90");
    out->pack_dv = d_word(0) * 10;          // 0,1 V -> 10 mV
    out->pack_ci = (int32_t)d_word(4) - 30000;   // Offset 30000 laut Protokoll
    out->soc_pm  = d_word(6);               // 0,1 %

    vTaskDelay(pdMS_TO_TICKS(20));

    // 0x91 — hoechste/niedrigste Zellspannung samt Zellnummer
    tx(0x91);
    if (!rx(0x91)) FAIL("at send CMD 91");
    out->cell_max_mv = d_word(0);
    out->cell_max_no = (uint8_t)d_byte(2);
    out->cell_min_mv = d_word(3);
    out->cell_min_no = (uint8_t)d_byte(5);
    if (got_minmax) *got_minmax = true;

    vTaskDelay(pdMS_TO_TICKS(20));

    // 0x92 — Temperatur (Offset 40)
    tx(0x92);
    if (!rx(0x92)) FAIL("at send CMD 92");
    out->temp_c = (int16_t)d_byte(0) - 40;

    vTaskDelay(pdMS_TO_TICKS(20));

    // 0x93 — Lade-/Entladezustand, MOSFETs, Restkapazitaet
    tx(0x93);
    if (!rx(0x93)) FAIL("at send CMD 93");
    switch (d_byte(0)) {
        case 1:  out->flow = Flow::charge;    break;
        case 2:  out->flow = Flow::discharge; break;
        default: out->flow = Flow::idle;      break;
    }
    out->charge_mos    = d_byte(1) != 0;
    out->discharge_mos = d_byte(2) != 0;
    out->rest_mah = ((uint32_t)d_byte(4) << 24) | ((uint32_t)d_byte(5) << 16) |
                    ((uint32_t)d_byte(6) << 8)  |  (uint32_t)d_byte(7);

    vTaskDelay(pdMS_TO_TICKS(20));

    // 0x94 — Ladezyklen
    tx(0x94);
    if (!rx(0x94)) FAIL("at send CMD 94");
    out->cycles = d_word(5);

    vTaskDelay(pdMS_TO_TICKS(20));

    if (!with_cells) return true;

    // 0x95 — Einzelzellspannungen: EIN Kommando, danach sechs Antwortframes
    // mit je einer Frame-Nummer und bis zu drei Zellen.
    // ⚠️ Die Frame-Nummer kommt gelegentlich verstuemmelt an. Ohne die Pruefung
    // "Nummer == erwarteter Index" wird an falschen Offsets geschrieben — das
    // hat im Original Abstuerze verursacht. Fehlschlaege hier sind NICHT
    // toedlich: die Einzelzellen gehen den SOLIS nichts an, der Rest des
    // Durchlaufs bleibt gueltig.
    tx(0x95);
    int frames_ok = 0;
    for (int i = 0; i < 6; i++) {
        if (!rx(0x95, 200)) {
            // ⚠️ cells_valid bleibt, wie es war: die zuletzt gelesenen
            // Zellspannungen sind zwar alt, aber richtig. Sie hier zu
            // verwerfen hiesse, die Zellenseite auf "wird gelesen" zu setzen,
            // obwohl brauchbare Werte vorliegen.
            snprintf(err, err_cap, "at send CMD 95 (%d)", i);
            return true;
        }
        const int frame = d_byte(0);
        if (frame == i + 1) {
            frames_ok++;
            const int off = (frame - 1) * 3;
            out->cell_mv[off] = d_word(1);
            if (i < 5) {
                out->cell_mv[off + 1] = d_word(3);
                out->cell_mv[off + 2] = d_word(5);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // ⚠️ Nur bei 6 von 6 gueltigen Rahmen. Ein verworfener Rahmen laesst drei
    // Zellen auf ihrem alten Wert stehen — beim ersten Durchlauf also auf 0 mV.
    // Die als gueltig zu melden hiesse: Nullbalken im Diagramm, falsche
    // Streuung und 0,000 V in Home Assistant.
    if (frames_ok == 6) out->cells_valid = true;
    else snprintf(err, err_cap, "CMD 95 unvollstaendig (%d/6)", frames_ok);
    return true;
}

#undef FAIL

// ---- Schreibende Kommandos ------------------------------------------------

static void write_frame_and_drain()
{
    uart_write_bytes(BMS_UART, mbuf, sizeof mbuf);
    uart_wait_tx_done(BMS_UART, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(100));   // das BMS quittiert mit 13 Bytes
    uart_flush_input(BMS_UART);
}

// SOC im BMS nachkalibrieren. Muster: a5 40 21 08 15 03 0b 0c 3a 39 <soc_hi> <soc_lo> <cs>
// Die Bytes 4..9 sind ein Zeitstempel, den das BMS nicht auswertet — sie bleiben
// wie im Original konstant.
void daly_set_soc(uint8_t percent)
{
    memset(mbuf, 0, sizeof mbuf);
    mbuf[0] = 0xA5;
    mbuf[1] = BMS_HOST_ADDR;
    mbuf[2] = 0x21;
    mbuf[3] = 0x08;
    mbuf[4] = 0x15; mbuf[5] = 0x03; mbuf[6] = 0x0B;
    mbuf[7] = 0x0C; mbuf[8] = 0x3A; mbuf[9] = 0x39;
    const uint16_t soc_pm = (uint16_t)percent * 10;   // BMS erwartet 0,1 %
    mbuf[10] = (soc_pm >> 8) & 0xFF;
    mbuf[11] = soc_pm & 0xFF;
    uint8_t cs = 0;
    for (int i = 0; i < 12; i++) cs += mbuf[i];
    mbuf[12] = cs;

    ESP_LOGI(TAG, "SOC-Kalibrierung auf %u %%", (unsigned)percent);
    write_frame_and_drain();
}

// MOSFETs schalten. Reihenfolge ist Absicht:
//   EIN : erst Entlade-MOS, dann Lade-MOS
//   AUS : erst Lade-MOS,    dann Entlade-MOS
// So ist der Verbraucherpfad beim Einschalten schon da bzw. beim Ausschalten
// noch da — die Batterie haengt nie mit offenem Entladepfad an einer Ladequelle.
void daly_mos(bool on)
{
    const uint8_t cmds[2] = { on ? (uint8_t)0xD9 : (uint8_t)0xDA,
                              on ? (uint8_t)0xDA : (uint8_t)0xD9 };
    for (int k = 0; k < 2; k++) {
        memset(mbuf, 0, sizeof mbuf);
        mbuf[0] = 0xA5;
        mbuf[1] = BMS_HOST_ADDR;
        mbuf[2] = cmds[k];
        mbuf[3] = 0x08;
        mbuf[4] = on ? 0x01 : 0x00;
        uint8_t cs = 0;
        for (int i = 0; i < 12; i++) cs += mbuf[i];
        mbuf[12] = cs;
        write_frame_and_drain();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "MOSFET %s", on ? "ein" : "aus");
}
