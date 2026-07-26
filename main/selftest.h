// Bildschirm-Abzug ueber die serielle Konsole — nur fuer den Gerätetest.
//
// Wird nur uebersetzt, wenn SOLIS_SELFTEST=1 gesetzt ist (main/CMakeLists.txt).
// Im Normalbetrieb ist die Datei nicht im Bild: der Dump haelt die Anzeige
// ueber eine Minute lang fest und schreibt ~1 MB in die Konsole.
//
// Gegenstueck am Rechner: tools/grab_screens.py
#pragma once

// Spielt plausible Messwerte ins Modell und dumpt alle Seiten.
// ⚠️ Der BMS-Task darf dabei NICHT laufen — er wuerde die eingespielten Werte
// im Sekundentakt ueberschreiben.
void selftest_run();
