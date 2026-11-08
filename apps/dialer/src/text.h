/* Text as the Dialer's views draw it (spec §0.3): the two families, Google Sans Flex for the Dialer's own styles
 * and Roboto for the platform's, at the weights "Bold text" makes of them, in Android's line boxes.
 *
 * A TextView's line is as tall as the font's bounding box (includeFontPadding), not its ascent and descent, so
 * text sits lower in its box than a GtkLabel puts it. The lists measure against Android's boxes; these helpers
 * give a layout and say where its box is. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  TEXT_GSF,                     /* config_bodyFontFamily and friends: Google Sans Flex */
  TEXT_ROBOTO,                  /* sans-serif, Typeface.DEFAULT, TextAppearance.Material.* */
} TextFamily;

typedef struct {
  TextFamily family;
  double size;                  /* sp, which is dp here */
  int weight;                   /* the effective weight: 700 for 400, 800 for 500 */
} TextStyle;

/* the Dialer's styles (dialer/theme/common/res/values/text_styles.xml), at their effective weights */
#define TEXT_PRIMARY      ((TextStyle) { TEXT_GSF, 16, 700 })     /* Dialer.TextAppearance.Primary */
#define TEXT_SECONDARY    ((TextStyle) { TEXT_GSF, 14, 700 })     /* Secondary */
#define TEXT_OVERLINE     ((TextStyle) { TEXT_GSF, 14, 800 })     /* OVERLINE */

/* the rendered colours (spec §1.1) */
#define COLOR_TEXT_PRIMARY    0xf6f7fb  /* textColorPrimary, #202124 made light */
#define COLOR_TEXT_SECONDARY  0xa3a7ac  /* textColorSecondary, #5F6368 made light */
#define COLOR_ICON            0xeff0f2  /* ?colorIcon, inverted */
#define COLOR_ACCENT_TEXT     0x81b5bc  /* colorAccent as text */

GdkRGBA      text_rgb (guint32 value);

/* a single-line layout in @style; @width > 0 ellipsizes it at the end to that width */
PangoLayout *text_layout (GtkWidget *widget, TextStyle style, const char *text, double width);
void         text_layout_set_style (PangoLayout *layout, TextStyle style);
/* the height of the layout's lines in Android's box */
double       text_box_height (PangoLayout *layout, TextStyle style);
/* the layout's line width (its logical width; a wrapped layout's widest line) */
double       text_width (PangoLayout *layout);
/* draws @layout with its box's top-left at (x, box_top) */
void         text_draw (GtkSnapshot *snapshot, PangoLayout *layout, TextStyle style, double x, double box_top,
                        const GdkRGBA *color);

G_END_DECLS
