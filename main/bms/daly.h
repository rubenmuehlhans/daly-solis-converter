// DALY/Deligreen Smart BMS ueber UART (9600 8N1, 13-Byte-Frames).
//
// Direkter Port der erprobten Routinen aus dem Arduino-Original — inklusive der
// Wartezeiten. Die sind NICHT willkuerlich: das BMS antwortet traege und liefert
// beim Zellen-Lesen (0x95) sechs Frames hintereinander; kuerzere Pausen haben
// im Feld Lesefehler erzeugt.
//
// Alle Funktionen hier laufen ausschliesslich im BMS-Task (bms/daly_task.cpp) —
// der UART hat keinen eigenen Schutz.
#pragma once
#include "../model.h"
#include <stddef.h>

void daly_init();

// Ein vollstaendiger Lesedurchlauf (0x90..0x94, optional 0x95).
// out wird nur bei true als gueltig betrachtet; err traegt sonst die Fundstelle
// ("at send CMD 91") wie bisher — dieser String geht 1:1 nach Home Assistant.
//
// got_minmax meldet, ob 0x91 (hoechste/niedrigste Zelle) durchkam — auch dann,
// wenn ein spaeteres Kommando scheitert. Der Aufrufer muss den Wert trotzdem in
// die Mittelung geben: die Regelung haengt an diesem Dreier-Fenster, und im
// Original lief die Akkumulation ebenfalls im 0x91-Zweig und damit unabhaengig
// vom Ausgang des restlichen Durchlaufs.
bool daly_read(BmsData *out, bool with_cells, char *err, size_t err_cap,
               bool *got_minmax = nullptr);

// Schreibende Kommandos.
void daly_set_soc(uint8_t percent);   // 0x21 — SOC im BMS nachkalibrieren
void daly_mos(bool on);               // 0xD9/0xDA — Entlade- und Lade-MOSFET
