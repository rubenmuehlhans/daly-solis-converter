#!/usr/bin/env python3
"""gen_vlw.py — TTF -> VLW (+ C-Header) fuer LovyanGFX' VLWfont-Loader.

Format EXAKT gegen die Load-Routine geschrieben (nicht geraten):
m5gfx 0.2.25, src/lgfx/v1/lgfx_fonts.cpp, VLWfont::loadFont:
  Header 24 B, u32 BIG-ENDIAN: [glyphCount, version, size, pad, ascent, descent]
  je Glyph 28 B, u32 BE:       [unicode, height, width, xAdvance, dY, dX, pad]
    dY = bitmap_top (Baseline -> Oberkante), dX = bitmap_left (int8!)
  danach Bitmaps: 8-bit Alpha, row-major, w*h Byte je Glyph, in Glyph-Reihenfolge.
  ⚠️ getUnicodeIndex nutzt std::lower_bound -> Glyphen MUESSEN nach Codepoint
  aufsteigend sortiert sein. Codepoints sind u16 (BMP only).
  Space (0x20) faellt sonst auf spaceWidth = yAdvance*2/7 zurueck -> wir nehmen
  ihn als echten Glyph auf (width 0, korrekter xAdvance) — bei Mono zaehlt das.

Der Loader legt nur die Metrik-Tabellen in den Heap (gCount * ~9 B je Font);
die Bitmaps bleiben im Flash und werden zur Zeichenzeit gelesen. Das ist der
Grund, warum das auch auf dem Core Basic OHNE PSRAM traegt.

Subset: Basic Latin + AeOeUeaeoeuess + ° + · + Em-Dash. ⚠️ Fehlt ein Zeichen im
Font, bricht der Generator HART ab (kein stiller Ausfall) — gewollt: ein
fehlendes '°' faellt am Geraet sonst erst im Betrieb auf.

Variable Fonts (Manrope[wght].ttf) werden vor dem Rendern mit fontTools zu
einer statischen Instanz eingefroren.

Nutzung:  ~/panelvenv/bin/python3 tools/gen_vlw.py
"""
import io
import struct
import sys
from pathlib import Path

import freetype
from fontTools import ttLib
from fontTools.varLib import instancer

HERE = Path(__file__).resolve().parent
FW = HERE.parent
FONTS = FW / "assets" / "fonts"
OUT_H = FW / "main" / "gen" / "fonts_vlw.h"

CODEPOINTS = sorted(
    set(range(0x20, 0x7F))
    | {0xC4, 0xD6, 0xDC, 0xDF, 0xE4, 0xF6, 0xFC,   # ÄÖÜßäöü
       0xB0, 0xB7, 0x2014}                          # ° · —
)

# (C-Name, Datei, wght-Achse oder None, Pixelgroesse)
JOBS = [
    ("vlw_mono44", "JetBrainsMono-Medium.ttf", None, 44),   # Hero-Wert (SOC)
    ("vlw_mono38", "JetBrainsMono-Medium.ttf", None, 38),   # Kachel-Werte
    ("vlw_mono20", "JetBrainsMono-Medium.ttf", None, 20),   # Einheiten, Uhr
    ("vlw_sans16", "Manrope[wght].ttf", 600, 16),           # Labels (SemiBold)
    ("vlw_sans12", "Manrope[wght].ttf", 600, 12),           # Zellen-Diagramm
]


def load_face(path: Path, wght):
    """TTF laden; Variable Font vorher zu statischer Instanz einfrieren."""
    if wght is not None:
        tt = ttLib.TTFont(path)
        if "fvar" in tt:
            instancer.instantiateVariableFont(tt, {"wght": wght}, inplace=True)
        buf = io.BytesIO()
        tt.save(buf)
        return freetype.Face(io.BytesIO(buf.getvalue()))
    return freetype.Face(str(path))


def build_vlw(face: freetype.Face, px: int) -> bytes:
    face.set_pixel_sizes(0, px)
    ascent = face.size.ascender >> 6
    descent = abs(face.size.descender >> 6)

    glyphs = []  # (cp, h, w, adv, dy, dx, bitmap)
    missing = []
    for cp in CODEPOINTS:
        if face.get_char_index(cp) == 0:
            missing.append(cp)
            continue
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        # bm.buffer beachtet pitch: Zeilen koennen gepaddet sein -> dicht packen
        rows = bytearray()
        for y in range(h):
            start = y * bm.pitch
            rows += bytes(bm.buffer[start:start + w])
        adv = g.advance.x >> 6
        glyphs.append((cp, h, w, adv, g.bitmap_top, g.bitmap_left, bytes(rows)))
    if missing:
        pretty = ", ".join(f"U+{c:04X}" for c in missing)
        sys.exit(f"FEHLER: Font hat keine Glyphe fuer: {pretty}")

    glyphs.sort(key=lambda t: t[0])          # lower_bound-Voraussetzung
    out = bytearray()
    out += struct.pack(">6i", len(glyphs), 11, px, 0, ascent, descent)
    for cp, h, w, adv, dy, dx, _ in glyphs:
        if not (-128 <= dx <= 127):
            sys.exit(f"FEHLER: dX={dx} von U+{cp:04X} passt nicht in int8")
        out += struct.pack(">7i", cp, h, w, adv, dy, dx, 0)
    for *_, bitmap in glyphs:
        out += bitmap
    return bytes(out), ascent, descent


def main():
    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// GENERIERT von tools/gen_vlw.py — NICHT von Hand editieren.",
        "// VLW-Format gegen VLWfont::loadFont (m5gfx 0.2.25) geschrieben.",
        "// Fonts: JetBrains Mono Medium + Manrope SemiBold (beide SIL OFL 1.1,",
        "// Lizenztexte in assets/fonts/ — OFL erlaubt Einbettung; die Fonts",
        "// selbst bleiben OFL, das Produkt wird dadurch nicht OFL).",
        "//",
        "// ascent/descent je Font stehen als Kommentar dabei: verschieden grosse",
        "// Fonts fluchten NICHT ueber bottom_left (die Glyphenbox enthaelt den",
        "// Descender) — tiles.cpp rechnet die Differenz als *_DY heraus.",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    total = 0
    for name, fname, wght, px in JOBS:
        face = load_face(FONTS / fname, wght)
        blob, ascent, descent = build_vlw(face, px)
        (FONTS / f"{name}.vlw").write_bytes(blob)   # fuers Debuggen am Desktop
        total += len(blob)
        lines.append(f"// {fname} @{px}px{f' wght={wght}' if wght else ''}"
                     f" — {len(blob)} B, {len(CODEPOINTS)} Codepoints,"
                     f" ascent={ascent} descent={descent}")
        lines.append(f"constexpr uint8_t {name}[{len(blob)}] = {{")
        for i in range(0, len(blob), 24):
            chunk = ",".join(str(b) for b in blob[i:i + 24])
            lines.append(f"  {chunk},")
        lines.append("};")
        lines.append("")
        print(f"{name}: {len(blob):6d} B  ({fname} @{px}px, "
              f"ascent={ascent} descent={descent})")
    OUT_H.write_text("\n".join(lines), encoding="utf-8")
    print(f"geschrieben: {OUT_H.relative_to(FW)}  (gesamt {total} B)")


if __name__ == "__main__":
    main()
