// Kachel- und Hero-Rendering.
//
// ALLES geht durch EINEN wiederverwendeten Sprite im internen RAM
// (148x170 ≈ 50 KB). Gruende:
//  * Ein Vollbild-Sprite waere 153,6 KB — auf dem Core Basic ohne PSRAM neben
//    WLAN und MQTT nicht zu haben.
//  * Im Sprite gerendert und einmal gepusht heisst: kein sichtbarer Aufbau,
//    kein Flackern. Direkt auf das Display gezeichnet sieht man jede Linie.
// Der Sprite ist so gross wie die Hero-Kachel; eine normale Kachel benutzt nur
// den oberen Teil und wird mit Clip-Rechteck gepusht.
#pragma once
#include "../model.h"
#include <stdint.h>

// Fonts (nach ui_fonts_init gueltig) — pages/screens nutzen sie mit.
namespace lgfx { inline namespace v1 { struct IFont; } }
const lgfx::IFont *font_mono44();
const lgfx::IFont *font_mono38();
const lgfx::IFont *font_mono20();
const lgfx::IFont *font_sans16();
const lgfx::IFont *font_sans12();

void ui_fonts_init();   // VLW-Fonts EINMAL laden; setFont() ist danach billig
void tiles_init();      // Sprite anlegen — vor dem ersten Zeichnen

struct TileSpec {
    const char    *label;
    const char    *value;
    const char    *unit;      // "" erlaubt
    Zone           zone;
    Fresh          fresh;
    const uint8_t *icon;      // 18x18-Maske aus ds_icons.h oder nullptr
};

struct HeroSpec {
    const char    *label;
    const char    *value;
    const char    *unit;
    int            fill_pm;   // Balkenfuellung 0..1000
    Zone           zone;
    Fresh          fresh;
    const uint8_t *icon;
    uint16_t       accent;    // Balken-/Icon-Farbe (Fluss)
    const char    *line1;     // z. B. "Laden · 640 W"
    const char    *line2;     // z. B. "47,2 Ah"
};

void tile_draw(int col, int row, const TileSpec &t);
void tile_update_value(int col, int row, const TileSpec &t);  // nur Wertbereich
void hero_draw(const HeroSpec &h);
