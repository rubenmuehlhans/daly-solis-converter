// Vollbild- und Overlay-Screens ausserhalb der vier Seiten.
#pragma once
#include <stdint.h>

// Startbildschirm: Titel + laufender Schritt ("CAN-Bus", "WLAN", …).
void screen_boot(const char *step, bool fail = false);

// Firmware-Update. Blockiert die Seiten, solange es laeuft.
void screen_ota(int percent);

// Halte-Bestaetigung fuer die MOSFETs. Wird bei jedem Tick aufgerufen; die
// Karte selbst wird nur einmal gezeichnet, danach nur noch der Fortschritt.
void screen_confirm_mos(bool turning_on, uint32_t held_ms, uint32_t need_ms);

// Kurzmeldung ueber der Fussleiste.
void screen_toast(const char *text);

// Alle Overlays vergessen lassen, was sie zuletzt gezeichnet haben.
// ⚠️ Nach JEDEM Vollaufbau einer Seite noetig: die Screens zeichnen ihren
// unveraenderlichen Teil nur einmal und merken sich das. Ohne dieses
// Zuruecksetzen kaeme beim zweiten Oeffnen nur noch der Fortschrittsbalken —
// die Karte darunter waere von der Seite ueberschrieben.
void screens_invalidate();
