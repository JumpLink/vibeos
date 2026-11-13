/* evaluator: the expressions, their results and the digits computed so far; ported from
 * ExactCalculator's Evaluator.
 *
 * It keeps a table of expressions by index: 0 is the main expression (the formula), -1 a copy of it
 * that the history shows, 1, 2, ... the history, and -11 and below hidden ones (for the memory and
 * the clipboard). Saved ones live in expression-db.
 *
 * Evaluation happens on one worker thread, one computation at a time. A computation gets copies of
 * everything it needs and gives back its results to the main thread, where all the evaluator's
 * state lives: nothing here is touched by two threads. Results come back through an
 * EvaluationListener, on the main thread.
 *
 * For each result it caches a string of digits ("0.33333333333333333333333333333") with the number
 * of decimals in it; evaluator_get_string cuts the part the display shows out of it, and asks for
 * more digits in the background when the display gets close to the end of what is there. */
#pragma once

#include "calc-expr.h"
#include "calc-failure.h"
#include "calc-format.h"

#include <gio/gio.h>

#define EVALUATOR_MAIN_INDEX          0
#define EVALUATOR_HISTORY_MAIN_INDEX -1

typedef struct _Evaluator Evaluator;

/* EvaluationListener: called on the main thread */
typedef struct {
  void (*on_cancelled) (gpointer data, long index);
  void (*on_error) (gpointer data, long index, CalcFailure failure);
  void (*on_evaluate) (gpointer data, long index, int init_prec_offset, int msd_index, int lsd_offset,
                       const char *truncated_whole_part);
  void (*on_reevaluate) (gpointer data, long index);
} EvaluationListenerFuncs;

typedef struct {
  const EvaluationListenerFuncs *funcs;
  gpointer                       data;
  /* if set, the listener is not called after this object is finalized (the history rows come and
   * go while their results are computed) */
  GObject                       *owner;
} EvaluationListener;

/* the measurements of the display a result is computed for (CharMetricsInfo); NULL for a
 * computation no display waits for */
typedef void (*CharMetricsFunc) (gpointer data, CalcCharMetrics *metrics);

/* Evaluator.Callback */
typedef struct {
  void (*memory_state_changed) (gpointer data);
  /* "Computation cancelled." */
  void (*show_cancelled) (gpointer data);
  /* "Value may be infinite or undefined.", with "Use longer timeouts" unless they are on */
  void (*show_timeout) (gpointer data, gboolean long_timeout);
} EvaluatorCallback;

/* settings: degree-mode, memory-index, saved-index, saved-name; NULL keeps them in memory only */
Evaluator   *evaluator_new (const char *db_path, GSettings *settings);
void         evaluator_free (Evaluator *self);
void         evaluator_set_callback (Evaluator *self, const EvaluatorCallback *callback, gpointer data);

long         evaluator_get_min_index (Evaluator *self);
long         evaluator_get_max_index (Evaluator *self);
CalcExpr    *evaluator_get_expr (Evaluator *self, long index);
gboolean     evaluator_get_degree_mode (Evaluator *self, long index);
void         evaluator_set_degree_mode (Evaluator *self, gboolean degree_mode);
/* the result, if computed; not a new reference */
UnifiedReal *evaluator_get_result (Evaluator *self, long index);
gint64       evaluator_get_time_stamp (Evaluator *self, long index);
gboolean     evaluator_has_trig_funcs (Evaluator *self);

/* evaluates speculatively, as the user types: no message if it fails or takes too long */
void         evaluator_evaluate_and_notify (Evaluator *self, long index, const EvaluationListener *listener,
                                            CharMetricsFunc metrics, gpointer metrics_data);
/* evaluates because the result is needed ("="): errors and timeouts are reported */
void         evaluator_require_result (Evaluator *self, long index, const EvaluationListener *listener,
                                       CharMetricsFunc metrics, gpointer metrics_data);
gboolean     evaluator_has_result (Evaluator *self, long index);
gboolean     evaluator_evaluation_in_progress (Evaluator *self, long index);
gboolean     evaluator_cancel (Evaluator *self, long index, gboolean quiet);
void         evaluator_cancel_all (Evaluator *self, gboolean quiet);
void         evaluator_cancel_non_main (Evaluator *self);
void         evaluator_set_long_timeout (Evaluator *self);

/* the digits ending at *prec_offset (adjusted to what can be shown), at most max_digs of them,
 * ' ' for digits still being computed (getString). Sets *truncated if digits on the left are cut
 * off, *negative for a negative result. */
char        *evaluator_get_string (Evaluator *self, long index, int *prec_offset, int max_prec_offset,
                                   int max_digs, gboolean *truncated, gboolean *negative,
                                   const EvaluationListener *listener);

/* editing the main expression */
gboolean     evaluator_append (Evaluator *self, CalcKey key);
void         evaluator_delete (Evaluator *self);
void         evaluator_clear_main (Evaluator *self);
/* history and memory and all saved expressions go */
void         evaluator_clear_everything (Evaluator *self);
/* the main expression becomes the result of history entry index ("=" then an operator) */
void         evaluator_collapse (Evaluator *self, long index);
/* the main expression's value may have changed without an edit */
void         evaluator_touch (Evaluator *self);
/* s[begin, end) is a pasted "E-12": the exponent of the constant being typed */
void         evaluator_add_exponent (Evaluator *self, const gunichar *s, int begin, int end);
/* appends a saved expression's result to the main expression (a paste, MR) */
void         evaluator_append_expr (Evaluator *self, long index);

/* saves the expression at index (with its result) to the history, or hidden; returns its index */
long         evaluator_preserve (Evaluator *self, long index, gboolean in_history);
void         evaluator_represerve (Evaluator *self);
void         evaluator_copy_main_to_history (Evaluator *self);

long         evaluator_get_memory_index (Evaluator *self);
long         evaluator_get_saved_index (Evaluator *self);
void         evaluator_copy_to_memory (Evaluator *self, long index);
void         evaluator_add_to_memory (Evaluator *self, long index);
void         evaluator_subtract_from_memory (Evaluator *self, long index);
/* saves the result at index for the clipboard; returns the name the clipboard carries, or NULL */
char        *evaluator_capture (Evaluator *self, long index);
gboolean     evaluator_is_last_saved (Evaluator *self, const char *name);

/* the main expression, its degree mode and timeout, as saved across starts (saveInstanceState) */
GBytes      *evaluator_save_state (Evaluator *self);
void         evaluator_restore_state (Evaluator *self, GBytes *state);

/* the message an error is shown with */
const char  *calc_failure_message (CalcFailure failure);
