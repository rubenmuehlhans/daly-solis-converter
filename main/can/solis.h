// Pylontech-kompatibler Datensatz fuer den SOLIS-Hybrid.
//
// Der Wechselrichter muss auf "User Battery" stehen; die hier gesendeten Grenzen
// ueberschreiben die im Geraet hinterlegten, sofern sie kleiner sind. Die
// Ladeschlussspannungen im SOLIS muessen hoch genug eingestellt sein, damit
// dieser Adapter das Laden beendet und nicht der Wechselrichter.
//
// Getestet gegen SOLIS RHI-4.6K-48ES-5G (Modell F2). Die CAN-Routinen stammen
// urspruenglich aus einem Victron-Projekt (VEcan) — fuer Victron waere die
// Busgeschwindigkeit auf 250 kbit/s zu aendern.
#pragma once
#include "../model.h"

// Sendet 0x351/0x355/0x356/0x35E/0x359. Rueckgabe false, sobald ein Rahmen
// nicht abgesetzt werden konnte.
bool solis_send(const BmsData &bms, const Limits &lim);
