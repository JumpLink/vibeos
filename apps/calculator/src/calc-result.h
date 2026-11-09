/* CalcResult: the result line, ExactCalculator's CalculatorResult (the display's, and each history row's).
 *
 * A window onto the result's endless decimal string (calc-format.h): it asks the evaluator for the digits it
 * shows, lays them out in whole characters with an exponent where one is needed, and scrolls sideways character
 * by character, with a drag or a fling, between the result's first and last digit (or forever, for an irrational
 * one: more digits are computed as it goes). Measured like the Java one, in device pixels: the widest digit, the
 * point, the minus, the ellipsis, all the credits of CharMetricsInfo.
 *
 * It is the listener of its own evaluations unless another one is set (the window listens to the main result's),
 * and it starts one itself when it is first laid out, as onLayout does: SHOULD_EVALUATE as the user types,
 * SHOULD_REQUIRE when a result is needed, SHOULD_NOT_EVALUATE while the window drives it.
 *
 * Signals: "long-pressed" () after 400 ms held still on a valid result (the window opens the copy toolbar there);
 * "copied" () after calc_result_copy put the result on the clipboard; "scroll-started" () when a drag or fling
 * moves it (the toolbar goes away). */
#pragma once

#include "evaluator.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  CALC_RESULT_SHOULD_NOT_EVALUATE,
  CALC_RESULT_SHOULD_EVALUATE,
  CALC_RESULT_SHOULD_REQUIRE,
} CalcResultRequest;

#define CALC_RESULT_MAX_COPY_SIZE 1000000

#define CALC_TYPE_RESULT (calc_result_get_type ())
G_DECLARE_FINAL_TYPE (CalcResult, calc_result, CALC, RESULT, GtkWidget)

/* text size and padding in dp */
GtkWidget  *calc_result_new              (double text_size);
void        calc_result_set_padding      (CalcResult *self, double top, double bottom, double start, double end);
/* its height in dp, fractional: it is allocated the whole logical pixels around it */
void        calc_result_set_exact_height (CalcResult *self, double height);
void        calc_result_set_evaluator    (CalcResult *self, Evaluator *evaluator, long index);
/* `listener` NULL: the result itself */
void        calc_result_set_should_evaluate (CalcResult *self, CalcResultRequest request,
                                             const EvaluationListener *listener);
/* the result as its own listener (for a window that passes results on) */
const EvaluationListener *calc_result_get_listener (CalcResult *self);
/* CharMetricsFunc, with the result as its data */
void        calc_result_char_metrics     (gpointer self, CalcCharMetrics *metrics);

/* EvaluationListener */
void        calc_result_on_evaluate      (CalcResult *self, long index, int init_prec_offset, int msd_index,
                                          int lsd_offset, const char *truncated_whole_part);
void        calc_result_on_error         (CalcResult *self, long index, CalcFailure failure);
void        calc_result_on_cancelled     (CalcResult *self, long index);
void        calc_result_on_reevaluate    (CalcResult *self, long index);

void        calc_result_clear            (CalcResult *self);
void        calc_result_redisplay        (CalcResult *self);
gboolean    calc_result_is_scrollable    (CalcResult *self);
gboolean    calc_result_is_valid         (CalcResult *self);
/* what it shows, as shown ("1.4142135623…") */
const char *calc_result_get_text         (CalcResult *self);
/* the whole result up to the digit shown (getFullText), and whether that is all of it */
char       *calc_result_get_full_text    (CalcResult *self, gboolean with_separators);
gboolean    calc_result_full_text_is_exact (CalcResult *self);
/* what Copy copies: up to 100 more digits if that makes it exact (getFullCopyText) */
char       *calc_result_get_full_copy_text (CalcResult *self);

void        calc_result_set_color        (CalcResult *self, const GdkRGBA *color);
GdkRGBA     calc_result_get_color        (CalcResult *self);
double      calc_result_get_text_size    (CalcResult *self);
/* the copy toolbar's highlight over the text (highlightResult) */
void        calc_result_set_highlight    (CalcResult *self, gboolean highlight);
/* where the text is drawn, in the widget's coordinates (the toolbar's content rect) */
void        calc_result_get_text_bounds  (CalcResult *self, graphene_rect_t *bounds);
/* the padding, dp: the window scales the result about its bottom end, less this */
void        calc_result_get_padding      (CalcResult *self, double *bottom, double *end);

/* the copy toolbar's actions */
void        calc_result_memory_store     (CalcResult *self);
void        calc_result_memory_add       (CalcResult *self);
void        calc_result_memory_subtract  (CalcResult *self);
/* FALSE while the digits are still being computed (it will not copy placeholders) */
gboolean    calc_result_copy             (CalcResult *self);

/* the MIME type the clipboard carries the result's saved name in, next to its text (ClipData's URI) */
#define CALC_CLIPBOARD_TAG_MIME "application/x-de-yesman-calculator-tag"

G_END_DECLS
