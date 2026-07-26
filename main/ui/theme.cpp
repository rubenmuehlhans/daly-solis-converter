#include "theme.h"
#include "ds_colors.h"
#include <M5Unified.h>

static const Palette PAL_DAY = {
    .bg          = ds::background,
    .card        = ds::card,
    .card_hi     = ds::card_hi,
    .border      = ds::border,
    .border_hd   = ds::border,
    .track       = ds::track,
    .label       = ds::muted,
    .value       = ds::foreground,
    .unit        = ds::muted,
    .primary     = ds::primary,
    .accent      = ds::accent,
    .charge      = ds::charge,
    .discharge   = ds::discharge,
    .idle        = ds::idle,
    .warn        = ds::warn,
    .crit        = ds::crit,
    .crit_border = ds::crit,
    .offline     = ds::offline,
};

static const Palette PAL_NIGHT = {
    .bg          = ds::night_bg,
    .card        = ds::night_card,      // Karte = Grund, der Rand traegt
    .card_hi     = ds::night_card,
    .border      = ds::night_border,
    .border_hd   = ds::night_border_hd,
    .track       = ds::night_track,
    .label       = ds::night_muted,
    .value       = ds::night_value,
    .unit        = ds::night_unit,
    .primary     = ds::night_primary,
    .accent      = ds::night_primary,
    .charge      = ds::night_value,     // monochrom: Richtung zeigt das Icon
    .discharge   = ds::night_value,
    .idle        = ds::night_muted,
    .warn        = ds::night_crit_border,
    .crit        = ds::night_crit_value,
    .crit_border = ds::night_crit_border,
    .offline     = ds::night_unit,
};

// Tag hell genug fuer einen Kellerraum mit Fenster; Nacht so weit herunter,
// dass das Geraet neben einem Wechselrichter im Wohnraum nicht blendet.
static constexpr uint8_t BRIGHT_DAY = 200, BRIGHT_NIGHT = 24;

static ThemeMode s_mode = ThemeMode::day;

const Palette &theme_pal()  { return s_mode == ThemeMode::day ? PAL_DAY : PAL_NIGHT; }
ThemeMode      theme_mode() { return s_mode; }

void theme_set(ThemeMode m)
{
    s_mode = m;
    M5.Display.setBrightness(m == ThemeMode::day ? BRIGHT_DAY : BRIGHT_NIGHT);
}

void theme_toggle()
{
    theme_set(s_mode == ThemeMode::day ? ThemeMode::night : ThemeMode::day);
}
