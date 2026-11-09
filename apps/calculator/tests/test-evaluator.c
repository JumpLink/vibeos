/* the evaluator: results computed in the background and delivered on the main loop, more digits
 * on request, the history database, memory, and the main expression across starts */
#include "evaluator.h"
#include "expression-db.h"

#include <locale.h>
#include <string.h>

#define SQRT2_100 "1.4142135623730950488016887242096980785696718753769480731766797379907324784621070388503875343276415727"

/* --- what the listener heard */

typedef struct {
  int         evaluated, reevaluated, cancelled, errors;
  CalcFailure failure;
  int         init_prec_offset, msd_index, lsd_offset;
  char       *whole;
  long        index;
} Heard;

static Heard heard;
static int   shown_cancelled, shown_timeout, memory_changed;

static void
on_cancelled (gpointer data, long index)
{
  heard.cancelled++;
  heard.index = index;
}

static void
on_error (gpointer data, long index, CalcFailure failure)
{
  heard.errors++;
  heard.failure = failure;
  heard.index = index;
}

static void
on_evaluate (gpointer data, long index, int init_prec_offset, int msd_index, int lsd_offset, const char *whole)
{
  heard.evaluated++;
  heard.index = index;
  heard.init_prec_offset = init_prec_offset;
  heard.msd_index = msd_index;
  heard.lsd_offset = lsd_offset;
  g_free (heard.whole);
  heard.whole = g_strdup (whole);
}

static void
on_reevaluate (gpointer data, long index)
{
  heard.reevaluated++;
}

static const EvaluationListenerFuncs funcs = { on_cancelled, on_error, on_evaluate, on_reevaluate };
static const EvaluationListener listener = { &funcs, NULL, NULL };

static void
cb_memory (gpointer data)
{
  memory_changed++;
}

static void
cb_cancelled (gpointer data)
{
  shown_cancelled++;
}

static void
cb_timeout (gpointer data, gboolean long_timeout)
{
  shown_timeout++;
}

static const EvaluatorCallback callback = { cb_memory, cb_cancelled, cb_timeout };

static void
metrics (gpointer data, CalcCharMetrics *m)
{
  memset (m, 0, sizeof *m);
  m->max_chars = 14;
  m->grouping_separator_width_ratio = 0.5f;
  m->decimal_credit = 0.5f;
  m->no_ellipsis_credit = 1.0f;
}

static void
forget (void)
{
  g_free (heard.whole);
  memset (&heard, 0, sizeof heard);
  shown_cancelled = shown_timeout = memory_changed = 0;
}

static gboolean
give_up (gpointer data)
{
  g_error ("nothing heard in 30 s");
  return G_SOURCE_REMOVE;
}

/* runs the main loop until *counter is at least n */
static void
wait_for (int *counter, int n)
{
  guint guard = g_timeout_add_seconds (30, give_up, NULL);

  while (*counter < n)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (guard);
}

/* runs what is pending, for jobs that should report nothing */
static void
settle (void)
{
  for (int i = 0; i < 50; i++) {
    g_usleep (2000);
    while (g_main_context_iteration (NULL, FALSE))
      ;
  }
}

static void
type (Evaluator *ev, const char *keys)
{
  for (const char *s = keys; *s; s = g_utf8_next_char (s)) {
    gunichar c = g_utf8_get_char (s);
    CalcKey key = c == 0x221A ? CALC_KEY_OP_SQRT : calc_key_for_char (c);
    g_assert_cmpint (key, !=, CALC_KEY_NONE);
    evaluator_append (ev, key);
  }
}

static Evaluator *
fresh (const char *db)
{
  Evaluator *ev = evaluator_new (db, NULL);

  evaluator_set_callback (ev, &callback, NULL);
  evaluator_clear_everything (ev);
  forget ();
  return ev;
}

/* --- the tests */

