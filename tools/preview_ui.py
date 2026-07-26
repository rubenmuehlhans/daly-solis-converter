#!/usr/bin/env python3
"""preview_ui.py — Layout-Vorschau der Panel-Screens am Desktop.

Zeichnet dieselben Bildschirme wie die Firmware, in echter Geraetegroesse
320x240, mit DENSELBEN Schriften (aus assets/fonts), DENSELBEN Pixelgroessen und
DERSELBEN Palette wie tools/gen_ds_colors.py. Zweck: Ueberlaeufe, Abstaende und
Lesbarkeit pruefen, ohne zu flashen.

⚠️ Das ist eine VORSCHAU, kein Simulator. Sie stellt das Layout nach, nicht die
Firmware: die Masse hier muessen mit ui/theme.h (lay::) von Hand
uebereinstimmen. Wer lay:: aendert, aendert LAY unten mit. Ausserdem rendert PIL
Antialiasing anders als LovyanGFX und kennt kein RGB565 — Farben und Kantenglaettung
sehen am Geraet minimal anders aus. Was sie zuverlaessig zeigt: ob Text in seine
Flaeche passt.

Nutzung:  ~/panelvenv/bin/python3 tools/preview_ui.py [--night]
"""
import io
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from fontTools import ttLib
from fontTools.varLib import instancer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_ds_colors import DAY, NIGHT, oklch_to_srgb8  # dieselbe Quelle

HERE = Path(__file__).resolve().parent
FONTS = HERE.parent / "assets" / "fonts"
OUT = HERE / "shots"

# ---- lay:: aus ui/theme.h -------------------------------------------------
LAY = dict(W=320, H=240, STATUS_H=28, BAR_H=26, PAD=8, GAP=8, RADIUS=10,
           TILE_PAD_X=9, TILE_PAD_Y=7)
LAY["GRID_Y"] = LAY["STATUS_H"]
LAY["GRID_H"] = LAY["H"] - LAY["STATUS_H"] - LAY["BAR_H"]
LAY["TILE_W"] = (LAY["W"] - 2 * LAY["PAD"] - LAY["GAP"]) // 2
LAY["TILE_H"] = (LAY["GRID_H"] - 2 * LAY["PAD"] - LAY["GAP"]) // 2
LAY["HERO_H"] = 2 * LAY["TILE_H"] + LAY["GAP"]
LAY["BAR_Y"] = LAY["H"] - LAY["BAR_H"]
BTN_CX = (60, 160, 260)


def tile_x(c): return LAY["PAD"] + c * (LAY["TILE_W"] + LAY["GAP"])
def tile_y(r): return LAY["GRID_Y"] + LAY["PAD"] + r * (LAY["TILE_H"] + LAY["GAP"])


def rgb(name, night):
    src = NIGHT if night else DAY
    key = ("night_" + name) if night else name
    if key not in src:                     # Nacht kennt weniger Rollen
        key = {"card_hi": "night_card", "charge": "night_value",
               "discharge": "night_value", "idle": "night_muted",
               "warn": "night_crit_border", "crit": "night_crit_value",
               "offline": "night_unit", "background": "night_bg",
               "foreground": "night_value", "muted": "night_muted",
               "accent": "night_primary"}.get(name, key)
    return oklch_to_srgb8(*src[key])


def load_font(fname, px, wght=None):
    if wght is None:
        return ImageFont.truetype(str(FONTS / fname), px)
    tt = ttLib.TTFont(FONTS / fname)
    if "fvar" in tt:
        instancer.instantiateVariableFont(tt, {"wght": wght}, inplace=True)
    buf = io.BytesIO()
    tt.save(buf)
    buf.seek(0)
    return ImageFont.truetype(buf, px)


class P:
    """Palette + Schriften, wie die Firmware sie sieht."""

    def __init__(self, night):
        self.night = night
        for r in ("background", "card", "card_hi", "border", "track", "muted",
                  "foreground", "primary", "accent", "charge", "discharge",
                  "idle", "warn", "crit", "offline"):
            setattr(self, r, rgb(r, night))
        self.bg, self.label, self.value, self.unit = (
            self.background, self.muted, self.foreground, self.muted)
        self.mono44 = load_font("JetBrainsMono-Medium.ttf", 44)
        self.mono38 = load_font("JetBrainsMono-Medium.ttf", 38)
        self.mono20 = load_font("JetBrainsMono-Medium.ttf", 20)
        self.sans16 = load_font("Manrope[wght].ttf", 16, 600)
        self.sans12 = load_font("Manrope[wght].ttf", 12, 600)


