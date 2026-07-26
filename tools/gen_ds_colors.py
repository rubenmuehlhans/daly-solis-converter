#!/usr/bin/env python3
"""gen_ds_colors.py — Palette -> RGB565-Header fuer das SolisM5-Panel.

Die Palette steht HIER (dieses Repo hat kein Design-System als Quelle). Sie ist
in OKLCH notiert, nicht in Hex: L/C/H sind die drei Groessen, an denen man beim
Feintunen wirklich dreht (Helligkeit getrennt von Buntheit getrennt vom Farbton).
Farbe aendern -> Werte unten anfassen -> Skript laufen lassen -> Firmware zieht
nach. main/gen/ds_colors.h wird dabei ueberschrieben und ist NIE von Hand zu
editieren.

OKLCH -> sRGB nach Bjoern Ottosson (oklab, public-domain Referenzformeln).
--verify schreibt verify_colors.html: dieselben Tokens im Browser gerendert,
Chrome ist die Referenzimplementierung. ⚠️ getComputedStyle taugt dafuer NICHT
(Chrome gibt oklch unaufgeloest zurueck) — die Seite misst deshalb ein
Canvas-Pixel (fillStyle + getImageData).

Nutzung:  python3 tools/gen_ds_colors.py            # schreibt Header
          python3 tools/gen_ds_colors.py --verify   # + Vergleichsseite
"""
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
OUT_H = FW / "main" / "gen" / "ds_colors.h"
OUT_VERIFY = HERE / "verify_colors.html"

# ---------------------------------------------------------------- Tag-Palette
# Dunkles Graphit-Petrol als Grund; Bedeutung traegt ausschliesslich der Akzent.
# Die Zuordnung ist bewusst eng: gruen = Energie fliesst IN die Batterie,
# cyan = Energie fliesst RAUS, amber = Achtung, rot = Grenze erreicht. Werte
# ohne Schwelle (Spannung, Temperatur) bleiben neutral — sonst leuchtet das
# Panel bunt, ohne etwas zu sagen.
DAY = {
    "background":     (0.17, 0.018, 231.0),   # Seitengrund
    "card":           (0.21, 0.020, 229.0),   # Kachelflaeche
    "card_hi":        (0.245, 0.022, 229.0),  # Hero-Kachel (eine Stufe heller)
    "foreground":     (0.95, 0.006, 225.0),   # Werte
    "muted":          (0.70, 0.015, 225.0),   # Labels/Einheiten
    "border":         (0.31, 0.020, 229.0),   # Kartenrand, Trennlinien
    "track":          (0.27, 0.020, 229.0),   # Balken-Hintergrund
    "primary":        (0.72, 0.105, 196.0),   # Pills, Seiten-Dots, Marken
    "accent":         (0.78, 0.140, 74.0),    # Hervorhebung
    "charge":         (0.72, 0.150, 150.0),   # Laden  (gruen)
    "discharge":      (0.72, 0.120, 235.0),   # Entladen (blau)
    "idle":           (0.70, 0.015, 225.0),   # Floating (neutral)
    "warn":           (0.80, 0.140, 74.0),    # Warnschwelle (amber)
    "crit":           (0.64, 0.210, 25.0),    # Grenzwert (rot)
    "offline":        (0.60, 0.012, 235.0),   # keine Daten
}

# --------------------------------------------------------------- Nacht-Palette
# ⚠️ Nacht ist eine EIGENE Palette, nicht dieselbe dunkler: nach RGB565 liegen
# card und background nur 1–3 Stufen auseinander und verschmelzen beim Dimmen.
# Im Amber-Monochrom traegt Helligkeit + Rahmen + Icon die Bedeutung, nicht der
# Farbton — deshalb zeigen charge/discharge/idle hier auf denselben Wert.
NIGHT = {
    "night_bg":          (0.00, 0.00, 0.0),
    "night_card":        (0.00, 0.00, 0.0),   # Karte = Grund, der Rand traegt
    "night_border":      (0.30, 0.07, 74.0),
    "night_border_hd":   (0.34, 0.08, 74.0),  # Trenner Statuszeile/Fussleiste
    "night_track":       (0.24, 0.06, 74.0),
    "night_muted":       (0.50, 0.09, 74.0),
    "night_value":       (0.62, 0.11, 74.0),
    "night_unit":        (0.44, 0.08, 74.0),
    "night_primary":     (0.70, 0.13, 74.0),
    "night_crit_border": (0.78, 0.14, 74.0),
    "night_crit_value":  (0.84, 0.15, 74.0),
}


