/* vibeos-style: the colours, the font and the stylesheet every libvibeos app shares.
 *
 * Call vibeos_init () once, from the application's startup, before building a window. Give each window the "vibeos"
 * CSS class (vibeos_window_new does). Widgets that draw themselves take their colours from vibeos_color (); everything
 * styled with CSS uses the same palette under the names @vibeos_primary, @vibeos_on_surface, and so on. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The colour roles of the apps' dark palette (the values are in vibeos-style.c). */
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
  VIBEOS_N_COLORS
} VibeosColor;

/* Loads the stylesheet (with the palette) for the whole display. Safe to call more than once. */
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

/* Screen pixels per GTK px for `widget`: its scale factor on the phone, or VIBEOS_SHOT_SCALE on a test display at scale
 * 1, so that what is rounded to whole screen pixels (sizes, guidelines, centring) comes out the same in the test's
 * picture */
int     vibeos_device_scale (GtkWidget *widget);

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

G_END_DECLS