def card(d, x, y, w, h, fill, border, bw=1, r=None):
    r = LAY["RADIUS"] if r is None else r
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=border)
    d.rounded_rectangle([x + bw, y + bw, x + w - 1 - bw, y + h - 1 - bw],
                        radius=r - bw, fill=fill)


def tw(d, text, font):
    return d.textlength(text, font=font)


def tile(d, p, col, row, label, value, unit, vcol=None, crit=False, warn=None):
    x, y, w, h = tile_x(col), tile_y(row), LAY["TILE_W"], LAY["TILE_H"]
    card(d, x, y, w, h, p.card, p.crit if crit else p.border, 2 if crit else 1)
    d.text((x + LAY["TILE_PAD_X"], y + LAY["TILE_PAD_Y"] - 2), label,
           font=p.sans16, fill=p.crit if crit else p.label)
    base = y + h - LAY["TILE_PAD_Y"]
    d.text((x + LAY["TILE_PAD_X"], base - 12), value, font=p.mono38,
           anchor="ls", fill=vcol or p.value)
    if unit:
        ux = x + LAY["TILE_PAD_X"] + tw(d, value, p.mono38) + 4
        d.text((ux, base - 6), unit, font=p.mono20, anchor="ls", fill=p.unit)
    need = tw(d, value, p.mono38) + (4 + tw(d, unit, p.mono20) if unit else 0)
    if need > w - 2 * LAY["TILE_PAD_X"]:       # Ueberlaufmarke der Vorschau
        d.line([x + w - 3, y + 3, x + w - 3, y + h - 3], fill=(255, 0, 0), width=2)


def status(d, p, title, page, uptime, fault=None):
    d.rectangle([0, 0, LAY["W"], LAY["STATUS_H"]], fill=p.bg)
    cy = LAY["STATUS_H"] // 2
    d.text((10, cy), title, font=p.sans16, anchor="lm", fill=p.value)
    dots = 10 + tw(d, title, p.sans16) + 14
    for i in range(4):
        cx = dots + i * 11
        if i == page:
            d.rounded_rectangle([cx - 4, cy - 2, cx + 6, cy + 2], radius=2, fill=p.primary)
        else:
            d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=p.border)
    d.text((LAY["W"] - 10, cy), uptime, font=p.mono20, anchor="rm", fill=p.label)
    if fault:
        rx = LAY["W"] - 10 - tw(d, uptime, p.mono20)
        pw = tw(d, fault, p.sans12) + 12
        px = rx - 10 - pw
        d.rounded_rectangle([px, cy - 8, px + pw, cy + 8], radius=8, fill=p.crit)
        d.text((px + pw / 2, cy), fault, font=p.sans12, anchor="mm", fill=p.bg)
    d.line([0, LAY["STATUS_H"] - 1, LAY["W"], LAY["STATUS_H"] - 1], fill=p.border)


def bar(d, p, mos_on):
    y = LAY["BAR_Y"]
    d.rectangle([0, y, LAY["W"], LAY["H"]], fill=p.bg)
    labels = ["Seite", "Nacht" if not p.night else "Tag",
              "MOS aus" if mos_on else "MOS ein"]
    for i, s in enumerate(labels):
        d.text((BTN_CX[i], y + LAY["BAR_H"] / 2 + 1), s, font=p.sans16,
               anchor="mm", fill=p.accent if i == 2 else p.label)
    d.line([0, y, LAY["W"], y], fill=p.border)


