#!/usr/bin/env python3
"""gen_icons.py — Zustands-Icons als 8-Bit-Graustufenmasken fuer das Panel.

Erzeugt main/gen/ds_icons.h: Alpha-Masken (0=Hintergrund, 255=Vordergrund) fuer
pushGrayscaleImage(..., grayscale_8bit, forecolor, backcolor) — die Farbe kommt
zur LAUFZEIT, ein Asset bedient gruen/amber/rot. Deshalb gibt es kein
"ic_warn_red": Icon und Farbe sind getrennt, das Icon traegt die Form.

⚠️ backcolor MUSS die tatsaechliche Hintergrundfarbe sein: pushGrayscaleImage
interpoliert fore<->back, das ist KEIN echtes Alpha. Ueber einer Kachel also
p.card, ueber der Statuszeile p.bg.

Bewusst 8 Bit statt 4: die Nibble-Reihenfolge von grayscale_4bit ist nicht
verifiziert; bei 8 Icons a 324 B ist die Ersparnis irrelevant.

Gezeichnet 8x supersampled (PIL), LANCZOS-downscale -> echtes Antialiasing.

Nutzung:  ~/panelvenv/bin/python3 tools/gen_icons.py
          ~/panelvenv/bin/python3 tools/gen_icons.py --png   # + Vorschau-PNGs
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
OUT_H = HERE.parent / "main" / "gen" / "ds_icons.h"
SIZE = 18          # Zielgroesse in px (die Label-Zeile ist 16 px Text)
SS = 8             # Supersampling-Faktor


def canvas():
    img = Image.new("L", (SIZE * SS, SIZE * SS), 0)
    return img, ImageDraw.Draw(img)


def icon_warn():
    """Dreieck mit Ausrufezeichen — Grenzwert erreicht."""
    img, d = canvas()
    s = SIZE * SS
    m = s * 0.06
    pts = [(s / 2, m), (s - m, s - m * 1.6), (m, s - m * 1.6)]
    d.polygon(pts, fill=255)
    d.line(pts + [pts[0]], fill=255, width=int(s * 0.10), joint="curve")
    # Ausrufezeichen ausstanzen — kraeftig, sonst bei 18 px unsichtbar
    bw = s * 0.115
    d.rounded_rectangle([s / 2 - bw, s * 0.28, s / 2 + bw, s * 0.60],
                        radius=bw, fill=0)
    r = s * 0.10
    d.ellipse([s / 2 - r, s * 0.73 - r, s / 2 + r, s * 0.73 + r], fill=0)
    return img


def icon_stale():
    """Uhr — Wert veraltet (BMS antwortet, aber der Wert ist alt)."""
    img, d = canvas()
    s = SIZE * SS
    m = s * 0.08
    w = int(s * 0.11)
    d.ellipse([m, m, s - m, s - m], outline=255, width=w)
    cx = cy = s / 2
    d.line([cx, cy, cx, s * 0.24], fill=255, width=w)
    d.line([cx, cy, s * 0.68, s * 0.66], fill=255, width=w)
    return img


def icon_offline():
    """Stecker gezogen — keine Verbindung zum BMS/Bus."""
    img, d = canvas()
    s = SIZE * SS
    w = int(s * 0.10)
    cx, cy = s * 0.5, s * 0.82
    for r in (s * 0.16, s * 0.34, s * 0.52):
        d.arc([cx - r, cy - r, cx + r, cy + r], start=215, end=325,
              fill=255, width=w)
    dot = s * 0.07
    d.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=255)
    # Schraegstrich: erst ausstanzen, dann duenner neu ziehen -> bleibt lesbar
    d.line([s * 0.16, s * 0.10, s * 0.86, s * 0.88], fill=0, width=int(s * 0.16))
    d.line([s * 0.20, s * 0.10, s * 0.90, s * 0.88], fill=255, width=w)
    return img


def icon_charge():
    """Blitz — Strom fliesst IN die Batterie."""
    img, d = canvas()
    s = SIZE * SS
    pts = [(s * 0.60, s * 0.05), (s * 0.24, s * 0.55), (s * 0.46, s * 0.55),
           (s * 0.38, s * 0.95), (s * 0.78, s * 0.42), (s * 0.54, s * 0.42),
           (s * 0.64, s * 0.05)]
    d.polygon(pts, fill=255)
    d.line(pts + [pts[0]], fill=255, width=int(s * 0.06), joint="curve")
    return img


def icon_discharge():
    """Pfeil nach unten — Strom wird entnommen."""
    img, d = canvas()
    s = SIZE * SS
    w = int(s * 0.18)
    d.line([s * 0.5, s * 0.10, s * 0.5, s * 0.72], fill=255, width=w)
    d.polygon([(s * 0.5, s * 0.95), (s * 0.16, s * 0.52), (s * 0.84, s * 0.52)],
              fill=255)
    return img


def icon_idle():
    """Zwei Balken (Pause) — weder Laden noch Entladen."""
    img, d = canvas()
    s = SIZE * SS
    bw = s * 0.14
    r = bw / 2
    for cx in (s * 0.34, s * 0.66):
        d.rounded_rectangle([cx - bw, s * 0.18, cx + bw, s * 0.82],
                            radius=r, fill=255)
    return img


def icon_temp():
    """Thermometer — Zelltemperatur."""
    img, d = canvas()
    s = SIZE * SS
    w = int(s * 0.10)
    cx = s * 0.5
    bulb = s * 0.17
    d.rounded_rectangle([cx - s * 0.11, s * 0.08, cx + s * 0.11, s * 0.70],
                        radius=s * 0.11, outline=255, width=w)
    d.ellipse([cx - bulb, s * 0.62, cx + bulb, s * 0.62 + 2 * bulb],
              outline=255, width=w)
    d.ellipse([cx - bulb * 0.55, s * 0.62 + bulb * 0.45,
               cx + bulb * 0.55, s * 0.62 + bulb * 1.55], fill=255)
    d.line([cx, s * 0.30, cx, s * 0.70], fill=255, width=int(s * 0.09))
    return img


def icon_power():
    """Ein/Aus-Symbol — Zustand der MOSFETs im BMS."""
    img, d = canvas()
    s = SIZE * SS
    w = int(s * 0.13)
    m = s * 0.14
    # Ring mit Luecke oben: als Bogen von 300deg ueber unten nach 240deg
    d.arc([m, m, s - m, s - m], start=-60, end=240, fill=255, width=w)
    d.line([s * 0.5, s * 0.05, s * 0.5, s * 0.46], fill=255, width=w)
    return img


ICONS = {
    "ic_warn":      icon_warn,
    "ic_stale":     icon_stale,
    "ic_offline":   icon_offline,
    "ic_charge":    icon_charge,
    "ic_discharge": icon_discharge,
    "ic_idle":      icon_idle,
    "ic_temp":      icon_temp,
    "ic_power":     icon_power,
}


def main():
    out = [
        "// GENERIERT von tools/gen_icons.py — NICHT von Hand editieren.",
        "// 8-Bit-Alpha-Masken fuer pushGrayscaleImage(grayscale_8bit, fore, back):",
        "// Farbe kommt zur Laufzeit, backcolor MUSS die echte Hintergrundfarbe",
        "// sein (Interpolation fore<->back, kein echtes Alpha).",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"constexpr int DS_ICON_W = {SIZE};",
        f"constexpr int DS_ICON_H = {SIZE};",
        "",
    ]
    for name, fn in ICONS.items():
        img = fn().resize((SIZE, SIZE), Image.LANCZOS)
        if "--png" in sys.argv:
            p = HERE / f"preview_{name}.png"
            img.resize((SIZE * 8, SIZE * 8), Image.NEAREST).save(p)
            print(f"Vorschau: {p.name}")
        data = list(img.getdata())
        out.append(f"constexpr uint8_t {name}[{SIZE * SIZE}] = {{")
        for y in range(SIZE):
            row = data[y * SIZE:(y + 1) * SIZE]
            out.append("  " + ",".join(f"0x{v:02X}" for v in row) + ",")
        out.append("};")
        out.append("")
    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text("\n".join(out), encoding="utf-8")
    print(f"geschrieben: {OUT_H} ({len(ICONS)} Icons, {SIZE}x{SIZE}, 8 bit)")


if __name__ == "__main__":
    main()
