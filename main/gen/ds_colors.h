// GENERIERT von tools/gen_ds_colors.py — NICHT von Hand editieren.
// Quelle ist die OKLCH-Palette im Generator selbst.
// Verifikation der oklch->sRGB-Konvertierung: tools/verify_colors.html
// (Canvas-Pixel aus Chrome als Referenz, Toleranz +-1/Kanal).
#pragma once
#include <stdint.h>

namespace ds {
constexpr uint16_t background        = 0x0082;  // oklch(0.17 0.018 231.0) -> rgb(7,17,22)
constexpr uint16_t card              = 0x08C4;  // oklch(0.21 0.02 229.0) -> rgb(14,26,32)
constexpr uint16_t card_hi           = 0x1105;  // oklch(0.245 0.022 229.0) -> rgb(21,35,41)
constexpr uint16_t foreground        = 0xEF7E;  // oklch(0.95 0.006 225.0) -> rgb(234,239,241)
constexpr uint16_t muted             = 0x9514;  // oklch(0.7 0.015 225.0) -> rgb(149,161,165)
constexpr uint16_t border            = 0x2187;  // oklch(0.31 0.02 229.0) -> rgb(38,50,56)
constexpr uint16_t track             = 0x1945;  // oklch(0.27 0.02 229.0) -> rgb(28,40,46)
constexpr uint16_t primary           = 0x45D7;  // oklch(0.72 0.105 196.0) -> rgb(64,185,186)
constexpr uint16_t accent            = 0xED48;  // oklch(0.78 0.14 74.0) -> rgb(236,168,66)
constexpr uint16_t charge            = 0x55EE;  // oklch(0.72 0.15 150.0) -> rgb(83,190,112)
constexpr uint16_t discharge         = 0x4D9C;  // oklch(0.72 0.12 235.0) -> rgb(76,176,229)
constexpr uint16_t idle              = 0x9514;  // oklch(0.7 0.015 225.0) -> rgb(149,161,165)
constexpr uint16_t warn              = 0xF569;  // oklch(0.8 0.14 74.0) -> rgb(243,175,73)
constexpr uint16_t crit              = 0xF228;  // oklch(0.64 0.21 25.0) -> rgb(241,68,69)
constexpr uint16_t offline           = 0x7C10;  // oklch(0.6 0.012 235.0) -> rgb(122,130,134)
constexpr uint16_t night_bg          = 0x0000;  // oklch(0.0 0.0 0.0) -> rgb(0,0,0)
constexpr uint16_t night_card        = 0x0000;  // oklch(0.0 0.0 0.0) -> rgb(0,0,0)
constexpr uint16_t night_border      = 0x4120;  // oklch(0.3 0.07 74.0) -> rgb(66,39,0)
constexpr uint16_t night_border_hd   = 0x5180;  // oklch(0.34 0.08 74.0) -> rgb(80,48,0)
constexpr uint16_t night_track       = 0x30C0;  // oklch(0.24 0.06 74.0) -> rgb(48,26,0)
constexpr uint16_t night_muted       = 0x82C3;  // oklch(0.5 0.09 74.0) -> rgb(130,91,31)
constexpr uint16_t night_value       = 0xABC5;  // oklch(0.62 0.11 74.0) -> rgb(173,123,47)
constexpr uint16_t night_unit        = 0x6A43;  // oklch(0.44 0.08 74.0) -> rgb(109,75,24)
constexpr uint16_t night_primary     = 0xCC86;  // oklch(0.7 0.13 74.0) -> rgb(206,145,49)
constexpr uint16_t night_crit_border = 0xED48;  // oklch(0.78 0.14 74.0) -> rgb(236,168,66)
constexpr uint16_t night_crit_value  = 0xFDC9;  // oklch(0.84 0.15 74.0) -> rgb(255,186,74)
}  // namespace ds
