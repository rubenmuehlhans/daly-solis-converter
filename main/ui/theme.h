// Layout + Farbpaletten des Panels.
//
// Farbwerte kommen ausschliesslich aus dem generierten ds_colors.h (Quelle ist
// tools/gen_ds_colors.py). Hier steht nur, WELCHE Rolle welchen Token bekommt —
// so bleibt ein Rebrand eine Generator-Sache und keine Suche im C-Code.
#pragma once
#include <stdint.h>

// ---- Layout (M5Stack Core Basic, 320x240) --------------------------------
// Die Fussleiste beschriftet die drei ECHTEN Tasten unter dem Display. Ihre
// Mitten liegen bei ~60/160/260 px — die Beschriftung steht genau darueber,
// sonst greift man daneben.
namespace lay {
constexpr int W = 320, H = 240;
constexpr int STATUS_H = 28;
constexpr int BAR_H    = 26;
constexpr int GRID_Y   = STATUS_H;
constexpr int GRID_H   = H - STATUS_H - BAR_H;          // 186
constexpr int PAD = 8, GAP = 8;
constexpr int TILE_W = (W - 2 * PAD - GAP) / 2;         // 148
constexpr int TILE_H = (GRID_H - 2 * PAD - GAP) / 2;    // 81
constexpr int HERO_W = TILE_W;
constexpr int HERO_H = 2 * TILE_H + GAP;                // 170
constexpr int RADIUS = 10;
constexpr int TILE_PAD_X = 9, TILE_PAD_Y = 7;

constexpr int tile_x(int col) { return PAD + col * (TILE_W + GAP); }
constexpr int tile_y(int row) { return GRID_Y + PAD + row * (TILE_H + GAP); }

// Mitten der Hardware-Tasten A/B/C
constexpr int BTN_CX[3] = {60, 160, 260};
constexpr int BAR_Y = H - BAR_H;
}

// ---- Palette -------------------------------------------------------------
// Eine Struktur fuer beide Modi. ⚠️ Nacht ist NICHT dieselbe Palette dunkler:
// nach RGB565 verschmelzen Karte und Grund beim Dimmen. Im Amber-Monochrom
// tragen Helligkeit, Rahmen und Icon die Bedeutung — deshalb zeigen dort
// charge/discharge/idle auf denselben Wert.
struct Palette {
    uint16_t bg, card, card_hi, border, border_hd, track;
    uint16_t label, value, unit;
    uint16_t primary, accent;
    uint16_t charge, discharge, idle;
    uint16_t warn, crit, crit_border, offline;
};

enum class ThemeMode : uint8_t { day, night };

const Palette &theme_pal();
ThemeMode      theme_mode();
void           theme_set(ThemeMode m);   // setzt Palette UND Hintergrundlicht
void           theme_toggle();
