// Seiten und Bedienung ueber die drei Hardware-Tasten.
//
//   A  Seite       — blaettert 0..3
//   B  Nacht/Tag   — kurz: Palette umschalten;  lang (1 s): Signalton an/aus
//   C  MOS         — 1,5 s HALTEN schaltet die MOSFETs. Bewusst kein einzelner
//                    Druck: das trennt die Hausbatterie vom Wechselrichter,
//                    und der Knopf sitzt in Griffhoehe.
#pragma once
#include <stdint.h>

inline constexpr int PAGE_COUNT = 4;

void pages_init();
void pages_show(int idx);      // Vollaufbau
void pages_tick(uint32_t ms);  // Tasten + Wertaktualisierung
