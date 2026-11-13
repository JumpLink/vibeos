/* CalcFormula: a line of text that scrolls sideways: the display's formula, and a history row's expression.
 *
 * Text placed as calc-text.h places it: at the bottom, aligned to the end, and as wide as the view or as the text,
 * whichever is wider, so a long formula scrolls sideways (a drag, a fling; no overscroll). The display's formula
 * jumps to its end whenever its text changes; a history row's starts at its beginning.
 *
 * Its text size goes down from the largest to the smallest in steps until the text fits.
 * When that changes the size, "text-size-changed" (old size) fires, and the window may play the change
 * (calc_formula_animate_size_change: the text starts at its old size and grows or shrinks to the new one about its
 * bottom end, 400 ms, accelerate-decelerate).
 *
 * Signals: "text-size-changed" (double old_size), "long-pressed" () after 400 ms held still. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_FORMULA (calc_formula_get_type ())
G_DECLARE_FINAL_TYPE (CalcFormula, calc_formula, CALC, FORMULA, GtkWidget)

/* sizes in px; min == max for a fixed size */
GtkWidget  *calc_formula_new                (double min_size, double max_size, double step);
/* px */
void        calc_formula_set_padding        (CalcFormula *self, double top, double bottom, double start, double end);
/* its height in px, fractional: it is allocated the whole logical pixels around it */
void        calc_formula_set_exact_height   (CalcFormula *self, double height);
/* whether a new text scrolls to its end (the display's formula) or stays at the start (a history row's) */
void        calc_formula_set_scroll_to_end  (CalcFormula *self, gboolean to_end);
/* `unprocessed` (may be NULL): characters pasted or typed that are not part of the expression, shown after it
 * in red */
void        calc_formula_set_text           (CalcFormula *self, const char *text, const char *unprocessed);
const char *calc_formula_get_text           (CalcFormula *self);
void        calc_formula_set_color          (CalcFormula *self, const GdkRGBA *color);
GdkRGBA     calc_formula_get_color          (CalcFormula *self);
double      calc_formula_get_text_size      (CalcFormula *self);
double      calc_formula_get_min_text_size  (CalcFormula *self);
/* the size `text` would be shown at */
double      calc_formula_get_variable_text_size (CalcFormula *self, const char *text);
void        calc_formula_animate_size_change (CalcFormula *self, double old_size);
/* where the text is drawn, in the widget's coordinates: for the floating toolbar */
void        calc_formula_get_text_bounds    (CalcFormula *self, graphene_rect_t *bounds);
/* scrolls to the end of the text */
void        calc_formula_scroll_to_end      (CalcFormula *self);

G_END_DECLS
