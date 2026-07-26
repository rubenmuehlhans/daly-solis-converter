#include "pages.h"
#include "theme.h"
#include "tiles.h"
#include "screens.h"
#include "ds_icons.h"
#include "../model.h"
#include "../app_config.h"
#include "../net/ota.h"
#include <M5Unified.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

static int  s_page;
static bool s_confirm;          // Halte-Bestaetigung fuer die MOSFETs laeuft
static bool s_confirm_done;     // in diesem Tastendruck bereits geschaltet
static uint32_t s_confirm_ms;
static char s_shown[4][16];     // zuletzt gezeichneter Wert je Kachelplatz
static char s_shown_status[40]; // zuletzt gezeichnete Statuszeile (rechter Teil)
static bool s_toast_on;

static const char *PAGE_TITLE[PAGE_COUNT] = {"Energie", "Batterie", "Zellen", "System"};

// ---- Zahlenformate --------------------------------------------------------
// Alle Werte kommen als Ganzzahl aus dem Modell; formatiert wird genau hier,
// damit das Display und Home Assistant nicht getrennt runden.

// ⚠️ Die Kachel ist innen 130 px breit; mono38 misst 22,8 px je Zeichen. Fuenf
// Zeichen plus Einheit liegen exakt auf der Kante, sechs laufen darueber. Die
// Formate hier sind danach gewaehlt — nicht nach Gefuehl:
//   "53.2"  V   4 Zeichen   91 px + 16   ok
//   "-110"  A   4 Zeichen   91 px + 16   ok   (mit Zehntel waeren es 137 px)
//   "3412"  mV  4 Zeichen   91 px + 28   ok   (als "3.412 V" waeren es 130 px)
// Home Assistant bekommt in jedem Fall den vollen Wert — die Kuerzung ist rein
// eine Sache der Anzeige.
static void f_volt(char *b, size_t n, uint16_t dv)
{
    snprintf(b, n, "%u.%u", dv / 100, (dv % 100) / 10);
}
static void f_amp(char *b, size_t n, int32_t ci)
{
    const char *s = ci < 0 ? "-" : "";
    const unsigned long a = (unsigned long)(ci < 0 ? -ci : ci);
    if (a >= 1000) snprintf(b, n, "%s%lu", s, a / 10);        // ab 100 A ohne Zehntel
    else           snprintf(b, n, "%s%lu.%lu", s, a / 10, a % 10);
}
static void f_cell(char *b, size_t n, uint16_t mv)      { snprintf(b, n, "%u", mv); }
static void f_int(char *b, size_t n, long v)            { snprintf(b, n, "%ld", v); }
static void f_ah(char *b, size_t n, uint32_t mah)       { snprintf(b, n, "%lu.%lu", (unsigned long)(mah / 1000), (unsigned long)((mah % 1000) / 100)); }
static void f_soc(char *b, size_t n, uint16_t pm)
{
    if (pm >= 1000) snprintf(b, n, "100");
    else            snprintf(b, n, "%u.%u", pm / 10, pm % 10);
}

// Leistung aus Spannung und Strom. Nirgends sonst sichtbar und die Zahl, die
// beim Blick aufs Geraet am meisten sagt: fliesst gerade viel oder wenig?
static int32_t watts(const BmsData &b)
{
    return (int32_t)((int64_t)b.pack_dv * b.pack_ci / 1000);
}

// ---- Neuzeichnen nur bei Aenderung ---------------------------------------
// Kacheln vergleichen ihren Wertstring. Fuer die drei Flaechen, die NICHT aus
// Kacheln bestehen (Hero, Zellendiagramm, Infokarte), gibt es stattdessen eine
// Signatur ueber alles, was man dort sieht. Ohne das wuerden sie fuenfmal pro
// Sekunde neu gemalt: das Diagramm flackert dabei sichtbar, und der Hero schiebt
// jedes Mal 50 KB ueber den SPI-Bus.
static uint32_t sig_mix(uint32_t h, uint32_t v)
{
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}
static uint32_t s_sig_hero, s_sig_cells, s_sig_info;

