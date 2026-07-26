#include "mcp2515.h"
#include "../app_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "mcp2515";

// ---- Registerkarte (Datenblatt MCP2515, Kapitel 11/12) --------------------
enum : uint8_t {
    OP_WRITE   = 0x02,
    OP_READ    = 0x03,
    OP_BITMOD  = 0x05,
    OP_RTS_TX0 = 0x81,
    OP_RESET   = 0xC0,
};
enum : uint8_t {
    R_CANSTAT  = 0x0E,
    R_CANCTRL  = 0x0F,
    R_RXM0SIDH = 0x20,
    R_RXM1SIDH = 0x24,
    R_CNF3     = 0x28,
    R_CNF2     = 0x29,
    R_CNF1     = 0x2A,
    R_CANINTE  = 0x2B,
    R_CANINTF  = 0x2C,
    R_TXB0CTRL = 0x30,
    R_TXB0SIDH = 0x31,
    R_RXB0CTRL = 0x60,
    R_RXB1CTRL = 0x70,
};
enum : uint8_t {
    MODE_NORMAL = 0x00,
    MODE_CONFIG = 0x80,
    MODE_MASK   = 0xE0,
    TXREQ       = 0x08,
};

// 500 kbit/s an einem 8-MHz-Quarz. Die drei Werte stammen aus der im Original
// verwendeten Bibliothek (coryjfowler MCP_CAN, MCP_8MHz_500kBPS_CFG1..3) und
// sind gegen genau diesen SOLIS erprobt — nicht neu ausgerechnet.
static constexpr uint8_t CFG1 = 0x00, CFG2 = 0xD1, CFG3 = 0x81;

static spi_device_handle_t s_dev;
static bool                s_ready;

// ---- SPI ------------------------------------------------------------------
//
// ⚠️⚠️ CS wird VON HAND gezogen, nicht ueber die Hardware-CS des SPI-Controllers
// (`spics_io_num` bleibt deshalb -1). Grund:
//
//   M5GFX schreibt in seinem beginTransaction() direkt ins Register
//   SPI_PIN_REG/SPI_MISC_REG und setzt dort SPI_CS0_DIS|CS1_DIS|CS2_DIS —
//   es schaltet also ALLE Hardware-CS-Leitungen ab, an der IDF vorbei.
//   Der IDF-Treiber stellt seine Geraetekonfiguration laut eigenem Kommentar
//   nur wieder her, "when the dev_id is changed" (spi_master.c,
//   spi_setup_device → spi_bus_lock_touch). Auf einem Bus, den ein Treiber so
//   umgeht, ist Hardware-CS nicht verlaesslich — die Initialisierung des
//   MCP2515 scheiterte damit am Geraet.
//
//   Die Arduino-Bibliothek macht es seit jeher manuell (`digitalWrite(MCPCS,
//   LOW)`, mcp_can_dfs.h:412) und laeuft auf genau dieser Hardware. Wir machen
//   es genauso.
//
// ⚠️ Dazu gehoert zwingend spi_device_acquire_bus() ueber die GANZE CS-Phase:
// sonst schiebt M5GFX mitten in ein MCP2515-Kommando Pixel auf den Bus, und der
// Baustein liest Bilddaten als Befehle. Im Arduino-Original stellt sich die
// Frage nicht, dort laeuft alles in einer Schleife.