static void
test_exact_result (void)
{
  Evaluator *ev = fresh (":memory:");

  type (ev, "1÷3×3");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpint (heard.errors, ==, 0);
  g_assert_cmpstr (heard.whole, ==, "1");
  g_assert_cmpint (heard.lsd_offset, ==, -1);
  g_assert_cmpint (heard.init_prec_offset, ==, -1);

  int prec = heard.init_prec_offset;
  gboolean truncated, negative;
  g_autofree char *s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 10000, 14, &truncated, &negative, &listener);
  g_assert_cmpstr (s, ==, "1");
  g_assert_false (negative);
  BoundedRational *r = real_rational_value (evaluator_get_result (ev, EVALUATOR_MAIN_INDEX));
  g_assert_nonnull (r);
  g_assert_true (rational_equals_long (r, 1));

  /* asked again, it answers at once */
  forget ();
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  g_assert_cmpint (heard.evaluated, ==, 1);
  evaluator_free (ev);
}

static void
test_more_digits (void)
{
  Evaluator *ev = fresh (":memory:");

  type (ev, "√2");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpstr (heard.whole, ==, "1");
  g_assert_cmpint (heard.lsd_offset, ==, G_MAXINT);
  g_assert_cmpint (heard.init_prec_offset, >, 5);

  /* the first screenful is there at once */
  int prec = heard.init_prec_offset;
  gboolean truncated, negative;
  g_autofree char *s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 10000, 14, &truncated, &negative, &listener);
  g_assert_cmpint (strlen (s), ==, 2 + prec);
  g_assert_true (strncmp (s, SQRT2_100, strlen (s)) == 0);
  g_assert_false (truncated);

  /* scrolled to the 100th decimal: blanks first, then the digits */
  int want = 100;
  prec = want;
  g_free (s);
  s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 10000, 14, &truncated, &negative, &listener);
  g_assert_cmpint (prec, ==, want);
  g_assert_cmpint (strlen (s), ==, 14);
  g_assert_true (truncated);
  if (strchr (s, ' ')) {
    wait_for (&heard.reevaluated, 1);
    g_free (s);
    prec = want;
    s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 10000, 14, &truncated, &negative, &listener);
  }
  g_assert_null (strchr (s, ' '));
  g_assert_cmpstr (s, ==, SQRT2_100 + strlen (SQRT2_100) - 14);

  /* far out, the digits keep coming */
  for (int target = 1000; target <= 4000; target += 1000) {
    forget ();
    prec = target;
    g_free (s);
    s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 100000, 14, &truncated, &negative, &listener);
    if (strchr (s, ' ')) {
      wait_for (&heard.reevaluated, 1);
      g_free (s);
      prec = target;
      s = evaluator_get_string (ev, EVALUATOR_MAIN_INDEX, &prec, 100000, 14, &truncated, &negative, &listener);
    }
    g_assert_null (strchr (s, ' '));
    g_assert_cmpint (strlen (s), ==, 14);
  }
  /* the 4000th decimals of √2 checked against a direct computation */
  g_autofree char *direct = real_to_string_truncated (evaluator_get_result (ev, EVALUATOR_MAIN_INDEX), 4000);
  g_assert_cmpstr (s, ==, direct + strlen (direct) - 14);
  evaluator_free (ev);
}

static void
test_errors (void)
{
  Evaluator *ev = fresh (":memory:");

  type (ev, "1÷0");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.errors, 1);
  g_assert_cmpint (heard.failure, ==, CALC_DIVIDE_BY_ZERO);
  g_assert_cmpstr (calc_failure_message (heard.failure), ==, "Can't divide by 0");
  g_assert_true (evaluator_has_result (ev, EVALUATOR_MAIN_INDEX));   /* the erroneous one */

  /* asked again, it computes again and reports again */
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.errors, 2);

  /* as the user types, failures are quiet, but still reported to the listener */
  evaluator_clear_main (ev);
  forget ();
  type (ev, "√(−1)");
  evaluator_evaluate_and_notify (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.errors, 1);
  g_assert_cmpint (heard.failure, ==, CALC_DOMAIN_ERROR);
  g_assert_false (evaluator_has_result (ev, EVALUATOR_MAIN_INDEX));
  evaluator_free (ev);
}