// ---- Statuszeile ----------------------------------------------------------
// Links Titel + Seitenpunkte, rechts Betriebszeit. Stoerungen erscheinen als
// Pille — und NUR dann. Eine Zeile voller gruener Haken traegt keine Information
// und macht die eine rote Pille, auf die es ankommt, unsichtbar.
//
// ⚠️ Text hier ohne Hintergrundfarbe (einargumentiges setTextColor): mit bg
// fuellt LovyanGFX die ganze Glyphenbox, und die ist hoeher als der Platz bis
// zur Trennlinie — die Linie wuerde uebermalt. Linien kommen zuletzt.
static void draw_status(const Model &m, bool full)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;

    char right[24];
    const uint32_t s = m.diag.uptime_s;
    if (s >= 86400) snprintf(right, sizeof right, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600));
    else            snprintf(right, sizeof right, "%luh %02lum", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));

    // Stoerungspillen in der Reihenfolge ihrer Dringlichkeit
    const char *fault = nullptr;
    uint16_t    fcol  = p.crit;
    if (m.bms.fresh == Fresh::offline) { fault = "BMS";  fcol = p.crit; }
    else if (!m.diag.can_ok)           { fault = "CAN";  fcol = p.crit; }
    else if (m.net.wifi && !m.net.mqtt) { fault = "MQTT"; fcol = p.warn; }
    else if (!m.net.wifi && config_get().wifi_ssid[0]) { fault = "WLAN"; fcol = p.warn; }

    char sig[40];   // Signatur des rechten Teils: nur bei Aenderung neu zeichnen
    snprintf(sig, sizeof sig, "%s|%s", right, fault ? fault : "");
    if (!full && strcmp(sig, s_shown_status) == 0) return;
    strlcpy(s_shown_status, sig, sizeof s_shown_status);

    d.fillRect(0, 0, lay::W, lay::STATUS_H, p.bg);

    d.setFont(font_sans16());
    d.setTextDatum(lgfx::middle_left);
    d.setTextColor(p.value);
    d.drawString(PAGE_TITLE[s_page], 10, lay::STATUS_H / 2);
    const int dots_x = 10 + d.textWidth(PAGE_TITLE[s_page]) + 14;

    for (int i = 0; i < PAGE_COUNT; i++) {
        const int cx = dots_x + i * 11;
        if (i == s_page) d.fillSmoothRoundRect(cx - 4, lay::STATUS_H / 2 - 2, 11, 5, 2, p.primary);
        else             d.fillSmoothCircle(cx, lay::STATUS_H / 2, 2, p.border);
    }

    d.setFont(font_mono20());
    d.setTextDatum(lgfx::middle_right);
    d.setTextColor(p.label);
    d.drawString(right, lay::W - 10, lay::STATUS_H / 2);
    const int right_x = lay::W - 10 - d.textWidth(right);

    if (fault) {
        d.setFont(font_sans12());
        const int pw = d.textWidth(fault) + 12;
        const int px = right_x - 10 - pw;
        d.fillSmoothRoundRect(px, lay::STATUS_H / 2 - 8, pw, 16, 8, fcol);
        d.setTextDatum(lgfx::middle_center);
        d.setTextColor(p.bg);
        d.drawString(fault, px + pw / 2, lay::STATUS_H / 2);
    }

    d.drawFastHLine(0, lay::STATUS_H - 1, lay::W, p.border_hd);   // zuletzt!
}

// ---- Fussleiste: Beschriftung der drei Tasten -----------------------------
static void draw_bar(const Model &m)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;
    d.fillRect(0, lay::BAR_Y, lay::W, lay::BAR_H, p.bg);

    const bool mos_on = m.bms.charge_mos && m.bms.discharge_mos;
    const char *lbl[3] = {
        "Seite",
        theme_mode() == ThemeMode::day ? "Nacht" : "Tag",
        mos_on ? "MOS aus" : "MOS ein",
    };

    d.setFont(font_sans16());
    d.setTextDatum(lgfx::middle_center);
    for (int i = 0; i < 3; i++) {
        // Die MOS-Taste traegt Akzentfarbe: sie ist die einzige, die etwas an
        // der Anlage veraendert.
        d.setTextColor(i == 2 ? p.accent : p.label);
        d.drawString(lbl[i], lay::BTN_CX[i], lay::BAR_Y + lay::BAR_H / 2 + 1);
    }
    d.drawFastHLine(0, lay::BAR_Y, lay::W, p.border);             // zuletzt!
}

