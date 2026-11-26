/* VibeosTopBar: Material 3's small top app bar, with its large and contextual forms.
 *
 * A title on one line, ellipsized, and icon buttons at the end. A page under another has a back button at the start:
 * an arrow in a tonal circle, a larger circle on a tablet; a tap on it emits "back". The bar has no shadow and no
 * fill of its own unless given one (vibeos_top_bar_set_fill), so it takes the page's colour.
 *
 * Large: the title starts big, first in the page's content (vibeos_top_bar_get_large_title: the app puts it at the
 * top of what scrolls), and as the content scrolls (the adjustment given to vibeos_top_bar_set_scroll) it goes under
 * the bar, fading, and the bar's small title comes in. The big title takes up to three lines: smaller first, as
 * VibeosFitLabel draws it, then ellipsized. Its band grows with its lines, as Android Settings' collapsing title does.
 *
 * Contextual (selection mode, vibeos_top_bar_set_selection): the back button becomes a close button, which emits
 * "selection-closed", the title says what is selected, and the selection's actions replace the usual ones. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The buttons at the top of a screen are one size in every app, the Calculator's: a 48 dp touch target round a 24 dp
 * glyph (vibeos_icon_button_new), the glyph from Material Symbols (libvibeos has the common ones: vibeos-more-symbolic,
 * vibeos-refresh-symbolic, vibeos-menu-symbolic; Adwaita's view-more and view-refresh are a size larger), side by side, centred on the bar's height, the last one's glyph VIBEOS_TOP_BAR_END_CENTRE dp from the
 * bar's end: where Android puts the overflow ⋮, its target running past the edge. The top bar puts its actions so; a
 * bar an app lays out itself gives its last button the CSS class vibeos-top-bar-end, which does the same. */
#define VIBEOS_TOP_BAR_HEIGHT     64
#define VIBEOS_TOP_BAR_END_CENTRE (66.5 / 3)

#define VIBEOS_TYPE_TOP_BAR (vibeos_top_bar_get_type ())
G_DECLARE_FINAL_TYPE (VibeosTopBar, vibeos_top_bar, VIBEOS, TOP_BAR, GtkWidget)

GtkWidget *vibeos_top_bar_new           (const char *title);
void       vibeos_top_bar_set_title     (VibeosTopBar *self, const char *title);
/* the back button at the start */
void       vibeos_top_bar_set_back      (VibeosTopBar *self, gboolean back);
/* an icon button at the end, after those added before; returns it (a VibeosPressable: connect to "clicked") */
GtkWidget *vibeos_top_bar_add_action    (VibeosTopBar *self, const char *icon_name);
/* the lower bar settings pages have, rather than the usual one */
void       vibeos_top_bar_set_compact   (VibeosTopBar *self, gboolean compact);
/* a fill behind the bar (NULL: none, the default) */
void       vibeos_top_bar_set_fill      (VibeosTopBar *self, const GdkRGBA *fill);

/* the large title; it goes under the bar as `vadjustment` scrolls (NULL: the small title never shows) */
void       vibeos_top_bar_set_large     (VibeosTopBar *self, gboolean large);
void       vibeos_top_bar_set_scroll    (VibeosTopBar *self, GtkAdjustment *vadjustment);
/* the large title, for the app to put first in the content that `vadjustment` scrolls; shown while the bar is large */
GtkWidget *vibeos_top_bar_get_large_title (VibeosTopBar *self);

/* selection mode with `title` ("3 selected"), or out of it with NULL */
void       vibeos_top_bar_set_selection (VibeosTopBar *self, const char *title);
/* an icon button shown in selection mode only */
GtkWidget *vibeos_top_bar_add_selection_action (VibeosTopBar *self, const char *icon_name);

/* Signals: "back" (), "selection-closed" () */

G_END_DECLS
