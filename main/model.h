// Gemeinsames Datenmodell — die einzige Schnittstelle zwischen den Schichten.
//
// Wer schreibt, wer liest:
//   bms/daly_task.cpp   schreibt bms/limits/diag   (einziger Schreiber davon)
//   net/wifi.cpp        schreibt net
//   net/ha_mqtt.cpp     liest alles, schreibt net.mqtt
//   ui/*                liest nur
// Geschrieben wird ausschliesslich zwischen model_lock()/model_unlock(),
// gelesen als Kopie ueber model_read(). Der Snapshot ist ~300 B — billiger als
// jede Feingranularitaet und immun gegen halb aktualisierte Zustaende (SOC neu,
// Strom noch alt) im Rendering.
//
// EINHEITEN — bewusst die des DALY/Pylontech-Protokolls, damit zwischen Lesen
// und CAN-Senden nichts umgerechnet und dabei verdreht wird:
//   pack_dv  10 mV   |  pack_ci  0.1 A (positiv = laden)  |  soc_pm  0.1 %
//   cell_mv  mV      |  rest_mah mAh                      |  *_ma    mA
#pragma once
#include <stdint.h>

inline constexpr int CELL_COUNT = 16;

// Frische eines Messwerts. Die Schwelle liegt im Modell (BMS_STALE_MS), die UI
// faerbt nur noch. offline = seit Start nie oder seit BMS_OFFLINE_MS nichts.
enum class Fresh : uint8_t { fresh, stale, offline };

// Farb-Zone eines Werts. none = Wert ohne Schwelle (Spannung, Temperatur) —
// bleibt neutral. Farbe kodiert Bedeutung, nie Dekoration.
enum class Zone : uint8_t { none, ok, low, critical };

// Energierichtung — kommt aus DALY-Register 0x93 Byte 0.
enum class Flow : uint8_t { idle, charge, discharge };

struct BmsData {
    Fresh    fresh;
    uint32_t updated_ms;        // esp_timer-Millisekunden des letzten Erfolgs
    uint16_t pack_dv;           // Packspannung, 10 mV
    int32_t  pack_ci;           // Packstrom, 0.1 A, positiv = laden
    uint16_t soc_pm;            // SOC, 0.1 %
    int16_t  temp_c;            // Zelltemperatur, °C
    uint16_t cell_max_mv, cell_min_mv;
    uint8_t  cell_max_no, cell_min_no;   // 1-basierte Zellnummer wie im BMS
    Flow     flow;
    bool     charge_mos, discharge_mos;
    uint32_t rest_mah;          // Restkapazitaet
    uint16_t cycles;
    uint16_t cell_mv[CELL_COUNT];
    bool     cells_valid;       // 0x95 wird nur alle CELL_READ_EVERY Zyklen gelesen
};

struct Limits {
    uint32_t charge_ma, discharge_ma;           // aktuell an den SOLIS gemeldet
    uint32_t charge_max_ma, discharge_max_ma;   // Obergrenzen (HA-einstellbar)
    uint32_t charge_mv;                         // Ladeschlussspannung
    bool     hv_block;      // Batterie voll -> Ladestrom 0 (BatHighVoltageFlag)
    bool     lv_block;      // Zelle am unteren Ende (BatEntladenFlag)
    bool     ramp_hold;     // Ladestrom darf nicht erhoeht werden
};

struct Diag {
    uint32_t err_count;         // BMS-Lesefehler seit Start
    char     last_error[80];
    uint32_t cycle_ms;          // Dauer eines BMS+CAN-Durchlaufs
    uint32_t uptime_s;
    uint32_t can_tx, can_err;
    bool     can_ok;
    bool     beep;              // Signalton bei Fehlern
    // Kurzmeldung fuer das Display ("MOSFET ein", "SOC gesetzt"). Wer eine
    // Aktion ausloest, sieht am Geraet, dass sie angekommen ist — auch wenn die
    // Wirkung erst im naechsten Lesedurchlauf sichtbar wird.
    char     notice[48];
    uint32_t notice_ms;
};

struct Net {
    bool wifi, mqtt;
    char ip[16];
    char ssid[33];
    int8_t rssi;
};

struct Model {
    BmsData bms;
    Limits  lim;
    Diag    diag;
    Net     net;
};

void   model_init();
Model *model_lock();          // fuer Schreiber; danach ZWINGEND model_unlock()
void   model_unlock();
void   model_read(Model *out);  // Leser-Snapshot
void   model_notice(const char *text);   // Kurzmeldung fuers Display

// ---- Kommandos an den BMS-Task -------------------------------------------
// UART und CAN gehoeren dem BMS-Task allein; UI und MQTT schicken nur Wuensche.
// Das erspart jede Sperre um die Schnittstellen und macht die Reihenfolge
// deterministisch (ein MOS-Aus mitten in einem 0x95-Lesevorgang gaebe sonst
// Frame-Salat).
enum class CmdType : uint8_t {
    mos_on, mos_off, set_soc, set_charge_max, set_discharge_max, restart
};
struct Cmd { CmdType type; int32_t arg; };

void cmd_init();
bool cmd_post(CmdType t, int32_t arg = 0);
bool cmd_take(Cmd *out, uint32_t wait_ms);

// ---- abgeleitete Bewertung (eine Quelle fuer UI und MQTT) -----------------
Zone zone_soc(uint16_t soc_pm);
Zone zone_cell_max(uint16_t mv);
Zone zone_cell_min(uint16_t mv);
Zone zone_temp(int16_t c);
const char *flow_text(Flow f);