// ---- Kachelinhalte --------------------------------------------------------
// build_tile fuellt spec + value-Puffer fuer (Seite, Platz). Die Trennung von
// Zeichnen und Bauen ist der Grund, warum die Aktualisierung billig ist: der
// Tick baut nur, vergleicht mit s_shown und zeichnet die Kachel nur bei
// Aenderung neu.
static void build_tile(const Model &m, int page, int slot, TileSpec *t, char *v, size_t vn)
{
    const BmsData &b = m.bms;
    const bool off = (b.fresh == Fresh::offline);
    t->fresh = b.fresh;
    t->zone  = Zone::none;
    t->icon  = nullptr;
    t->unit  = "";
    snprintf(v, vn, "—");
    t->value = v;

    switch (page * 10 + slot) {
        // Seite 0: rechte Spalte (Platz 0 ist die Hero-Kachel)
        case 1: t->label = "Spannung";
                if (!off) f_volt(v, vn, b.pack_dv);
                t->unit = "V";
                break;
        case 2: t->label = "Strom";
                if (!off) f_amp(v, vn, b.pack_ci);
                t->unit = "A";
                t->icon = b.flow == Flow::charge    ? ic_charge
                        : b.flow == Flow::discharge ? ic_discharge : ic_idle;
                break;

        // Seite 1: Zellbild
        case 10: t->label = "Zelle max";
                 if (!off) f_cell(v, vn, b.cell_max_mv);
                 t->unit = "mV";
                 t->zone = zone_cell_max(b.cell_max_mv);
                 break;
        case 11: t->label = "Zelle min";
                 if (!off) f_cell(v, vn, b.cell_min_mv);
                 t->unit = "mV";
                 t->zone = zone_cell_min(b.cell_min_mv);
                 break;
        case 12: t->label = "Differenz";
                 if (!off) f_int(v, vn, (long)b.cell_max_mv - (long)b.cell_min_mv);
                 t->unit = "mV";
                 // Drift ist die fruehste Warnung vor einer sterbenden Zelle.
                 if (b.cell_max_mv && b.cell_max_mv - b.cell_min_mv > 120) t->zone = Zone::critical;
                 else if (b.cell_max_mv && b.cell_max_mv - b.cell_min_mv > 60) t->zone = Zone::low;
                 break;
        case 13: t->label = "Temperatur";
                 if (!off) f_int(v, vn, b.temp_c);
                 t->unit = "\xC2\xB0" "C";
                 t->zone = zone_temp(b.temp_c);
                 t->icon = ic_temp;
                 break;

        // Seite 3: Systemwerte
        case 30: t->label = "Ladestrom";
                 f_int(v, vn, (long)((m.lim.charge_ma + 500) / 1000));
                 t->unit = "A";
                 t->fresh = Fresh::fresh;         // eigener Wert, nicht vom BMS
                 if (m.lim.charge_ma == 0) t->zone = Zone::low;
                 break;
        case 31: t->label = "Entladestrom";
                 f_int(v, vn, (long)((m.lim.discharge_ma + 500) / 1000));
                 t->unit = "A";
                 t->fresh = Fresh::fresh;
                 if (m.lim.discharge_ma == 0) t->zone = Zone::low;
                 break;
        default: t->label = "";
                 break;
    }
}

// ---- Seite 2: Zellendiagramm ---------------------------------------------
// Sechzehn Balken auf gemeinsamer, ENGER Skala. Absolut betrachtet sehen alle
// Zellen gleich aus (3,2..3,4 V) — interessant ist die Streuung. Deshalb spannt
// die Skala nur um das tatsaechliche Feld, mindestens aber 60 mV; sonst wuerde
// Rauschen wie Drift aussehen.
static constexpr int CHART_TOP = lay::GRID_Y + 34;
static constexpr int CHART_BOT = lay::BAR_Y - 20;
static constexpr int CHART_H   = CHART_BOT - CHART_TOP;

