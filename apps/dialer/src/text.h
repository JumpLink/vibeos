/* Text as the Dialer's views draw it: two families, at the weights the phone's Bold text setting makes
 * of them, in line boxes with font padding.
 *
 * A line here is as tall as the font's bounding box, not its ascent and descent, so
 * text sits lower in its box than a GtkLabel puts it. The lists measure against these boxes; these helpers
 * give a layout and say where its box is. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  TEXT_GSF,                     /* the Dialer's own text */
  TEXT_ROBOTO,                  /* text in the system's styles: menus, dialogs, settings rows */
} TextFamily;

typedef struct {
  TextFamily family;
  double size;                  /* px */
  int weight;                   /* as drawn with Bold text on: 700 for a 400, 800 for a 500 */
} TextStyle;

/* the Dialer's text styles */
#define TEXT_PRIMARY      ((TextStyle) { TEXT_GSF, 16, 700 })     /* a row's main line */
#define TEXT_SECONDARY    ((TextStyle) { TEXT_GSF, 14, 700 })     /* a row's second line */
#define TEXT_OVERLINE     ((TextStyle) { TEXT_GSF, 14, 800 })     /* a section header */

/* text and icon colours */
#define COLOR_TEXT_PRIMARY    0xf6f7fb
#define COLOR_TEXT_SECONDARY  0xa3a7ac
#define COLOR_ICON            0xeff0f2
#define COLOR_ACCENT_TEXT     0x81b5bc

GdkRGBA      text_rgb (guint32 value);

/* a single-line layout in @style; @width > 0 ellipsizes it at the end to that width */
PangoLayout *text_layout (GtkWidget *widget, TextStyle style, const char *text, double width);
void         text_layout_set_style (PangoLayout *layout, TextStyle style);
/* the height of the layout's lines in that box */
double       text_box_height (PangoLayout *layout, TextStyle style);
/* the layout's line width (its logical width; a wrapped layout's widest line) */
double       text_width (PangoLayout *layout);
/* draws @layout with its box's top-left at (x, box_top) */
void         text_draw (GtkSnapshot *snapshot, PangoLayout *layout, TextStyle style, double x, double box_top,
                        const GdkRGBA *color);

G_END_DECLS
