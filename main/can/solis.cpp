#include "solis.h"
#include "mcp2515.h"
#include "../app_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// Alle Mehrbytewerte im Pylontech-Protokoll sind little-endian.
static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

bool solis_send(const BmsData &bms, const Limits &lim)
{
    uint8_t m[8];
    bool ok = true;

    // 0x351 — Lade-/Entladegrenzen.
    // Einheiten: Spannung 0,1 V, Strom 0,1 A. Unsere Modelleinheiten sind mV
    // bzw. mA, deshalb durchgehend /100.
    memset(m, 0, sizeof m);
    put16(m + 0, (uint16_t)(lim.charge_mv / 100));
    put16(m + 2, (uint16_t)(lim.charge_ma / 100));
    put16(m + 4, (uint16_t)(lim.discharge_ma / 100));
    put16(m + 6, (uint16_t)(DISCHARGE_VOLTAGE_MV / 100));  // vom SOLIS ignoriert
    ok &= mcp2515_send(0x351, m, 8);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 0x355 — SOC/SOH in ganzen Prozent.
    // ⚠️ SOH ist fest 100: das DALY liefert keinen Wert dafuer. Ein gemeldeter
    // Verschleiss wuerde den SOLIS zu falschen Schluessen bringen.
    memset(m, 0, sizeof m);
    put16(m + 0, (uint16_t)(bms.soc_pm / 10));
    put16(m + 2, 100);
    ok &= mcp2515_send(0x355, m, 8);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 0x356 — Messwerte: Spannung 0,01 V, Strom 0,1 A (vorzeichenbehaftet),
    // Temperatur 0,1 °C.
    memset(m, 0, sizeof m);
    put16(m + 0, bms.pack_dv);
    put16(m + 2, (uint16_t)(int16_t)bms.pack_ci);
    put16(m + 4, (uint16_t)(int16_t)(bms.temp_c * 10));
    ok &= mcp2515_send(0x356, m, 8);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 0x35E — Herstellerkennung. Der SOLIS erwartet hier einen Pylontech-Namen,
    // sonst nimmt er die Batterie nicht an.
    static const uint8_t manu[8] = {'P', 'Y', 'L', 'O', 'N', 0, 0, 0};
    ok &= mcp2515_send(0x35E, manu, 8);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 0x359 — Fehler-/Warnbits. Wir melden nichts: die Schutzabschaltungen
    // laufen ueber die Stromgrenzen in 0x351 und das BMS selbst. Der Rahmen muss
    // trotzdem kommen, sonst haelt der SOLIS die Batterie fuer verschwunden.
    static const uint8_t fault[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    ok &= mcp2515_send(0x359, fault, 8);
    vTaskDelay(pdMS_TO_TICKS(5));

    return ok;
}
