/* calc-text: drawing one line of text in the calculator's own line box.
 *
 * The calculator's text is drawn by its own widgets (the formula, the result, the history rows), because their
 * sizes change with the text (the formula shrinks to fit, a result is scaled into the formula's place) and their
 * positions follow these rules, not GtkLabel's:
 *  - no font padding: the line is the font's ascent and descent, Roboto's hhea 1900 and -500 of 2048 units;
 *  - a text box takes up to the height of a capital H's top bearing out of the top padding and up to the descent
 *    out of the bottom padding.
 * So with gravity "bottom" the baseline sits at bottom - max (padding_bottom, descent), give or take a pixel's
 * rounding (calc_text_bottom_baseline has the exact rule).
 *
 * Sizes are px of the widget. Fonts: "Neo Roboto", the phone's Roboto, at the weights the
 * reference phone draws with Bold text on (+300: Regular 400 is drawn 700, Medium 500 as 800). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_FONT              "Neo Roboto"
#define CALC_WEIGHT_REGULAR    700          /* regular, made heavier by Bold text */
#define CALC_WEIGHT_MEDIUM     800          /* medium, made heavier by Bold text */

/* Roboto's vertical metrics, units of the em (2048) */
#define CALC_ASCENT            (1900 / 2048.0)
#define CALC_DESCENT           (500 / 2048.0)
#define CALC_TOP               (2163 / 2048.0)     /* yMax: the top of the line with font padding */
#define CALC_BOTTOM            (555 / 2048.0)
#define CALC_CAP_HEIGHT        (1456 / 2048.0)

/* a layout of `text` in Neo Roboto at `size` px, `weight`, with the calculator's font features (tnum, cv03,
 * cv04) */
PangoLayout *calc_text_layout (GtkWidget *widget, const char *text, double size, int weight);
/* a layout for the rest of the app's text (menus, toolbars, the toast): Neo Roboto at `size` px and `weight`,
 * without the display's font features, `spacing` em between the letters */
PangoLayout *calc_text_ui_layout (GtkWidget *widget, const char *text, double size, int weight, double spacing);
/* sets the size of a layout made by calc_text_layout */
void         calc_text_set_size (PangoLayout *layout, double size, int weight);
/* the advance width of the layout's text, px: with letter spacing, the line has half the spacing before its
 * first letter and half after its last, which Pango's leaves out */
double       calc_text_width (PangoLayout *layout);
/* the baseline of a bottom-aligned text box `height` tall with `padding_bottom`, for text of `size`, worked out
 * in whole device pixels (`scale` of them to the px) */
double       calc_text_bottom_baseline (double height, double padding_bottom, double size, int scale);
/* the natural height of a text box holding a line of `size` */
double       calc_text_view_height (double padding_top, double padding_bottom, double size, int scale);
/* draws the layout with its baseline at (x, baseline), x being its left edge (the half letter spacing before the
 * first letter included) */
void         calc_text_draw (GtkSnapshot *snapshot, PangoLayout *layout, double x, double baseline,
                             const GdkRGBA *color);

G_END_DECLS
