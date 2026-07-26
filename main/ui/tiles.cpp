#include "tiles.h"
#include "theme.h"
#include "ds_colors.h"
#include "ds_icons.h"
#include "fonts_vlw.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "tiles";

// ---- Fonts ----------------------------------------------------------------
// Eigene VLWfont-Instanzen statt LGFXBase::loadFont(): loadFont parst bei JEDEM
// Aufruf den ganzen Font neu. Fuenf statische Instanzen laden einmal, danach ist
// setFont() ein Zeigertausch. ⚠️ Die PointerWrapper muessen dauerhaft leben —
// VLWfont::drawChar liest die Glyphen-Bitmaps zur Zeichenzeit daraus.
static lgfx::VLWfont        s_f44, s_f38, s_f20, s_f16, s_f12;
static lgfx::PointerWrapper s_w44, s_w38, s_w20, s_w16, s_w12;

void ui_fonts_init()
{
    s_w44.set(vlw_mono44, sizeof vlw_mono44);
    s_w38.set(vlw_mono38, sizeof vlw_mono38);
    s_w20.set(vlw_mono20, sizeof vlw_mono20);
    s_w16.set(vlw_sans16, sizeof vlw_sans16);
    s_w12.set(vlw_sans12, sizeof vlw_sans12);
    const bool ok = s_f44.loadFont(&s_w44) && s_f38.loadFont(&s_w38) &&
                    s_f20.loadFont(&s_w20) && s_f16.loadFont(&s_w16) &&
                    s_f12.loadFont(&s_w12);
    if (!ok) ESP_LOGE(TAG, "VLW laden fehlgeschlagen — Formatfehler im Generator?");
}

const lgfx::IFont *font_mono44() { return &s_f44; }
const lgfx::IFont *font_mono38() { return &s_f38; }
const lgfx::IFont *font_mono20() { return &s_f20; }
const lgfx::IFont *font_sans16() { return &s_f16; }
const lgfx::IFont *font_sans12() { return &s_f12; }

// Descender je Font (aus den VLW-Headern, s. gen_vlw.py-Ausgabe). Gebraucht,
// weil `bottom_left` die GLYPHENBOX ausrichtet und die den Descender enthaelt:
// Wert und Einheit auf demselben y saehen sonst versetzt aus.
static constexpr int DESC_44 = 14, DESC_38 = 12, DESC_20 = 6;

// ---- Sprite ---------------------------------------------------------------
static M5Canvas s_cv(&M5.Display);
static bool     s_ready;

void tiles_init()
{
    s_cv.setColorDepth(16);
    s_cv.setPsram(false);      // dieses Board hat keins; explizit fuer den Fall
    s_ready = s_cv.createSprite(lay::HERO_W, lay::HERO_H);
    if (!s_ready) {
        ESP_LOGE(TAG, "Sprite %dx%d (%d B) nicht allozierbar",
                 lay::HERO_W, lay::HERO_H, lay::HERO_W * lay::HERO_H * 2);
    }
}

// sx/sy = wohin die linke obere SPRITE-Ecke gehoert.
// cx..ch = welcher Ausschnitt des Displays dabei ueberhaupt beschrieben wird.
//
// ⚠️ Das ist NICHT dasselbe, und die Verwechslung ist teuer: pushSprite legt
// Sprite-Pixel (0,0) auf (sx,sy). Wer das Clip-Rechteck auch als Push-Ziel
// uebergibt, verschiebt das ganze Bild um den Versatz des Ausschnitts — beim
// Dirty-Update erschienen dann Kartenrand und Label an der Stelle des
// Messwerts, und der Wert war nach der ersten Aktualisierung unlesbar.
static void push(int sx, int sy, int cx, int cy, int cw, int ch)
{
    M5.Display.setClipRect(cx, cy, cw, ch);
    s_cv.pushSprite(sx, sy);
    M5.Display.clearClipRect();
}

// Karte zeichnen. Es gibt kein drawSmoothRoundRect in LovyanGFX — Antialiasing
// gibt es nur gefuellt. Also: aussen gefuellt in Randfarbe, innen 1 px kleiner
// in Kartenfarbe. Auf RGB565 ist dieser Rand die eigentliche Kartengrenze.
static void card(int x, int y, int w, int h, uint16_t fill, uint16_t border, int bw)
{
    s_cv.fillSmoothRoundRect(x, y, w, h, lay::RADIUS, border);
    s_cv.fillSmoothRoundRect(x + bw, y + bw, w - 2 * bw, h - 2 * bw,
                             lay::RADIUS - bw, fill);
}

static uint16_t value_color(Zone z, Fresh f, const Palette &p)
{
    if (f == Fresh::offline) return p.offline;
    if (f == Fresh::stale)   return p.label;      // gedimmt; der Punkt erklaert es
    switch (z) {
        case Zone::ok:       return p.charge;
        case Zone::low:      return p.warn;
        case Zone::critical: return p.crit;
        default:             return p.value;
    }
}