static void draw_cells(const Model &m, bool full)
{
    const Palette &p = theme_pal();
    const BmsData &b = m.bms;
    auto &d = M5.Display;

    uint32_t sig = sig_mix(b.cells_valid ? 1 : 0, b.cell_max_no | (b.cell_min_no << 8));
    for (int i = 0; i < CELL_COUNT; i++) sig = sig_mix(sig, b.cell_mv[i]);
    if (!full && sig == s_sig_cells) return;
    s_sig_cells = sig;

    d.fillRect(0, lay::GRID_Y, lay::W, lay::GRID_H, p.bg);

    if (!b.cells_valid) {
        d.setFont(font_sans16());
        d.setTextDatum(lgfx::middle_center);
        d.setTextColor(p.offline);
        d.drawString("Zellspannungen werden gelesen ...", lay::W / 2, lay::H / 2);
        return;
    }

    // ⚠️ Durchgehend int32: mit uint16_t liefe "lo -= 10" bei einer 0-mV-Zelle
    // auf 65526 unter, span wuerde negativ und ALLE Balken stuenden auf
    // Vollausschlag — im Extremfall span == 0 und Division durch null.
    // Zellen mit 0 mV sind nicht gemessen (ein 0x95-Frame kam nicht durch) und
    // gehoeren nicht in die Skala.
    int32_t lo = INT32_MAX, hi = 0;
    for (int i = 0; i < CELL_COUNT; i++) {
        const int32_t mv = b.cell_mv[i];
        if (mv <= 0) continue;
        if (mv < lo) lo = mv;
        if (mv > hi) hi = mv;
    }
    if (hi == 0 || lo == INT32_MAX) return;
    int32_t span = hi - lo;
    if (span < 60) { const int32_t add = (60 - span) / 2; lo -= add; hi += add; }
    lo -= 10; hi += 10;
    span = hi - lo;
    if (span <= 0) return;

    // Kopfzeile: wie weit die Zellen auseinanderliegen. ⚠️ Kein "∆" — der
    // Zeichensatz (gen_vlw.py) traegt nur Latin-1 plus °, · und —; ein fehlendes
    // Zeichen faellt sonst erst am Geraet als Luecke auf.
    d.fillRect(0, lay::GRID_Y, lay::W, 30, p.bg);
    d.setFont(font_sans16());
    d.setTextDatum(lgfx::middle_left);
    d.setTextColor(p.label);
    d.drawString("Streuung", 10, lay::GRID_Y + 15);
    d.setFont(font_mono20());
    d.setTextDatum(lgfx::middle_right);
    // max < min kommt bei einem verstuemmelten 0x91-Frame vor; ohne die
    // Klammer stuende dann eine sechsstellige Streuung in Rot auf der Seite.
    const int diff = (b.cell_max_mv >= b.cell_min_mv)
                   ? (int)(b.cell_max_mv - b.cell_min_mv) : 0;
    d.setTextColor(diff > 120 ? p.crit : diff > 60 ? p.warn : p.value);
    char dtxt[16];
    snprintf(dtxt, sizeof dtxt, "%d mV", diff);
    d.drawString(dtxt, lay::W - 10, lay::GRID_Y + 15);

    const int step = (lay::W - 2 * lay::PAD) / CELL_COUNT;   // 19
    const int bw   = step - 2;
    for (int i = 0; i < CELL_COUNT; i++) {
        const int x = lay::PAD + i * step;
        const int32_t mv = b.cell_mv[i];
        int h = mv <= 0 ? 0 : (int)((mv - lo) * CHART_H / span);
        if (h < 2) h = 2;
        if (h > CHART_H) h = CHART_H;

        uint16_t col = p.primary;
        if (i + 1 == b.cell_max_no) col = zone_cell_max(mv) != Zone::none ? p.warn : p.charge;
        if (i + 1 == b.cell_min_no) col = zone_cell_min(mv) == Zone::critical ? p.crit : p.discharge;

        d.fillRect(x, CHART_TOP, bw, CHART_H, p.track);
        d.fillRect(x, CHART_BOT - h, bw, h, col);

        d.setFont(font_sans12());
        d.setTextDatum(lgfx::top_center);
        d.setTextColor(i + 1 == b.cell_max_no || i + 1 == b.cell_min_no ? p.value : p.label);
        char no[4];
        snprintf(no, sizeof no, "%d", i + 1);
        d.fillRect(x, CHART_BOT + 3, bw, 15, p.bg);
        d.drawString(no, x + bw / 2, CHART_BOT + 3);
    }
}

