// Feste Verdrahtung + Regelparameter + Laufzeit-Konfiguration.
//
// Die Zahlen unter "Regelparameter" sind im Feld eingestellt worden (Original
// src/main.cpp, Kommentare dort). Sie sind hier zusammengezogen, damit man sie
// an EINER Stelle nachjustiert — die Logik in bms/control.cpp enthaelt keine
// magischen Konstanten mehr.
#pragma once
#include <stdint.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/uart.h>

// ---- Verdrahtung M5Stack Core Basic + COMMU-Modul -------------------------
// ⚠️ Der DALY-UART liegt auf Pin 2 (TX) / 5 (RX). Der DALY sendet 5-V-TTL, der
// ESP32 vertraegt 3,3 V — ohne Pegelbegrenzung (Z-Diode + Widerstand, siehe
// README) nimmt der M5Stack Schaden. Das ist Hardware, kein Firmware-Problem,
// aber es steht hier, weil es beim Nachbauen genau hier gesucht wird.
inline constexpr uart_port_t BMS_UART      = UART_NUM_1;
inline constexpr gpio_num_t  BMS_PIN_RX    = GPIO_NUM_5;
inline constexpr gpio_num_t  BMS_PIN_TX    = GPIO_NUM_2;
inline constexpr int         BMS_BAUD      = 9600;

// MCP2515 auf dem COMMU-Modul haengt am selben SPI-Bus wie Display und
// SD-Karte. Den Bus richtet M5Unified in M5.begin() ein; CAN und SD melden sich
// danach nur noch als weitere Geraete an (spi_bus_add_device bzw. sdspi). Die
// Arbitrierung macht der IDF-Treiber — M5GFX nimmt sich den Bus zum Zeichnen
// ueber spi_device_acquire_bus und gibt ihn danach wieder frei.
inline constexpr spi_host_device_t SPI_BUS = SPI3_HOST;   // VSPI
inline constexpr gpio_num_t  PIN_SPI_SCLK  = GPIO_NUM_18;
inline constexpr gpio_num_t  PIN_SPI_MISO  = GPIO_NUM_19;
inline constexpr gpio_num_t  PIN_SPI_MOSI  = GPIO_NUM_23;
inline constexpr gpio_num_t  PIN_CAN_CS    = GPIO_NUM_12;
inline constexpr gpio_num_t  PIN_SD_CS     = GPIO_NUM_4;

// ---- DALY-Protokoll -------------------------------------------------------
inline constexpr uint8_t  BMS_HOST_ADDR  = 0x40;  // Doku sagt 0x80, die PC-Software nutzt 0x40
inline constexpr uint8_t  BMS_SLAVE_ADDR = 0x01;
inline constexpr uint32_t BMS_RX_TIMEOUT_MS = 2000;
inline constexpr int      CELL_READ_EVERY   = 10;  // Einzelzellen nur jeden n-ten Zyklus (0x95 dauert ~1,5 s)

// Frische-Schwellen fuer die Anzeige (der Zyklus laeuft im Sekundentakt).
inline constexpr uint32_t BMS_STALE_MS   = 5000;
inline constexpr uint32_t BMS_OFFLINE_MS = 15000;

// ---- Regelparameter (Feldwerte aus dem Original) --------------------------
inline constexpr uint32_t CHARGE_VOLTAGE_MV        = 57600;   // Ladeschluss (3,45 V/Zelle)
inline constexpr uint32_t DISCHARGE_VOLTAGE_MV     = 48000;   // vom SOLIS derzeit ignoriert
inline constexpr uint32_t CHARGE_CURRENT_INIT_MA   = 5000;
inline constexpr uint32_t CHARGE_CURRENT_MAX_MA    = 90000;
inline constexpr uint32_t DISCHARGE_CURRENT_MAX_MA = 100000;

inline constexpr uint16_t CELL_MIN_ALLOWED_MV = 2700;  // Entladeende
inline constexpr uint16_t CELL_MAX_ALLOWED_MV = 3450;  // ~90 % SOC

inline constexpr uint16_t CELL_FULL_MV        = 3460;  // -> SOC auf 100 % kalibrieren
inline constexpr uint16_t CELL_HARD_STOP_MV   = 3480;  // -> Ladestrom sofort 0
inline constexpr uint16_t CELL_RESUME_MV      = 3350;  // -> Laden wieder freigeben
inline constexpr uint16_t CELL_RAMP_DOWN_MV   = 3410;  // -> Ladestrom Schritt fuer Schritt senken
inline constexpr uint16_t CELL_RAMP_20A_MV    = 3430;  // -> auf 20 A deckeln
inline constexpr uint16_t CELL_RAMP_FREE_MV   = 3320;  // darunter darf wieder erhoeht werden
inline constexpr uint16_t CELL_DISCHARGE_OK_MV = 3050; // Entladesperre aufheben

inline constexpr uint32_t RAMP_INTERVAL_MS  = 5000;    // Takt der Ladestromanpassung
inline constexpr uint32_t RAMP_HOLD_S       = 600;     // Sperre nach Absenken (10 min)

// Plausibilitaet: ausserhalb dieser Fenster gelten die BMS-Daten als kaputt
// und es geht NICHTS an den Wechselrichter.
inline constexpr int32_t  PLAUS_CURRENT_MIN_CI = -1100;   // -110 A
inline constexpr int32_t  PLAUS_CURRENT_MAX_CI =  1100;   // +110 A
inline constexpr uint16_t PLAUS_PACK_MIN_DV    = 4700;    // 47,0 V
inline constexpr uint16_t PLAUS_PACK_MAX_DV    = 5600;    // 56,0 V

// ---- Home Assistant -------------------------------------------------------
// ⚠️ Diese MAC ist die Identitaet des Geraets in Home Assistant: sie bildet die
// Device-ID "0010fa6e384a" in allen Topics. Aendern = alle Entities in HA sind
// neu, Historie weg. Deshalb steht sie hier fest und wird NICHT aus der echten
// WLAN-MAC abgeleitet.
inline constexpr uint8_t HA_DEVICE_MAC[6] = {0x00, 0x10, 0xFA, 0x6E, 0x38, 0x4A};

// ---- Laufzeit-Konfiguration ----------------------------------------------
// Quelle ist /config.txt auf der SD-Karte (Format wie bisher: key=value).
// Erfolgreich gelesene Werte landen zusaetzlich in NVS, damit das Geraet auch
// ohne Karte hochkommt — die Karte ist damit Konfigurationsweg, nicht
// Betriebsvoraussetzung.
struct AppConfig {
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_host[64];
    char mqtt_user[33];
    char mqtt_pass[65];
    uint16_t mqtt_port;
    bool from_sd;      // true = kam in diesem Boot von der Karte
    bool valid;        // mindestens SSID vorhanden
};

const AppConfig &config_get();
void config_load();          // SD -> NVS; ohne Karte NVS