// ⚠️⚠️ ALLE Texte im Sprite werden TRANSPARENT gesetzt (einargumentiges
// setTextColor). Mit Hintergrundfarbe fuellt LovyanGFX die komplette
// GLYPHENBOX, und die ist viel hoeher als die Zeichen darin:
//   Kachel 148x81 — Label-Box  7..30, Wert-Box 23..74 (mono38 ist 51 px hoch).
//   Die Boxen ueberlappen also um 7 px, obwohl die Zeichen 12 px Luft haben.
// Der Wert wird nach dem Label gezeichnet und hat damit dessen Unterlaengen
// ausradiert — "Spannung" und "Temperatur" verloren ihre p/g. Genau das war im
// Feld zu sehen.
// Transparent ist hier gefahrlos: die Karte liegt schon im Sprite, das
// Antialiasing mischt gegen die tatsaechliche Kartenfarbe.
// Ausnahme bleibt pushGrayscaleImage — das BRAUCHT die echte Hintergrundfarbe,
// weil es zwischen fore und back interpoliert statt zu ueberlagern.

// Kopfzeile einer Karte: Label links, Zustands-Icon rechts, Stale-Punkt dahinter.
static void head(int w, const char *label, const uint8_t *icon, uint16_t icol,
                 Fresh fresh, bool crit, const Palette &p, uint16_t bg)
{
    s_cv.setFont(font_sans16());
    s_cv.setTextDatum(lgfx::top_left);
    s_cv.setTextColor(crit ? p.crit_border : p.label);
    s_cv.drawString(label, lay::TILE_PAD_X, lay::TILE_PAD_Y);

    if (fresh == Fresh::stale) {
        const int lx = lay::TILE_PAD_X + s_cv.textWidth(label) + 8;
        s_cv.fillSmoothCircle(lx + 3, lay::TILE_PAD_Y + 8, 3, p.warn);
    }
    if (icon) {
        // ⚠️ pushGrayscaleImage ist kein echtes Alpha: es interpoliert zwischen
        // fore und back. backcolor MUSS die tatsaechliche Flaechenfarbe sein.
        s_cv.pushGrayscaleImage(w - lay::TILE_PAD_X - DS_ICON_W, lay::TILE_PAD_Y,
                                DS_ICON_W, DS_ICON_H, icon,
                                lgfx::grayscale_8bit, icol, bg);
    }
}

// Wert + Einheit auf gemeinsamer Grundlinie. Passt der Wert in der grossen
// Schrift nicht in die Karte, wird er klein gesetzt statt abgeschnitten —
// lieber unauffaellig anders als halb unsichtbar.
static void value_line(int inner_w, const char *value, const char *unit,
                       uint16_t col, uint16_t ucol, int base_y, bool big)
{
    const lgfx::IFont *vf = big ? font_mono38() : font_mono20();
    int desc = big ? DESC_38 : DESC_20;

    s_cv.setFont(vf);
    int need = s_cv.textWidth(value);
    if (unit && unit[0]) {
        s_cv.setFont(font_mono20());
        need += s_cv.textWidth(unit) + 4;
    }
    if (big && need > inner_w) {          // Notbremse: eine Stufe kleiner
        vf = font_mono20();
        desc = DESC_20;
    }

    s_cv.setFont(vf);
    s_cv.setTextDatum(lgfx::bottom_left);
    s_cv.setTextColor(col);
    s_cv.drawString(value, lay::TILE_PAD_X, base_y);
    if (unit && unit[0]) {
        const int ux = lay::TILE_PAD_X + s_cv.textWidth(value) + 4;
        s_cv.setFont(font_mono20());
        s_cv.setTextColor(ucol);
        s_cv.drawString(unit, ux, base_y - (desc - DESC_20));
    }
}

// ---- Kachel ---------------------------------------------------------------

static void render_tile(const TileSpec &t)
{
    const Palette &p = theme_pal();
    const bool crit = (t.zone == Zone::critical && t.fresh == Fresh::fresh);
    const int  bw   = crit ? 2 : 1;

    s_cv.fillRect(0, 0, lay::TILE_W, lay::TILE_H, p.bg);
    card(0, 0, lay::TILE_W, lay::TILE_H, p.card, crit ? p.crit_border : p.border, bw);

    const uint8_t *icon = t.icon;
    uint16_t icol = p.label;
    if (crit)                           { icon = ic_warn;    icol = p.crit_border; }
    else if (t.fresh == Fresh::stale)   { icon = ic_stale;   icol = p.warn; }
    else if (t.fresh == Fresh::offline) { icon = ic_offline; icol = p.offline; }
    head(lay::TILE_W, t.label, icon, icol, t.fresh, crit, p, p.card);

    value_line(lay::TILE_W - 2 * lay::TILE_PAD_X, t.value, t.unit,
               value_color(t.zone, t.fresh, p), p.unit,
               lay::TILE_H - lay::TILE_PAD_Y, true);
}