def hero(d, p, soc, flow, watt):
    x, y, w, h = tile_x(0), tile_y(0), LAY["TILE_W"], LAY["HERO_H"]
    card(d, x, y, w, h, p.card_hi, p.border)
    acc = {"Laden": p.charge, "Entladen": p.discharge}.get(flow, p.idle)
    d.text((x + LAY["TILE_PAD_X"], y + LAY["TILE_PAD_Y"] - 2), "Ladezustand",
           font=p.sans16, fill=p.label)
    val = "100" if soc >= 100 else f"{soc:.1f}"
    vcol = p.crit if soc <= 10 else p.warn if soc <= 20 else p.charge
    d.text((x + LAY["TILE_PAD_X"], y + 86 - 14), val, font=p.mono44,
           anchor="ls", fill=vcol)
    ux = x + LAY["TILE_PAD_X"] + tw(d, val, p.mono44) + 5
    d.text((ux, y + 86 - 6), "%", font=p.mono20, anchor="ls", fill=p.unit)
    bx, bw_ = x + LAY["TILE_PAD_X"], w - 2 * LAY["TILE_PAD_X"]
    by, bh = y + 94, 12
    d.rounded_rectangle([bx, by, bx + bw_, by + bh], radius=bh // 2, fill=p.track)
    fw = int(bw_ * min(max(soc, 0), 100) / 100)
    if fw:
        d.rounded_rectangle([bx, by, bx + max(fw, bh), by + bh], radius=bh // 2, fill=vcol)
    d.text((x + LAY["TILE_PAD_X"], y + 112), flow, font=p.sans16, fill=acc)
    ws = f"{watt} W" if abs(watt) < 1000 else f"{watt/1000:.1f} kW"
    d.text((x + LAY["TILE_PAD_X"], y + 138), ws, font=p.mono20, fill=p.label)
    # Ueberlaufmarke — genau wie die Firmware misst: Wert + 5 px + Einheit
    inner = w - 2 * LAY["TILE_PAD_X"]
    widths = [tw(d, val, p.mono44) + 5 + tw(d, "%", p.mono20),
              tw(d, flow, p.sans16), tw(d, ws, p.mono20)]
    if max(widths) > inner:
        d.line([x + w - 3, y + 3, x + w - 3, y + h - 3], fill=(255, 0, 0), width=2)


def page_energie(d, p):
    status(d, p, "Energie", 0, "6h 14m")
    hero(d, p, 82.4, "Entladen", -664)
    tile(d, p, 1, 0, "Spannung", "53.2", "V")
    tile(d, p, 1, 1, "Strom", "-110", "A")
    bar(d, p, True)


def page_batterie(d, p):
    status(d, p, "Batterie", 1, "6h 14m")
    tile(d, p, 0, 0, "Zelle max", "3412", "mV", p.warn)
    tile(d, p, 1, 0, "Zelle min", "3284", "mV")
    tile(d, p, 0, 1, "Differenz", "128", "mV", p.crit, crit=True)
    tile(d, p, 1, 1, "Temperatur", "23", "°C")
    bar(d, p, True)


def page_zellen(d, p):
    status(d, p, "Zellen", 2, "6h 14m")
    mv = [3284, 3301, 3412, 3298, 3305, 3299, 3302, 3288,
          3295, 3300, 3284, 3297, 3303, 3299, 3301, 3296]
    lo, hi = min(mv), max(mv)
    span = max(hi - lo, 60)
    lo, hi = lo - 10, lo + span + 10
    span = hi - lo
    top, bot = LAY["GRID_Y"] + 34, LAY["BAR_Y"] - 20
    H = bot - top
    d.text((10, LAY["GRID_Y"] + 15), "Streuung", font=p.sans16, anchor="lm", fill=p.label)
    d.text((LAY["W"] - 10, LAY["GRID_Y"] + 15), f"{hi-lo-20} mV", font=p.mono20,
           anchor="rm", fill=p.crit)
    step = (LAY["W"] - 2 * LAY["PAD"]) // 16
    for i, v in enumerate(mv):
        x = LAY["PAD"] + i * step
        h = max(int((v - lo) * H / span), 2)
        col = p.primary
        if v == hi: col = p.warn
        if v == lo: col = p.discharge
        d.rectangle([x, top, x + step - 3, bot], fill=p.track)
        d.rectangle([x, bot - h, x + step - 3, bot], fill=col)
        d.text((x + (step - 2) / 2, bot + 3), str(i + 1), font=p.sans12,
               anchor="ma", fill=p.value if v in (lo, hi) else p.label)
    bar(d, p, True)


def page_system(d, p):
    status(d, p, "System", 3, "6h 14m")
    tile(d, p, 0, 0, "Ladestrom", "42", "A")
    tile(d, p, 1, 0, "Entladestrom", "100", "A")
    x, y = LAY["PAD"], tile_y(1)
    w, h = LAY["W"] - 2 * LAY["PAD"], LAY["TILE_H"]
    card(d, x, y, w, h, p.card, p.border)
    rows = [("Rest", "47.2 Ah · 312 Zyklen", p.value),
            ("Netz", "192.168.1.225 · MQTT ok", p.value),
            ("Betrieb", "CAN ok · 0 Fehler · 2.0.0", p.value)]
    rh = (h - 12) // 3
    for i, (k, v, c) in enumerate(rows):
        ry = y + 6 + i * rh + rh / 2
        d.text((x + 10, ry), k, font=p.sans12, anchor="lm", fill=p.label)
        d.text((x + 82, ry), v, font=p.sans16, anchor="lm", fill=c)
        # +3 px je Leerzeichen: so breit zeichnet LovyanGFX es wirklich (s. pages.cpp)
        if 82 + tw(d, v, p.sans16) + v.count(" ") * 3 + 10 > w:
            d.line([x + w - 3, y + 3, x + w - 3, y + h - 3], fill=(255, 0, 0), width=2)
    bar(d, p, True)


def screen_confirm(d, p):
    page_energie(d, p)
    CW, CH = 268, 118
    cx, cy = (LAY["W"] - CW) // 2, (LAY["H"] - CH) // 2
    card(d, cx, cy, CW, CH, p.card, p.crit, 2, LAY["RADIUS"] + 2)
    d.text((cx + 42, cy + 23), "MOSFET ausschalten?", font=p.sans16, anchor="lm", fill=p.value)
    d.text((cx + 16, cy + 50), "Trennt die Batterie vom Wechselrichter",
           font=p.sans12, fill=p.label)
    d.text((cx + 16, cy + 70), "Taste C halten", font=p.sans12, fill=p.label)
    bx, by, bw_, bh = cx + 16, cy + 86, CW - 32, 12
    d.rounded_rectangle([bx, by, bx + bw_, by + bh], radius=bh // 2, fill=p.track)
    d.rounded_rectangle([bx, by, bx + int(bw_ * 0.6), by + bh], radius=bh // 2, fill=p.crit)


def screen_boot(d, p):
    d.rectangle([0, 0, LAY["W"], LAY["H"]], fill=p.bg)
    d.text((LAY["W"] / 2, 78), "SolisM5", font=p.mono38, anchor="mm", fill=p.primary)
    d.text((LAY["W"] / 2, 110), "DALY BMS · SOLIS Wechselrichter",
           font=p.sans16, anchor="mm", fill=p.label)
    d.text((LAY["W"] / 2, 132), "2.0.0 (a1b2c3d)", font=p.sans12, anchor="mm", fill=p.border)
    d.text((LAY["W"] / 2, 182), "CAN-Bus", font=p.sans16, anchor="mm", fill=p.value)


SCREENS = {
    "page0_energie": page_energie,
    "page1_batterie": page_batterie,
    "page2_zellen": page_zellen,
    "page3_system": page_system,
    "confirm_mos": screen_confirm,
    "boot": screen_boot,
}


def main():
    night = "--night" in sys.argv
    p = P(night)
    OUT.mkdir(exist_ok=True)
    sheet = Image.new("RGB", (LAY["W"] * 3 + 40, LAY["H"] * 2 + 30), (20, 20, 20))
    for i, (name, fn) in enumerate(SCREENS.items()):
        img = Image.new("RGB", (LAY["W"], LAY["H"]), p.bg)
        fn(ImageDraw.Draw(img), p)
        suffix = "_nacht" if night else ""
        img.save(OUT / f"{name}{suffix}.png")
        sheet.paste(img, (10 + (i % 3) * (LAY["W"] + 10), 10 + (i // 3) * (LAY["H"] + 10)))
    sheet.save(OUT / f"uebersicht{'_nacht' if night else ''}.png")
    print(f"geschrieben: {OUT}  ({len(SCREENS)} Screens{' — Nacht' if night else ''})")
    print("Rote Linie am rechten Kachelrand = Text passt nicht in die Flaeche.")


if __name__ == "__main__":
    main()
