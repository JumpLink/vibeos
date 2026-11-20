/* vibeos-style: the colours, the font and the stylesheet every libvibeos app shares.
 *
 * Call vibeos_init () once, from the application's startup, before building a window. Give each window the "vibeos"
 * CSS class (vibeos_window_new does). Widgets that draw themselves take their colours from vibeos_color (); everything
 * styled with CSS uses the same palette under the names @vibeos_primary, @vibeos_on_surface, and so on. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The colour roles. Their values come from the theme (VibeosTheme below): the scheme of vibeos.Theme's seed, light or
 * dark. New roles are only ever added at the end. */
typedef enum {
  VIBEOS_PRIMARY,
  VIBEOS_ON_PRIMARY,
  VIBEOS_PRIMARY_CONTAINER,
  VIBEOS_ON_PRIMARY_CONTAINER,
  VIBEOS_INVERSE_PRIMARY,
  VIBEOS_SECONDARY,
  VIBEOS_ON_SECONDARY,
  VIBEOS_SECONDARY_CONTAINER,
  VIBEOS_ON_SECONDARY_CONTAINER,
  VIBEOS_TERTIARY,
  VIBEOS_ON_TERTIARY,
  VIBEOS_TERTIARY_CONTAINER,
  VIBEOS_ON_TERTIARY_CONTAINER,
  VIBEOS_ERROR,
  VIBEOS_ERROR_CONTAINER,
  VIBEOS_SURFACE,
  VIBEOS_SURFACE_CONTAINER_LOWEST,
  VIBEOS_SURFACE_CONTAINER_LOW,
  VIBEOS_SURFACE_CONTAINER,
  VIBEOS_SURFACE_CONTAINER_HIGH,
  VIBEOS_SURFACE_CONTAINER_HIGHEST,
  VIBEOS_SURFACE_BRIGHT,
  VIBEOS_ON_SURFACE,
  VIBEOS_ON_SURFACE_VARIANT,
  VIBEOS_OUTLINE,
  VIBEOS_OUTLINE_VARIANT,
  VIBEOS_INVERSE_SURFACE,
  VIBEOS_WHITE,
  /* the rest of the scheme's roles */
  VIBEOS_ON_ERROR,
  VIBEOS_ON_ERROR_CONTAINER,
  VIBEOS_BACKGROUND,
  VIBEOS_ON_BACKGROUND,
  VIBEOS_SURFACE_DIM,
  VIBEOS_SURFACE_VARIANT,
  VIBEOS_INVERSE_ON_SURFACE,
  VIBEOS_SURFACE_TINT,
  VIBEOS_SHADOW,
  VIBEOS_SCRIM,
  VIBEOS_PRIMARY_FIXED,
  VIBEOS_PRIMARY_FIXED_DIM,
  VIBEOS_ON_PRIMARY_FIXED,
  VIBEOS_ON_PRIMARY_FIXED_VARIANT,
  VIBEOS_SECONDARY_FIXED,
  VIBEOS_SECONDARY_FIXED_DIM,
  VIBEOS_ON_SECONDARY_FIXED,
  VIBEOS_ON_SECONDARY_FIXED_VARIANT,
  VIBEOS_TERTIARY_FIXED,
  VIBEOS_TERTIARY_FIXED_DIM,
  VIBEOS_ON_TERTIARY_FIXED,
  VIBEOS_ON_TERTIARY_FIXED_VARIANT,
  /* fixed colours: not from the seed, but each with a light and a dark value */
  VIBEOS_DIALOG_TITLE,                  /* a classic (non-Material) dialog's title */
  VIBEOS_CATEGORY_NETWORK,              /* Settings' category icon circles and their glyphs */
  VIBEOS_ON_CATEGORY_NETWORK,
  VIBEOS_CATEGORY_SYSTEM,
  VIBEOS_ON_CATEGORY_SYSTEM,
  VIBEOS_CALL,                          /* the Call pill and its label */
  VIBEOS_ON_CALL,
  VIBEOS_MISSED_CALL,                   /* a missed call's line in the call log */
  VIBEOS_SEARCH_CURSOR,                 /* the Settings search field's cursor */
  VIBEOS_EMOJI_TAB_SELECTED,            /* the emoji keyboard's category tabs */
  VIBEOS_EMOJI_TAB,
  VIBEOS_CATEGORY_APPS,                 /* the rest of Settings' category circles, and their glyphs */
  VIBEOS_ON_CATEGORY_APPS,
  VIBEOS_CATEGORY_NOTIFICATIONS,        /* notifications, sound */
  VIBEOS_ON_CATEGORY_NOTIFICATIONS,
  VIBEOS_CATEGORY_DISPLAY,              /* display, wallpaper and style */
  VIBEOS_ON_CATEGORY_DISPLAY,
  VIBEOS_CATEGORY_BATTERY,
  VIBEOS_ON_CATEGORY_BATTERY,
  VIBEOS_CATEGORY_SECURITY,             /* security, location, accounts */
  VIBEOS_ON_CATEGORY_SECURITY,
  VIBEOS_CATEGORY_ACCESSIBILITY,
  VIBEOS_ON_CATEGORY_ACCESSIBILITY,
  VIBEOS_N_COLORS
} VibeosColor;

/* Loads the stylesheet (with the theme's colours) for the whole display and follows the theme (below). Safe to call
 * more than once. */
void    vibeos_init        (void);

GdkRGBA vibeos_color       (VibeosColor color);
GdkRGBA vibeos_color_alpha (VibeosColor color, float alpha);
/* a + (b - a) * t, per channel: a colour between two, for colour animations */
GdkRGBA vibeos_rgba_mix    (const GdkRGBA *a, const GdkRGBA *b, float t);