// Nur die Versionsnummer ohne den Git-Hash: der volle PROJECT_VER-String
// ("2.0.0 (a1b2c3d)") sprengt die Zeilenbreite der Infokarte. Der Hash steht auf
// dem Startbildschirm und in Home Assistant.
static const char *fw_short()
{
    static char v[16];
    if (!v[0]) {
        strlcpy(v, esp_app_get_description()->version, sizeof v);
        char *sp = strchr(v, ' ');
        if (sp) *sp = '\0';
    }
    return v;
}

// ---- Seite 3: Infokarte unter den beiden Kacheln --------------------------
static void draw_infocard(const Model &m, bool full)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;
    const int x = lay::PAD, y = lay::tile_y(1);
    const int w = lay::W - 2 * lay::PAD, h = lay::TILE_H;

    uint32_t sig = sig_mix(m.bms.rest_mah / 100, m.bms.cycles);
    sig = sig_mix(sig, (m.net.wifi ? 1u : 0u) | (m.net.mqtt ? 2u : 0u) | (m.diag.can_ok ? 4u : 0u));
    sig = sig_mix(sig, m.diag.err_count);
    for (const char *c = m.net.ip; *c; c++) sig = sig_mix(sig, (uint8_t)*c);
    if (!full && sig == s_sig_info) return;
    s_sig_info = sig;

    d.fillSmoothRoundRect(x, y, w, h, lay::RADIUS, p.border);
    d.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, lay::RADIUS - 1, p.card);

    struct Row { const char *k; char v[64]; uint16_t col; } rows[3];
    char ah[12];
    f_ah(ah, sizeof ah, m.bms.rest_mah);
    snprintf(rows[0].v, sizeof rows[0].v, "%s Ah · %u Zyklen", ah,
             (unsigned)m.bms.cycles);
    rows[0].k = "Rest"; rows[0].col = p.value;

    if (m.net.wifi) snprintf(rows[1].v, sizeof rows[1].v, "%s · %s", m.net.ip,
                             m.net.mqtt ? "MQTT ok" : "MQTT weg");
    else            snprintf(rows[1].v, sizeof rows[1].v, "nicht verbunden");
    rows[1].k = "Netz"; rows[1].col = m.net.wifi && m.net.mqtt ? p.value : p.warn;

    // ⚠️ Bewusst OHNE die Zykluszeit: die schwankt im Sekundentakt und wuerde
    // die Karte dauernd neu zeichnen lassen. Sie steht im Log und in Home
    // Assistant, wo Schwanken nichts kostet.
    snprintf(rows[2].v, sizeof rows[2].v, "%s · %lu Fehler · %s",
             m.diag.can_ok ? "CAN ok" : "CAN aus",
             (unsigned long)m.diag.err_count, fw_short());
    rows[2].k = "Betrieb"; rows[2].col = m.diag.can_ok && !m.diag.err_count ? p.value : p.warn;

    // ⚠️ Werte LINKSBUENDIG in einer festen Spalte, NICHT rechtsbuendig an der
    // Kartenkante. Grund ist eine Eigenheit von LovyanGFX: VLWfont::drawChar
    // zeichnet das Leerzeichen mit `spaceWidth = yAdvance*2/7` — einer
    // Schaetzung —, waehrend textWidth() den echten Glyphenvorschub nimmt. Bei
    // Manrope 16 sind das 6 statt 3 px, also 3 px Fehler JE Leerzeichen. Diese
    // Zeilen haben bis zu sechs davon; rechtsbuendig gesetzt liefen sie damit
    // ~18 px ueber den Kartenrand hinaus (am Geraet nachgemessen). Linksbuendig
    // kommt keine Breitenrechnung vor und der Fehler kann nichts verschieben.
    // Aus demselben Grund stehen um die Trennpunkte EINFACHE Leerzeichen.
    const int row_h = (h - 12) / 3;
    for (int i = 0; i < 3; i++) {
        const int ry = y + 6 + i * row_h + row_h / 2;
        d.setFont(font_sans12());
        d.setTextDatum(lgfx::middle_left);
        d.setTextColor(p.label);
        d.drawString(rows[i].k, x + 10, ry);
        d.setFont(font_sans16());
        d.setTextColor(rows[i].col);
        d.drawString(rows[i].v, x + 82, ry);
    }
}