static bool xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_dev) return false;

    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    // ⚠️ portMAX_DELAY ist Pflicht: spi_device_acquire_bus lehnt jede endliche
    // Wartezeit mit ESP_ERR_INVALID_ARG ab ("acquire finite time not supported
    // now.", spi_master.c:1280). Das Warten ist unkritisch — den Bus haelt nur
    // das Display, und zwar je Push wenige Millisekunden.
    esp_err_t err = spi_device_acquire_bus(s_dev, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bus nicht bekommen: %s", esp_err_to_name(err));
        return false;
    }
    gpio_set_level(PIN_CAN_CS, 0);
    err = spi_device_polling_transmit(s_dev, &t);
    gpio_set_level(PIN_CAN_CS, 1);
    spi_device_release_bus(s_dev);

    if (err != ESP_OK) ESP_LOGE(TAG, "Transfer: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool reg_write(uint8_t addr, uint8_t val)
{
    const uint8_t f[3] = {OP_WRITE, addr, val};
    return xfer(f, nullptr, sizeof f);
}

static bool reg_read(uint8_t addr, uint8_t *out)
{
    const uint8_t tx[3] = {OP_READ, addr, 0x00};
    uint8_t rx[3] = {0, 0, 0};
    if (!xfer(tx, rx, sizeof tx)) return false;
    *out = rx[2];
    return true;
}

static bool reg_modify(uint8_t addr, uint8_t mask, uint8_t val)
{
    const uint8_t f[4] = {OP_BITMOD, addr, mask, val};
    return xfer(f, nullptr, sizeof f);
}

static bool set_mode(uint8_t mode, uint8_t *last_read)
{
    if (!reg_modify(R_CANCTRL, MODE_MASK, mode)) return false;
    for (int i = 0; i < 20; i++) {           // Moduswechsel braucht Taktzyklen
        uint8_t st = 0;
        if (!reg_read(R_CANSTAT, &st)) return false;
        if (last_read) *last_read = st;
        if ((st & MODE_MASK) == mode) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

// ---- oeffentlich ----------------------------------------------------------

bool mcp2515_init()
{
    s_ready = false;

    // CS als gewoehnlicher Ausgang, Ruhepegel HIGH.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_CAN_CS;
    io.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_CAN_CS, 1);

    if (!s_dev) {
        spi_device_interface_config_t dev = {};
        dev.mode           = 0;              // MCP2515 kann 0,0 und 1,1
        dev.clock_speed_hz = 10 * 1000 * 1000;
        dev.spics_io_num   = -1;             // CS machen wir selbst, s. oben
        dev.queue_size     = 1;
        esp_err_t err = spi_bus_add_device(SPI_BUS, &dev, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
            s_dev = nullptr;
            return false;
        }
    }

    const uint8_t rst = OP_RESET;
    if (!xfer(&rst, nullptr, 1)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t st = 0xFF;
    if (!set_mode(MODE_CONFIG, &st)) {
        // Rohwert mitgeben: 0x00 oder 0xFF heisst "niemand treibt MISO" —
        // dann steckt das COMMU-Modul nicht oder die Verkabelung stimmt nicht.
        // Alles andere heisst: der Baustein antwortet, aber nicht wie erwartet.
        ESP_LOGE(TAG, "kein Config-Mode (CANSTAT liest 0x%02X) — %s",
                 st, (st == 0x00 || st == 0xFF) ? "Modul steckt nicht?"
                                                : "unerwartete Antwort");
        return false;
    }

    bool ok = true;
    ok &= reg_write(R_CNF1, CFG1);
    ok &= reg_write(R_CNF2, CFG2);
    ok &= reg_write(R_CNF3, CFG3);

    // Alles annehmen (entspricht MCP_ANY im Original). Wir lesen zwar nichts,
    // aber ein Empfangspuffer, der nie ueberlaeuft, kann auch nie stoeren.
    ok &= reg_write(R_RXB0CTRL, 0x60);
    ok &= reg_write(R_RXB1CTRL, 0x60);
    for (uint8_t i = 0; i < 4; i++) {
        ok &= reg_write(R_RXM0SIDH + i, 0x00);
        ok &= reg_write(R_RXM1SIDH + i, 0x00);
    }
    ok &= reg_write(R_CANINTE, 0x00);      // wir pollen
    ok &= reg_write(R_CANINTF, 0x00);
    if (!ok) { ESP_LOGE(TAG, "Konfigurationsregister nicht beschreibbar"); return false; }

    // Gegenprobe VOR dem Moduswechsel: CNF2 ist nur im Config-Mode schreibbar,
    // ein korrekter Rueckwert beweist also eine funktionierende Verbindung.
    uint8_t back = 0;
    if (!reg_read(R_CNF2, &back) || back != CFG2) {
        ESP_LOGE(TAG, "CNF2 liest 0x%02X statt 0x%02X — SPI-Verbindung pruefen",
                 back, CFG2);
        return false;
    }

    if (!set_mode(MODE_NORMAL, &st)) {
        ESP_LOGE(TAG, "kein Normal-Mode (CANSTAT liest 0x%02X)", st);
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "bereit: 500 kbit/s, 8-MHz-Quarz, CS=GPIO%d (manuell)",
             (int)PIN_CAN_CS);
    return true;
}

bool mcp2515_alive()
{
    if (!s_dev || !s_ready) return false;
    uint8_t st = 0;
    if (!reg_read(R_CANSTAT, &st)) return false;
    return (st & MODE_MASK) == MODE_NORMAL;
}

bool mcp2515_send(uint16_t sid, const uint8_t *data, uint8_t len)
{
    if (!s_ready || len > 8) return false;

    // Auf den Sendepuffer warten. Bei 500 kbit/s dauert ein Standardrahmen
    // ~0,25 ms; haengt TXREQ laenger, ist niemand am Bus (kein ACK) — dann
    // abbrechen statt den Aufrufer festzuhalten.
    for (int i = 0; i < 50; i++) {
        uint8_t ctrl = 0;
        if (!reg_read(R_TXB0CTRL, &ctrl)) return false;
        if (!(ctrl & TXREQ)) break;
        if (i == 49) {
            reg_modify(R_TXB0CTRL, TXREQ, 0);   // Sendeauftrag verwerfen
            ESP_LOGW(TAG, "TXB0 blockiert (kein ACK am Bus?) — Rahmen 0x%03X verworfen",
                     (unsigned)sid);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // SIDH/SIDL/EID8/EID0/DLC/D0..D7 in einem Rutsch ab TXB0SIDH schreiben.
    uint8_t f[2 + 5 + 8];
    f[0] = OP_WRITE;
    f[1] = R_TXB0SIDH;
    f[2] = (uint8_t)(sid >> 3);
    f[3] = (uint8_t)((sid & 0x07) << 5);   // Standard-ID, kein IDE
    f[4] = 0;
    f[5] = 0;
    f[6] = len;
    memcpy(&f[7], data, len);
    if (!xfer(f, nullptr, 7 + len)) return false;

    const uint8_t rts = OP_RTS_TX0;
    return xfer(&rts, nullptr, 1);
}
