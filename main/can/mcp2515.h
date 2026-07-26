// MCP2515 auf dem M5Stack-COMMU-Modul — nur senden.
//
// Der Adapter ist am CAN reiner Sprecher: der SOLIS erwartet den
// Pylontech-Datensatz im Sekundentakt und antwortet nicht. Deshalb gibt es hier
// keinen Empfangspfad, keine Filter und keinen Interrupt — CAN_INT (GPIO15)
// bleibt unbenutzt.
//
// ⚠️ Der Baustein haengt am selben SPI-Bus wie Display und SD-Karte. Den Bus
// richtet M5.begin() ein; hier wird nur ein weiteres Geraet angemeldet, also
// ERST nach M5.begin() aufrufen.
//
// ⚠️ CS wird MANUELL per GPIO gezogen, nicht ueber die Hardware-CS des
// SPI-Controllers — M5GFX schaltet die naemlich an der IDF vorbei ab.
// Ausfuehrliche Begruendung in mcp2515.cpp. mcp2515_init() ist deshalb auch
// jederzeit wiederholbar: bleibt das Modul beim Start stumm, versucht es der
// BMS-Task alle 10 s erneut.
//
// (Nebenbei: M5GFX konfiguriert den Bus mit max_transfer_sz = 1, weil es seine
// Pixel selbst schreibt — wirksam sind daraus trotzdem 4092 B, weil der Treiber
// auf einen vollen DMA-Deskriptor aufrundet. Sonst waere hier bei 2 B Schluss.)
#pragma once
#include <stdint.h>

bool mcp2515_init();                                  // 500 kbit/s, 8-MHz-Quarz
bool mcp2515_send(uint16_t sid, const uint8_t *data, uint8_t len);
bool mcp2515_alive();     // CANCTRL noch im Normal-Mode? (erkennt Reset/Abzug)