void tile_draw(int col, int row, const TileSpec &t)
{
    if (!s_ready) return;
    render_tile(t);
    const int x = lay::tile_x(col), y = lay::tile_y(row);
    push(x, y, x, y, lay::TILE_W, lay::TILE_H);
}

// Nur der Wertbereich wandert ueber den Bus. Gerendert wird trotzdem die ganze
// Kachel — im RAM ist das billig, teuer ist der Transfer zum Display.
void tile_update_value(int col, int row, const TileSpec &t)
{
    if (!s_ready) return;
    render_tile(t);
    const int x = lay::tile_x(col), y = lay::tile_y(row);
    // Fenster ueber der Wertzeile: Oberkante 44 px ueber der Grundlinie (mono38
    // ist 51 px hoch, davon 12 px Descender — die Ziffern beginnen ~39 px
    // darueber), 46 px hoch bis knapp unter die Grundlinie. Ein zu knapper
    // Ausschnitt schneidet oben die Ziffernkoepfe ab.
    const int clip_y = y + lay::TILE_H - lay::TILE_PAD_Y - 44;
    push(x, y, x + 1, clip_y, lay::TILE_W - 2, 46);
}

// ---- Hero -----------------------------------------------------------------
// Feste Bandlagen; die Zahlen sind gegen die Fontmetriken gerechnet
// (mono44: ascent 45 / descent 14) und nicht geraten.
static constexpr int HERO_VALUE_BASE = 86;
static constexpr int HERO_BAR_Y = 94, HERO_BAR_H = 12;
static constexpr int HERO_LINE1_Y = 112, HERO_LINE2_Y = 138;

void hero_draw(const HeroSpec &h)
{
    if (!s_ready) return;
    const Palette &p = theme_pal();
    const bool crit = (h.zone == Zone::critical && h.fresh == Fresh::fresh);
    const int  bw   = crit ? 2 : 1;

    s_cv.fillRect(0, 0, lay::HERO_W, lay::HERO_H, p.bg);
    card(0, 0, lay::HERO_W, lay::HERO_H, p.card_hi,
         crit ? p.crit_border : p.border, bw);

    const uint8_t *icon = h.icon;
    uint16_t icol = h.accent;
    if (crit)                           { icon = ic_warn;    icol = p.crit_border; }
    else if (h.fresh == Fresh::stale)   { icon = ic_stale;   icol = p.warn; }
    else if (h.fresh == Fresh::offline) { icon = ic_offline; icol = p.offline; }
    head(lay::HERO_W, h.label, icon, icol, h.fresh, crit, p, p.card_hi);

    // Grosser Wert
    const uint16_t vcol = value_color(h.zone, h.fresh, p);
    s_cv.setFont(font_mono44());
    s_cv.setTextDatum(lgfx::bottom_left);
    s_cv.setTextColor(vcol);
    s_cv.drawString(h.value, lay::TILE_PAD_X, HERO_VALUE_BASE);
    if (h.unit && h.unit[0]) {
        const int ux = lay::TILE_PAD_X + s_cv.textWidth(h.value) + 5;
        s_cv.setFont(font_mono20());
        s_cv.setTextColor(p.unit);
        s_cv.drawString(h.unit, ux, HERO_VALUE_BASE - (DESC_44 - DESC_20));
    }

    // Fuellbalken: Spur immer voll zeichnen, damit ein sinkender Wert die alte
    // Fuellung ueberdeckt.
    const int bx = lay::TILE_PAD_X;
    const int bw_total = lay::HERO_W - 2 * lay::TILE_PAD_X;
    s_cv.fillSmoothRoundRect(bx, HERO_BAR_Y, bw_total, HERO_BAR_H,
                             HERO_BAR_H / 2, p.track);
    int fill = h.fill_pm < 0 ? 0 : h.fill_pm > 1000 ? 1000 : h.fill_pm;
    int fw = bw_total * fill / 1000;
    if (fw > 0) {
        if (fw < HERO_BAR_H) fw = HERO_BAR_H;   // Rundung braucht Mindestbreite
        s_cv.fillSmoothRoundRect(bx, HERO_BAR_Y, fw, HERO_BAR_H,
                                 HERO_BAR_H / 2,
                                 h.fresh == Fresh::fresh ? vcol : p.offline);
    }

    s_cv.setFont(font_sans16());
    s_cv.setTextDatum(lgfx::top_left);
    s_cv.setTextColor(h.accent);
    s_cv.drawString(h.line1, lay::TILE_PAD_X, HERO_LINE1_Y);

    s_cv.setFont(font_mono20());
    s_cv.setTextColor(p.label);
    s_cv.drawString(h.line2, lay::TILE_PAD_X, HERO_LINE2_Y);

    const int hx = lay::tile_x(0), hy = lay::tile_y(0);
    push(hx, hy, hx, hy, lay::HERO_W, lay::HERO_H);
}