def oklch_to_srgb8(L: float, C: float, H_deg: float):
    """OKLCH -> (r,g,b) 0..255, Gamut-Clip durch Clamp je Kanal."""
    h = math.radians(H_deg)
    a = C * math.cos(h)
    b = C * math.sin(h)
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
    lr = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s
    lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s
    lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s

    def gamma(x: float) -> float:
        x = min(max(x, 0.0), 1.0)
        return 12.92 * x if x <= 0.0031308 else 1.055 * (x ** (1 / 2.4)) - 0.055

    return tuple(round(min(max(gamma(v), 0.0), 1.0) * 255) for v in (lr, lg, lb))


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def write_verify(entries):
    """Vergleichsseite: Browser rechnet oklch selbst, wir lesen ein Canvas-Pixel."""
    rows = "\n".join(
        f'  {{name:"{n}", lch:[{l},{c},{h}], py:[{r},{g},{b}]}},'
        for n, (l, c, h), (r, g, b), _ in entries
    )
    OUT_VERIFY.write_text(
        "<!doctype html><meta charset=utf-8><title>SolisM5 Farb-Verifikation</title>"
        "<style>body{font:14px system-ui;background:#111;color:#eee}"
        "td{padding:2px 8px}.bad{color:#f55;font-weight:700}.ok{color:#5b5}</style>"
        "<h1>oklch -> sRGB: Python gegen Chrome</h1><table id=t></table>"
        f"<script>const E=[\n{rows}\n];"
        "const cv=document.createElement('canvas');cv.width=cv.height=1;"
        "const cx=cv.getContext('2d',{willReadFrequently:true});"
        "let bad=0,html='<tr><th>Token<th>oklch<th>Chrome<th>Python<th>d';"
        "for(const e of E){cx.fillStyle=`oklch(${e.lch[0]} ${e.lch[1]} ${e.lch[2]})`;"
        "cx.fillRect(0,0,1,1);const p=cx.getImageData(0,0,1,1).data;"
        "const ch=[p[0],p[1],p[2]];"
        "const d=Math.max(...ch.map((v,i)=>Math.abs(v-e.py[i])));if(d>1)bad++;"
        "html+=`<tr><td>${e.name}<td>${e.lch.join(' ')}<td>${ch}<td>${e.py}`+"
        "`<td class=${d>1?'bad':'ok'}>${d}`;}"
        "html+=`<tr><td colspan=5>${bad?bad+' Abweichung(en) > 1':'alle Farben exakt'}`;"
        "document.getElementById('t').innerHTML=html;</script>",
        encoding="utf-8",
    )
    print(f"geschrieben: {OUT_VERIFY.name}  (in Chrome oeffnen)")


def main():
    entries = []  # (ident, (L,C,H), (r,g,b), rgb565)
    for name, lch in list(DAY.items()) + list(NIGHT.items()):
        rgb = oklch_to_srgb8(*lch)
        entries.append((name, lch, rgb, rgb565(*rgb)))

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// GENERIERT von tools/gen_ds_colors.py — NICHT von Hand editieren.",
        "// Quelle ist die OKLCH-Palette im Generator selbst.",
        "// Verifikation der oklch->sRGB-Konvertierung: tools/verify_colors.html",
        "// (Canvas-Pixel aus Chrome als Referenz, Toleranz +-1/Kanal).",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace ds {",
    ]
    width = max(len(n) for n, *_ in entries)
    for name, (L, C, H), (r, g, b), v in entries:
        lines.append(
            f"constexpr uint16_t {name:<{width}} = 0x{v:04X};"
            f"  // oklch({L} {C} {H}) -> rgb({r},{g},{b})"
        )
    lines.append("}  // namespace ds")
    lines.append("")
    OUT_H.write_text("\n".join(lines), encoding="utf-8")
    print(f"geschrieben: {OUT_H.relative_to(FW)}  ({len(entries)} Farben)")

    if "--verify" in sys.argv:
        write_verify(entries)


if __name__ == "__main__":
    main()
