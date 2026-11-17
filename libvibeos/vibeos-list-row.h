/* VibeosListRow: one row of a settings-style list, on its card.
 *
 * A title, an optional summary under it, something at the start and something at the end:
 *
 *   start   nothing, an outlined icon, an icon in a coloured circle, or an avatar (a picture or initials in a circle)
 *   end     nothing, a switch, a chevron, a chevron and a switch with a divider between them (the row opens a page,
 *           the switch toggles on its own), a value, or another icon (an external link's arrow: the row opens a
 *           web page)
 *
 * The text starts at one of two insets, the farther one when there is something at the start. The row is as tall as
 * its lines need, never less than a one-line row: one, two and three lines give the three heights of the list.
 *
 * It draws its own card: rows in a VibeosCardList have their outer corners rounded where their group starts and
 * ends and small corners between, which the list sets (vibeos_list_row_set_corners). A tap shows the ripple and emits
 * "clicked"; on a row with a switch it also toggles the switch. A row can be selected (the item whose page shows
 * beside the list): a pill in the colour of the pane beside it, and its title bolder. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_LIST_ROW (vibeos_list_row_get_type ())
G_DECLARE_FINAL_TYPE (VibeosListRow, vibeos_list_row, VIBEOS, LIST_ROW, GtkWidget)

GtkWidget  *vibeos_list_row_new          (const char *title, const char *summary);
void        vibeos_list_row_set_title    (VibeosListRow *self, const char *title);
/* NULL: no summary, a shorter row */
void        vibeos_list_row_set_summary  (VibeosListRow *self, const char *summary);

/* the start: an outlined icon in the text's colour */
void        vibeos_list_row_set_icon     (VibeosListRow *self, const char *icon_name);
/* the start: an icon in a filled circle */
void        vibeos_list_row_set_icon_circle (VibeosListRow *self, const char *icon_name, const GdkRGBA *fill,
                                             const GdkRGBA *glyph);
/* the start: a picture in a circle, or without one the initials on `fill` */
void        vibeos_list_row_set_avatar   (VibeosListRow *self, GdkPaintable *picture, const char *initials,
                                          const GdkRGBA *fill);
/* the start: nothing */
void        vibeos_list_row_clear_start  (VibeosListRow *self);
/* the text's inset from the card's edge, in px, instead of the one the start gives (-1: back to that) */
void        vibeos_list_row_set_text_inset (VibeosListRow *self, int inset);

/* the end: a switch (returned: a VibeosSwitch, to read and connect), alone or after a chevron and a divider */
GtkWidget  *vibeos_list_row_set_switch   (VibeosListRow *self, gboolean split);
/* the end: a chevron */
void        vibeos_list_row_set_chevron  (VibeosListRow *self);
/* the end: an icon in the chevron's place, such as vibeos-external-link-symbolic for a row that opens a web page */
void        vibeos_list_row_set_end_icon (VibeosListRow *self, const char *icon_name);
/* the end: a value, in the summary's colour */
void        vibeos_list_row_set_value    (VibeosListRow *self, const char *value);
/* the end: nothing */
void        vibeos_list_row_clear_end    (VibeosListRow *self);

void        vibeos_list_row_set_selected (VibeosListRow *self, gboolean selected);
gboolean    vibeos_list_row_get_selected (VibeosListRow *self);

/* the card's corner radii at the top and at the bottom (a row alone has its outer corners at both) */
void        vibeos_list_row_set_corners  (VibeosListRow *self, double top, double bottom);
/* the exact height the row has at `width` (its allocation is that, rounded up to a whole px) */
double      vibeos_list_row_get_exact_height (VibeosListRow *self, int width);

/* Signals: "clicked" () */

G_END_DECLS