// ---- Hero (Seite 0) -------------------------------------------------------
static void draw_hero(const Model &m, bool full)
{
    const Palette &p = theme_pal();
    const BmsData &b = m.bms;
    const bool off = (b.fresh == Fresh::offline);

    uint32_t sig = sig_mix(b.soc_pm, (uint32_t)b.flow | ((uint32_t)b.fresh << 8));
    sig = sig_mix(sig, (uint32_t)watts(b));
    if (!full && sig == s_sig_hero) return;
    s_sig_hero = sig;

    static char val[10], l1[16], l2[16];
    HeroSpec h = {};
    h.label = "Ladezustand";
    h.unit  = "%";
    if (off) { snprintf(val, sizeof val, "—"); h.unit = ""; }
    else     f_soc(val, sizeof val, b.soc_pm);
    h.value = val;
    h.fill_pm = off ? 0 : b.soc_pm;
    h.zone  = off ? Zone::none : zone_soc(b.soc_pm);
    h.fresh = b.fresh;

    switch (b.flow) {
        case Flow::charge:    h.icon = ic_charge;    h.accent = p.charge;    break;
        case Flow::discharge: h.icon = ic_discharge; h.accent = p.discharge; break;
        default:              h.icon = ic_idle;      h.accent = p.idle;      break;
    }
    if (off) { h.icon = ic_offline; h.accent = p.offline; }

    // Zwei kurze Zeilen statt einer langen: die Hero-Kachel ist innen nur 130 px
    // breit, "Entladen · -1234 W" laeuft darueber hinaus. Ab 1 kW auf Kilowatt
    // umstellen, sonst wird die Zahl allein schon zu breit.
    const long w = (long)watts(b);
    snprintf(l1, sizeof l1, "%s", off ? "keine Daten" : flow_text(b.flow));
    if (off)                     l2[0] = '\0';
    else if (w > -1000 && w < 1000) snprintf(l2, sizeof l2, "%ld W", w);
    else {
        const long a = w < 0 ? -w : w;
        snprintf(l2, sizeof l2, "%s%ld.%ld kW", w < 0 ? "-" : "", a / 1000, (a % 1000) / 100);
    }
    h.line1 = l1;
    h.line2 = l2;

    hero_draw(h);
}

// ---- Seitenaufbau ---------------------------------------------------------

static void draw_page(const Model &m, bool full)
{
    const Palette &p = theme_pal();
    if (full) M5.Display.fillRect(0, lay::GRID_Y, lay::W, lay::GRID_H, p.bg);

    char v[16];
    TileSpec t;

    switch (s_page) {
        case 0:
            draw_hero(m, full);
            for (int slot = 1; slot <= 2; slot++) {
                build_tile(m, 0, slot, &t, v, sizeof v);
                if (full || strcmp(v, s_shown[slot]) != 0) {
                    strlcpy(s_shown[slot], v, sizeof s_shown[slot]);
                    if (full) tile_draw(1, slot - 1, t);
                    else      tile_update_value(1, slot - 1, t);
                }
            }
            break;

        case 1:
            for (int slot = 0; slot < 4; slot++) {
                build_tile(m, 1, slot, &t, v, sizeof v);
                if (full || strcmp(v, s_shown[slot]) != 0) {
                    strlcpy(s_shown[slot], v, sizeof s_shown[slot]);
                    if (full) tile_draw(slot % 2, slot / 2, t);
                    else      tile_update_value(slot % 2, slot / 2, t);
                }
            }
            break;

        case 2:
            draw_cells(m, full);
            break;

        case 3:
            for (int slot = 0; slot < 2; slot++) {
                build_tile(m, 3, slot, &t, v, sizeof v);
                if (full || strcmp(v, s_shown[slot]) != 0) {
                    strlcpy(s_shown[slot], v, sizeof s_shown[slot]);
                    if (full) tile_draw(slot, 0, t);
                    else      tile_update_value(slot, 0, t);
                }
            }
            draw_infocard(m, full);
            break;
    }
}

void pages_init()
{
    memset(s_shown, 0, sizeof s_shown);
    s_shown_status[0] = '\0';
}

void pages_show(int idx)
{
    s_page = ((idx % PAGE_COUNT) + PAGE_COUNT) % PAGE_COUNT;
    memset(s_shown, 0, sizeof s_shown);
    s_shown_status[0] = '\0';
    s_sig_hero = s_sig_cells = s_sig_info = 0;
    screens_invalidate();      // die Seite ueberschreibt jedes Overlay

    Model m;
    model_read(&m);
    M5.Display.fillScreen(theme_pal().bg);
    draw_status(m, true);
    draw_bar(m);
    draw_page(m, true);
}

