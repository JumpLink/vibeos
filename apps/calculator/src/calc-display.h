/* CalcDisplay: the top of the calculator, display_two_line.xml (portrait) and display_one_line.xml (landscape).
 *
 * A toolbar (64 dp, ?actionBarSize: the angle mode, "RAD" or "DEG", and the ⋮ menu), the formula and the result
 * (one above the other, sharing what is left equally; or both in one place, landscape, only one of them shown at
 * a time), and the drag handle at the bottom (24 x 4 dp, 8 dp above and below), on display_background_color with
 * its bottom corners rounded 16 dp (rounded_bottom.xml).
 *
 * After "=" the result takes the formula's place: scaled to the formula's size about its bottom end and moved up,
 * the formula moved off the top (Calculator.onResult; LineageOS 23.2 does this without animating). */
#pragma once

#include "calc-formula.h"
#include "calc-result.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_DISPLAY (calc_display_get_type ())
G_DECLARE_FINAL_TYPE (CalcDisplay, calc_display, CALC, DISPLAY, GtkWidget)

GtkWidget   *calc_display_new            (gboolean one_line);
CalcFormula *calc_display_get_formula    (CalcDisplay *self);
CalcResult  *calc_display_get_result     (CalcDisplay *self);
/* the ⋮ button ("clicked") */
GtkWidget   *calc_display_get_overflow   (CalcDisplay *self);
gboolean     calc_display_is_one_line    (CalcDisplay *self);
/* "RAD" or "DEG" */
void         calc_display_set_mode       (CalcDisplay *self, const char *mode);
/* its size in dp, fractional: it is allocated the whole logical pixels around it */
void         calc_display_set_exact_size (CalcDisplay *self, double width, double height);
/* onResult: the result scaled by `scale` into the formula's place, the formula gone */
void         calc_display_show_result    (CalcDisplay *self, double scale);
/* restoreDisplayPositions */
void         calc_display_restore_positions (CalcDisplay *self);
/* the one-line display's visibilities (setState) */
void         calc_display_set_visible    (CalcDisplay *self, gboolean formula, gboolean result);

G_END_DECLS
