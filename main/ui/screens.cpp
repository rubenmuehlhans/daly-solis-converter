#include "screens.h"
#include "theme.h"
#include "tiles.h"
#include "ds_icons.h"
#include <M5Unified.h>
#include <esp_app_desc.h>
#include <string.h>
#include <stdio.h>

// ---- Start ----------------------------------------------------------------
// Zeigt beim Hochlaufen, WO es haengt. Das Original hat hier Rohtexte in die
// obere linke Ecke geschrieben; wenn der CAN-Bus fehlte, sah man 60 Sekunden
// lang einen roten Bildschirm ohne Hinweis, was als Naechstes passiert.
// Merker, welcher unveraenderliche Teil eines Screens schon steht.
// screens_invalidate() setzt sie zurueck (s. screens.h).
static bool s_boot_frame, s_ota_frame, s_confirm_frame, s_confirm_on;
static int  s_ota_last = -1, s_confirm_last_fw = -1;
static char s_toast_last[48];

void screens_invalidate()
{
    s_boot_frame = s_ota_frame = s_confirm_frame = false;
    s_ota_last = s_confirm_last_fw = -1;
    s_toast_last[0] = '\0';
}

void screen_boot(const char *step, bool fail)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;

    if (!s_boot_frame) {
        s_boot_frame = true;
        d.fillScreen(p.bg);

        d.setFont(font_mono38());
        d.setTextDatum(lgfx::middle_center);
        d.setTextColor(p.primary);
        d.drawString("SolisM5", lay::W / 2, 78);

        d.setFont(font_sans16());
        d.setTextColor(p.label);
        d.drawString("DALY BMS \xC2\xB7 SOLIS Wechselrichter", lay::W / 2, 110);

        d.setFont(font_sans12());
        d.setTextColor(p.border);
        d.drawString(esp_app_get_description()->version, lay::W / 2, 132);
    }

    d.fillRect(0, 168, lay::W, 34, p.bg);
    d.setFont(font_sans16());
    d.setTextDatum(lgfx::middle_center);
    d.setTextColor(fail ? p.crit : p.value);
    d.drawString(step, lay::W / 2, 182);
}

// ---- Firmware-Update ------------------------------------------------------
void screen_ota(int percent)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;

    if (!s_ota_frame) {
        s_ota_frame = true;
        d.fillScreen(p.bg);
        d.setFont(font_sans16());
        d.setTextDatum(lgfx::middle_center);
        d.setTextColor(p.label);
        d.drawString("Firmware-Update", lay::W / 2, 74);
    }
    if (percent == s_ota_last) return;
    s_ota_last = percent;

    char t[8];
    snprintf(t, sizeof t, "%d", percent);
    d.fillRect(60, 96, 200, 52, p.bg);
    d.setFont(font_mono44());
    d.setTextDatum(lgfx::middle_center);
    d.setTextColor(p.value);
    d.drawString(t, lay::W / 2 - 12, 122);
    d.setFont(font_mono20());
    d.setTextDatum(lgfx::middle_left);
    d.setTextColor(p.unit);
    d.drawString("%", lay::W / 2 + 26, 130);

    const int bx = 40, by = 164, bw = lay::W - 80, bh = 14;
    d.fillSmoothRoundRect(bx, by, bw, bh, bh / 2, p.track);
    int fw = bw * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
    if (fw > 0 && fw < bh) fw = bh;
    if (fw > 0) d.fillSmoothRoundRect(bx, by, fw, bh, bh / 2, p.primary);

    d.setFont(font_sans12());
    d.setTextDatum(lgfx::middle_center);
    d.setTextColor(p.label);
    d.drawString("Gerät nicht ausschalten", lay::W / 2, 200);
}

// ---- MOSFET-Bestaetigung --------------------------------------------------
// Warum ueberhaupt eine Bestaetigung: Taste C hat im Original mit EINEM Druck
// die MOSFETs geschaltet — also die Hausbatterie vom Wechselrichter getrennt.
// Am Geraet vorbeizustreifen reichte. Halten macht die Absicht eindeutig.
static constexpr int CW = 268, CH = 118;

void screen_confirm_mos(bool turning_on, uint32_t held_ms, uint32_t need_ms)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;
    const int cx = (lay::W - CW) / 2, cy = (lay::H - CH) / 2;

    if (!s_confirm_frame || s_confirm_on != turning_on) {
        s_confirm_frame = true;
        s_confirm_on = turning_on;
        s_confirm_last_fw = -1;

        const uint16_t edge = turning_on ? p.charge : p.crit;
        d.fillSmoothRoundRect(cx, cy, CW, CH, lay::RADIUS + 2, edge);
        d.fillSmoothRoundRect(cx + 2, cy + 2, CW - 4, CH - 4, lay::RADIUS, p.card);

        d.pushGrayscaleImage(cx + 16, cy + 14, DS_ICON_W, DS_ICON_H,
                             turning_on ? ic_power : ic_warn,
                             lgfx::grayscale_8bit, edge, p.card);

        d.setFont(font_sans16());
        d.setTextDatum(lgfx::middle_left);
        d.setTextColor(p.value);
        d.drawString(turning_on ? "MOSFET einschalten?" : "MOSFET ausschalten?",
                     cx + 42, cy + 23);

        d.setFont(font_sans12());
        d.setTextColor(p.label);
        d.drawString(turning_on ? "Laden und Entladen werden freigegeben"
                                : "Trennt die Batterie vom Wechselrichter",
                     cx + 16, cy + 50);
        d.drawString("Taste C halten", cx + 16, cy + 70);
    }

    const int bx = cx + 16, by = cy + 86, bw = CW - 32, bh = 12;
    uint32_t h = held_ms > need_ms ? need_ms : held_ms;
    int fw = (int)((uint64_t)bw * h / need_ms);
    if (fw != s_confirm_last_fw) {
        s_confirm_last_fw = fw;
        d.fillSmoothRoundRect(bx, by, bw, bh, bh / 2, p.track);
        if (fw > 0) {
            if (fw < bh) fw = bh;
            d.fillSmoothRoundRect(bx, by, fw, bh, bh / 2,
                                  turning_on ? p.charge : p.crit);
        }
    }
}

// ---- Kurzmeldung ----------------------------------------------------------
void screen_toast(const char *text)
{
    const Palette &p = theme_pal();
    auto &d = M5.Display;
    if (strcmp(s_toast_last, text) == 0) return;   // nur bei neuem Text zeichnen
    strlcpy(s_toast_last, text, sizeof s_toast_last);

    d.setFont(font_sans16());
    const int tw = d.textWidth(text) + 28;
    const int w  = tw > lay::W - 20 ? lay::W - 20 : tw;
    const int x  = (lay::W - w) / 2, y = lay::BAR_Y - 42, h = 32;

    // ⚠️ Erst das ganze Band raeumen: eine kuerzere Meldung, die eine laengere
    // abloest, liesse sonst deren Enden links und rechts stehen.
    d.fillRect(0, y, lay::W, h, p.bg);
    d.fillSmoothRoundRect(x, y, w, h, h / 2, p.primary);
    d.setTextDatum(lgfx::middle_center);
    d.setTextColor(p.bg);
    d.drawString(text, lay::W / 2, y + h / 2);
}
