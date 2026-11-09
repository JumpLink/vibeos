/* CalcPad: one of the two keypads, a ConstraintLayout of HapticButtons between percent guidelines
 * (advanced_pad.xml, input_pad.xml and their layout-land versions).
 *
 * The guidelines sit at their percent of the pad's content box (the pad less its padding), rounded to the device
 * pixel, as ConstraintLayout places them; a key fills the cell between two guidelines, less its horizontal margin
 * on each side (button_horizontal_margin 4 dp, button_small_horizontal_margin 2 dp). Measured on still:calc-main:
 * the input pad's rows start at 1222, 1431, 1640, 1850, 2059 px, which is 1094 + 209.2 n rounded.
 *
 * GTK allocates whole logical pixels (3 device pixels on the phone), so a key gets the next whole size up, moved
 * to its exact place, and is told its exact size to draw (calc_button_set_exact_size). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_PAD (calc_pad_get_type ())
G_DECLARE_FINAL_TYPE (CalcPad, calc_pad, CALC, PAD, GtkWidget)

/* the guidelines, as fractions: n_columns - 1 vertical ones and n_rows - 1 horizontal ones */
GtkWidget *calc_pad_new         (const double *column_guides, int n_columns, const double *row_guides, int n_rows);
/* dp */
void       calc_pad_set_padding (CalcPad *self, int top, int bottom, int start, int end);
/* the pad's size in dp, fractional: it is allocated the whole logical pixels around it */
void       calc_pad_set_exact_size (CalcPad *self, double width, double height);
/* puts a key in the cell (column, row), `margin` dp in from both sides */
void       calc_pad_attach      (CalcPad *self, GtkWidget *child, int column, int row, int margin);

G_END_DECLS