// ---- Tasten ---------------------------------------------------------------
// M5Unified liefert wasPressed/wasReleased/pressedFor. Die Halte-Bestaetigung
// fuer die MOSFETs braucht den Zwischenzustand, deshalb der eigene Zaehler:
// solange C gedrueckt ist, waechst der Ring; wer loslaesst, hat abgebrochen.
static constexpr uint32_t MOS_HOLD_MS = 1500;

static void handle_buttons(const Model &m, uint32_t ms)
{
    if (M5.BtnA.wasPressed()) {
        pages_show(s_page + 1);
        return;
    }

    // ⚠️ Langer Druck NUR mit wasReleaseFor prüfen. pressedFor() verlangt, dass
    // die Taste GERADE gedrueckt ist — innerhalb von wasReleased() ist sie das
    // nie, die Abfrage waere immer falsch und der Signalton nie umschaltbar.
    // wasReleaseFor liefert die Haltedauer des gerade beendeten Drucks.
    if (M5.BtnB.wasReleaseFor(1000)) {
        Model *w = model_lock();
        w->diag.beep = !w->diag.beep;
        const bool on = w->diag.beep;
        model_unlock();
        model_notice(on ? "Signalton ein" : "Signalton aus");
        return;
    }
    if (M5.BtnB.wasReleased()) {
        theme_toggle();
        pages_show(s_page);
        return;
    }

    // C: halten. s_confirm_done sperrt bis zum Loslassen — ohne die Sperre
    // startet der Balken sofort neu und ein Dauerdruck schaltet die MOSFETs
    // alle 1,5 Sekunden erneut.
    if (M5.BtnC.isPressed()) {
        if (s_confirm_done) return;
        if (!s_confirm) { s_confirm = true; s_confirm_ms = ms; }
        const uint32_t held = ms - s_confirm_ms;
        const bool mos_on = m.bms.charge_mos && m.bms.discharge_mos;
        screen_confirm_mos(!mos_on, held, MOS_HOLD_MS);
        if (held >= MOS_HOLD_MS) {
            cmd_post(mos_on ? CmdType::mos_off : CmdType::mos_on);
            s_confirm      = false;
            s_confirm_done = true;
            pages_show(s_page);
        }
        return;
    }
    if (s_confirm || s_confirm_done) {
        const bool overlay_stood = s_confirm;   // vorzeitig losgelassen
        s_confirm = s_confirm_done = false;
        if (overlay_stood) pages_show(s_page);
    }
}

// ---- Tick -----------------------------------------------------------------

void pages_tick(uint32_t ms)
{
    Model m;
    model_read(&m);

    // Ein laufendes Update haengt alles andere ab. Beim Verlassen (nur nach
    // einem FEHLGESCHLAGENEN Update — sonst startet das Geraet neu) muss die
    // Seite komplett zurueckkommen.
    static bool was_ota;
    if (ota_active()) { was_ota = true; screen_ota(ota_percent()); return; }
    if (was_ota) { was_ota = false; pages_show(s_page); return; }

    handle_buttons(m, ms);
    if (s_confirm) return;                 // Bestaetigung liegt ueber der Seite

    // Kurzmeldung: drei Sekunden ueber der Fussleiste. Solange sie steht, ruhen
    // die Wertaktualisierungen — sonst schoebe sich eine Kachel darueber und die
    // Meldung waere halb ueberdeckt.
    const bool want_toast = m.diag.notice[0] && (ms - m.diag.notice_ms) < 3000;
    if (want_toast) {
        screen_toast(m.diag.notice);
        s_toast_on = true;
        return;
    }
    if (s_toast_on) {
        s_toast_on = false;
        pages_show(s_page);
        return;
    }

    static uint32_t last_data;
    if (ms - last_data >= 200) {
        last_data = ms;
        draw_status(m, false);
        draw_page(m, false);

        // Fussleiste nur neu zeichnen, wenn sich die MOS-Beschriftung aendert.
        static int last_mos = -1;
        const int mos = (m.bms.charge_mos && m.bms.discharge_mos) ? 1 : 0;
        if (mos != last_mos) { last_mos = mos; draw_bar(m); }
    }
}