/* A window with the "vibeos" class, its content on a root overlay that dialogs and snackbars can cover
 * (vibeos_window_get_overlay). With VIBEOS_SHOT=file.png set, it renders itself to that file once it shows and on each
 * SIGUSR1: a picture of the UI for testing, made offscreen, so it works behind the lock screen too. */
GtkWidget  *vibeos_window_new         (GtkApplication *app);
void        vibeos_window_set_content (GtkWindow *window, GtkWidget *content);
/* the root overlay of the window `widget` is in, or NULL */
GtkOverlay *vibeos_window_get_overlay (GtkWidget *widget);

/* Screen pixels per GTK px for `widget`: its window's scale on the screen, fractional when the screen's is (2.5,
 * 2.4), or VIBEOS_SHOT_SCALE on a test display at scale 1, so that what is rounded to whole screen pixels (sizes,
 * guidelines, centring) comes out the same in the test's picture. Before the window is on a screen, the scale GTK
 * draws it at (the fractional scale rounded up). */
double  vibeos_device_scale (GtkWidget *widget);

/* Font padding: on the reference phone a line of text is as tall as the font's bounding box (Roboto's yMax 2163 and
 * yMin -555 of 2048 units) rather than its ascent and descent (1900, -500), so its text stands 0.128 of the text
 * size taller above the text and 0.027 below it than a GtkLabel does. This adds those margins to a label whose text
 * is `size` px, for layouts measured on the reference, and pins the text right under the top one (yalign 0), where
 * the reference draws it. Centre such a label in its row with valign, not with yalign: GtkLabel rounds a centred
 * line down to the whole pixel above, up to 1 px (3 screen px) too high. */
void    vibeos_label_set_font_padding (GtkWidget *label, double size);

/* A symbolic icon from the icon theme, drawn at `size` dp in `color` (an Android icon with its tint) */
void    vibeos_snapshot_icon (GtkSnapshot *snapshot, GtkWidget *widget, const char *icon_name, double x, double y,
                           double size, const GdkRGBA *color);

/* ---- the theme ------------------------------------------------------------------------------------------------ *
 *
 * The colours above are the scheme of vibeos.Theme's seed colour, style and contrast level, light or dark as
 * org.gnome.desktop.interface color-scheme says ("prefer-dark" is dark). When either changes, libvibeos swaps the
 * stylesheet, redraws every window and emits VibeosTheme::changed: widgets that draw with vibeos_color () at draw
 * time follow by themselves; one that keeps a colour (an app's own GdkRGBA, a fill set once) re-reads it there.
 *
 * Without the vibeos.Theme schema (libvibeos on a system without VibeOS's settings) the apps are dark, in the
 * colours of vibeos.Theme's default seed.
 *
 * For tests and development, the environment overrides the settings: VIBEOS_COLOR_SCHEME=light or dark, VIBEOS_SEED
 * (#RRGGBB) and VIBEOS_CONTRAST (-1 to 1). */

typedef enum {
  VIBEOS_INIT_DEFAULT     = 0,
  VIBEOS_INIT_ALWAYS_DARK = 1 << 0,   /* dark whatever color-scheme says, as some apps are on Android */
  VIBEOS_INIT_SCALED_TEXT = 1 << 1,   /* text follows Font size and Bold text: see vibeos-text.h */
} VibeosInitFlags;

/* vibeos_init () with flags */
void    vibeos_init_full (VibeosInitFlags flags);

#define VIBEOS_TYPE_THEME (vibeos_theme_get_type ())
G_DECLARE_FINAL_TYPE (VibeosTheme, vibeos_theme, VIBEOS, THEME, GObject)

/* The process's theme. Signal: "changed" (), after the colours changed and before the redraw. */
VibeosTheme *vibeos_theme_get_default (void);
gboolean     vibeos_theme_get_dark    (VibeosTheme *theme);
/* An app's own choice of light or dark, over color-scheme: VIBEOS_SCHEME_SYSTEM (the default) follows color-scheme,
 * LIGHT and DARK keep the app in that mode. VIBEOS_COLOR_SCHEME in the environment still decides over it. A change
 * restyles and redraws every window and emits "changed", as a change of color-scheme does. */
typedef enum {
  VIBEOS_SCHEME_SYSTEM,
  VIBEOS_SCHEME_LIGHT,
  VIBEOS_SCHEME_DARK,
} VibeosScheme;

void         vibeos_theme_set_scheme  (VibeosTheme *theme, VibeosScheme scheme);
VibeosScheme vibeos_theme_get_scheme  (VibeosTheme *theme);
/* the seed colour, 0xRRGGBB */
guint32      vibeos_theme_get_seed    (VibeosTheme *theme);

/* The tonal palettes, by Android's names (system_accent1_200 is VIBEOS_PALETTE_ACCENT1, stop 200), in the current
 * mode: stops 0, 10, 50, 100, 200, ... 900 and 1000, tones 100 (white) down to 0 (black). */
typedef enum {
  VIBEOS_PALETTE_ACCENT1,
  VIBEOS_PALETTE_ACCENT2,
  VIBEOS_PALETTE_ACCENT3,
  VIBEOS_PALETTE_NEUTRAL1,
  VIBEOS_PALETTE_NEUTRAL2,
  VIBEOS_PALETTE_ERROR,
} VibeosPalette;

GdkRGBA vibeos_palette_stop (VibeosPalette palette, int stop);

G_END_DECLS