static void
test_cancel_and_timeout (void)
{
  Evaluator *ev = fresh (":memory:");

  /* cancelled by the user: "Computation cancelled." */
  type (ev, "π^π^π^π");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  g_assert_true (evaluator_evaluation_in_progress (ev, EVALUATOR_MAIN_INDEX));
  g_assert_true (evaluator_cancel (ev, EVALUATOR_MAIN_INDEX, FALSE));
  g_assert_false (evaluator_evaluation_in_progress (ev, EVALUATOR_MAIN_INDEX));
  wait_for (&heard.cancelled, 1);
  g_assert_cmpint (shown_cancelled, ==, 1);
  g_assert_cmpint (heard.evaluated, ==, 0);

  /* far too big to print: treated as a timeout, with the timeout message */
  evaluator_clear_main (ev);
  forget ();
  type (ev, "10^1000000");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.cancelled, 1);
  g_assert_cmpint (shown_timeout, ==, 1);
  g_assert_cmpint (shown_cancelled, ==, 0);

  /* while typing, the same is quiet */
  evaluator_touch (ev);
  forget ();
  evaluator_evaluate_and_notify (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.cancelled, 1);
  g_assert_cmpint (shown_timeout, ==, 0);
  evaluator_free (ev);
}

static void
test_history (void)
{
  const char *db = "test-evaluator.db";
  Evaluator *ev = fresh (db);

  /* "=": the main expression goes to the history */
  type (ev, "2+3");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  long first = evaluator_preserve (ev, EVALUATOR_MAIN_INDEX, TRUE);
  g_assert_cmpint (first, ==, 1);
  g_assert_cmpint (evaluator_get_max_index (ev), ==, 1);
  g_assert_cmpint (evaluator_get_time_stamp (ev, first), >, 0);

  /* then an operator: the result, abbreviated, starts the new formula */
  evaluator_collapse (ev, first);
  g_autofree char *collapsed = calc_expr_to_string (evaluator_get_expr (ev, EVALUATOR_MAIN_INDEX));
  g_assert_cmpstr (collapsed, ==, "5");
  type (ev, "×√2");
  forget ();
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpstr (heard.whole, ==, "7");
  long second = evaluator_preserve (ev, EVALUATOR_MAIN_INDEX, TRUE);
  g_assert_cmpint (second, ==, 2);

  /* (5)×√2 squared: evaluated again through the reference, exactly */
  evaluator_collapse (ev, second);
  type (ev, "×");
  evaluator_append_expr (ev, second);
  forget ();
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  BoundedRational *r = real_rational_value (evaluator_get_result (ev, EVALUATOR_MAIN_INDEX));
  g_assert_nonnull (r);
  g_assert_true (rational_equals_long (r, 50));

  /* the history's copy of the main expression */
  evaluator_copy_main_to_history (ev);
  g_assert_true (evaluator_has_result (ev, EVALUATOR_HISTORY_MAIN_INDEX));
  evaluator_free (ev);

  /* started again: the history is still there, and evaluates */
  ev = evaluator_new (db, NULL);
  evaluator_set_callback (ev, &callback, NULL);
  forget ();
  g_assert_cmpint (evaluator_get_max_index (ev), ==, 2);
  g_autofree char *s1 = calc_expr_to_string (evaluator_get_expr (ev, 1));
  g_assert_cmpstr (s1, ==, "2+3");
  g_assert_false (evaluator_has_result (ev, 2));
  evaluator_evaluate_and_notify (ev, 2, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpint (heard.index, ==, 2);
  g_assert_cmpstr (heard.whole, ==, "7");
  int prec = 10;
  gboolean truncated, negative;
  g_autofree char *digits = evaluator_get_string (ev, 2, &prec, 10000, 14, &truncated, &negative, &listener);
  g_assert_cmpstr (digits, ==, "7.0710678118");   /* 5√2 */

  /* "Clear history" empties the table */
  evaluator_clear_everything (ev);
  g_assert_cmpint (evaluator_get_max_index (ev), ==, 0);
  evaluator_free (ev);
}

static void
test_memory (void)
{
  Evaluator *ev = fresh (":memory:");

  type (ev, "1÷4");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  evaluator_copy_to_memory (ev, EVALUATOR_MAIN_INDEX);
  g_assert_cmpint (memory_changed, ==, 1);
  long memory = evaluator_get_memory_index (ev);
  g_assert_cmpint (memory, <, EVALUATOR_HISTORY_MAIN_INDEX);   /* hidden */

  /* M+ the same: the memory becomes a new hidden expression once it is evaluated */
  evaluator_add_to_memory (ev, EVALUATOR_MAIN_INDEX);
  g_assert_cmpint (evaluator_get_memory_index (ev), ==, 0);
  wait_for (&memory_changed, 2);
  long sum = evaluator_get_memory_index (ev);
  g_assert_cmpint (sum, <, memory);
  BoundedRational *r = real_rational_value (evaluator_get_result (ev, sum));
  g_autoptr (BoundedRational) half = rational_new (1, 2);
  g_assert_cmpint (rational_compare (r, half), ==, 0);

  /* MR: the memory is appended to the formula as a result */
  evaluator_clear_main (ev);
  evaluator_append_expr (ev, sum);
  type (ev, "×4");
  forget ();
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpstr (heard.whole, ==, "2");

  /* copying a result gives the clipboard a private name */
  g_autofree char *name = evaluator_capture (ev, EVALUATOR_MAIN_INDEX);
  g_assert_nonnull (name);
  g_assert_true (g_str_has_prefix (name, "tag:calculator2.android.com,"));
  g_assert_true (evaluator_is_last_saved (ev, name));
  g_assert_false (evaluator_is_last_saved (ev, "tag:calculator2.android.com,2016-01-01:1"));
  g_assert_cmpint (evaluator_get_saved_index (ev), <, EVALUATOR_HISTORY_MAIN_INDEX);
  evaluator_free (ev);
}

static void
test_state (void)
{
  Evaluator *ev = fresh (":memory:");

  type (ev, "12+6×");
  evaluator_append (ev, CALC_KEY_FUN_SIN);
  evaluator_set_degree_mode (ev, TRUE);
  g_autoptr (GBytes) state = evaluator_save_state (ev);
  evaluator_free (ev);

  ev = fresh (":memory:");
  evaluator_restore_state (ev, state);
  g_autofree char *s = calc_expr_to_string (evaluator_get_expr (ev, EVALUATOR_MAIN_INDEX));
  g_assert_cmpstr (s, ==, "12+6×sin(");
  g_assert_true (evaluator_get_degree_mode (ev, EVALUATOR_MAIN_INDEX));
  g_assert_true (evaluator_has_trig_funcs (ev));
  type (ev, "30");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  wait_for (&heard.evaluated, 1);
  g_assert_cmpstr (heard.whole, ==, "15");
  evaluator_free (ev);
}

static void
test_freed_while_running (void)
{
  /* a job still running when the evaluator goes away must not call anything */
  Evaluator *ev = fresh (":memory:");

  type (ev, "π^π^π^π");
  evaluator_require_result (ev, EVALUATOR_MAIN_INDEX, &listener, metrics, NULL);
  evaluator_free (ev);
  settle ();
  g_assert_cmpint (heard.evaluated + heard.errors, ==, 0);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/evaluator/exact-result", test_exact_result);
  g_test_add_func ("/evaluator/more-digits", test_more_digits);
  g_test_add_func ("/evaluator/errors", test_errors);
  g_test_add_func ("/evaluator/cancel-and-timeout", test_cancel_and_timeout);
  g_test_add_func ("/evaluator/history", test_history);
  g_test_add_func ("/evaluator/memory", test_memory);
  g_test_add_func ("/evaluator/state", test_state);
  g_test_add_func ("/evaluator/freed-while-running", test_freed_while_running);
  return g_test_run ();
}
