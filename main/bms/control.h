// Lade-/Entladeregelung — 1:1-Port der im Feld eingestellten Logik.
//
// Der Adapter meldet dem SOLIS nicht einfach die BMS-Werte durch, sondern
// begrenzt den Ladestrom anhand der HOECHSTEN Zellspannung (und den Entladepfad
// anhand der niedrigsten). Grund: bei LiFePO4 laeuft die Packspannung bis kurz
// vor Schluss flach, waehrend einzelne Zellen schon davonlaufen. Wer nur auf die
// Packspannung regelt, ueberlaedt genau die staerkste Zelle.
//
// Zwei Eigenheiten, die beim Lesen sonst wie Fehler aussehen:
//  * Gemittelt wird ueber DREI Messungen (control_note_cells) — eine einzelne
//    Fehlmessung soll den Strom nicht wegreissen.
//  * Nach einem Absenken bleibt das Erhoehen fuer RAMP_HOLD_S gesperrt. Ohne
//    diese Sperre pumpt die Regelung, sobald eine Wolke ueber die Anlage zieht.
//
// Laeuft ausschliesslich im BMS-Task: die Funktionen schreiben ueber daly_*
// direkt auf den UART.
#pragma once
#include "../model.h"
#include <stddef.h>

void control_init();

// Drei-Messungen-Mittel fortschreiben. Nach jedem erfolgreichen 0x91 aufrufen.
void control_note_cells(uint16_t cell_min_mv, uint16_t cell_max_mv);

// Einen Regelschritt rechnen. Aendert lim.charge_ma/discharge_ma und die Flags;
// kalibriert bei Bedarf den SOC im BMS. note traegt danach ggf. eine
// Begruendung, warum der Strom auf 0 steht (geht als Klartext nach HA).
//
// ⚠️ bms ist NICHT const: eine SOC-Kalibrierung setzt bms.soc_pm sofort mit.
// Das ist keine Bequemlichkeit, sondern notwendig — Begruendung bei
// calibrate_soc() in control.cpp.
void control_step(BmsData &bms, Limits &lim, char *note, size_t note_cap);

// Gemittelte Zellspannungen — die Anzeige zeigt sie, weil die Regelung sie und
// nicht die Momentanwerte benutzt.
uint16_t control_cell_min_avg();
uint16_t control_cell_max_avg();
