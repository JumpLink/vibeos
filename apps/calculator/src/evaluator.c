/* evaluator: see evaluator.h. A port of Evaluator.java; the comments name its methods. */
#include "evaluator.h"
#include "expression-db.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

/* extra digits computed beyond what is shown, to cut down on recomputations while scrolling */
#define EXTRA_DIGITS         20
#define EXTRA_DIVISOR        5
#define PRECOMPUTE_DIGITS    30
#define PRECOMPUTE_DIVISOR   5
/* the largest precision an initial evaluation goes to looking for a first non-zero digit */
#define MAX_MSD_PREC_OFFSET  1100
/* at least this many digits are shown, even when a whole number is long */
#define MIN_DISPLAYED_DIGS   5
/* timeouts, ms: of a requested result (short and long), of a speculative one, of a history entry */
#define TIMEOUT_SHORT        2000
#define TIMEOUT_LONG         15000
#define QUICK_TIMEOUT        1000
#define NON_MAIN_TIMEOUT     100000
/* the largest results printed, in bits (getMaxResultBits); longer ones are treated as a timeout */
#define MAX_RESULT_BITS_SHORT 240000
#define MAX_RESULT_BITS_LONG  700000

/* the result string of a failed evaluation (ERRONEOUS_RESULT); compared by address */
static char ERRONEOUS_RESULT[] = "ERR";

typedef struct _Job Job;

/* ExprInfo: one expression, and what is known of its result */
typedef struct {
  CalcExpr    *expr;
  gboolean     degree_mode;
  UnifiedReal *val;                       /* the value, once computed (mVal) */
  char        *result_string;             /* the digits so far, or ERRONEOUS_RESULT, or NULL */
  int          result_string_offset;      /* decimals in result_string */
  int          result_string_offset_req;  /* decimals being computed */
  int          msd_index;                 /* index of the first significant digit, or CALC_INVALID_MSD */
  gboolean     long_timeout;
  gint64       time_stamp;
  Job         *job;                       /* the computation running for it (mEvaluator) */
} ExprInfo;

/* the jobs' way back to the evaluator, which may be gone when a job ends */
typedef struct {
  gint       ref_count;
  Evaluator *evaluator;
} EvaluatorLink;

typedef enum {
  JOB_EVALUATE,     /* AsyncEvaluator: the value and its first digits */
  JOB_REEVALUATE,   /* AsyncReevaluator: more digits of a known value */
} JobKind;

/* a copy of an expression the job may need */
typedef struct {
  CalcExpr    *expr;
  gboolean     degree_mode;
  UnifiedReal *val;
  gboolean     computed;   /* val was computed by this job */
} JobExpr;

struct _Job {
  EvaluatorLink     *link;
  JobKind            kind;
  long               index;
  gint               cancelled;   /* set by the main thread, read by the worker */
  gboolean           required;    /* the user asked for the result (=) */
  gboolean           quiet;       /* no "Computation cancelled." */
  gboolean           long_timeout;
  EvaluationListener listener;
  GWeakRef           owner;
  CharMetricsFunc    metrics_func;
  gpointer           metrics_data;
  CalcCharMetrics    metrics;     /* as measured when the job started */
  guint              timeout_id;
  GHashTable        *exprs;       /* JOB_EVALUATE: long index -> JobExpr */
  UnifiedReal       *value;       /* JOB_REEVALUATE: the value; JOB_EVALUATE: the result */
  int                prec_offset; /* JOB_REEVALUATE: the decimals wanted */
  /* the outcome */
  CalcFailure        failure;
  gboolean           too_big;     /* "timeout" without one: too big to print */
  char              *result_string;
  int                result_string_offset;
  int                init_display_offset;
};

/* a result that, when computed, becomes the memory or the clipboard's saved expression
 * (SetMemoryWhenDoneListener, SetSavedWhenDoneListener) */
typedef struct {
  Evaluator *evaluator;
  long       index;
  gboolean   memory;    /* or the saved one */
  gboolean   persist;
} SetWhenDone;

struct _Evaluator {
  GHashTable       *exprs;      /* long index -> ExprInfo */
  ExprInfo         *main_expr;
  ExpressionDB     *db;
  GSettings        *settings;
  GThreadPool      *worker;     /* one thread: computations run one at a time */
  EvaluatorLink    *link;
  long              saved_index;
  long              memory_index;
  char             *saved_name;
  gboolean          changed_value;   /* the main expression changed since its last evaluation */
  gboolean          has_trig_funcs;
  gboolean          degree_mode;     /* of the main expression, when not in settings */
  GList            *set_when_done;   /* of SetWhenDone */
  const EvaluatorCallback *callback;
  gpointer          callback_data;
};

static void job_run (gpointer data, gpointer user_data);

const char *
calc_failure_message (CalcFailure failure)
{
  switch (failure) {
  case CALC_SYNTAX_ERROR:
    return _("Bad expression");
  case CALC_DIVIDE_BY_ZERO:
    return _("Can't divide by 0");
  case CALC_PRECISION_OVERFLOW:
    return _("Infinite?");
  case CALC_ABORTED:
    return _("Aborted");
  case CALC_DOMAIN_ERROR:
  case CALC_TOO_BIG:
  case CALC_OK:
  default:
    return _("Not a number");
  }
}

/* --- the table of expressions */

static gpointer
index_key (long index)
{
  return (gpointer) (gintptr) index;
}

static void
expr_info_set_result_string (ExprInfo *ei, char *s)
{
  if (ei->result_string != ERRONEOUS_RESULT)
    g_free (ei->result_string);
  ei->result_string = s;
}

static ExprInfo *
expr_info_new (CalcExpr *expr, gboolean degree_mode)
{
  ExprInfo *ei = g_new0 (ExprInfo, 1);

  ei->expr = expr;
  ei->degree_mode = degree_mode;
  ei->msd_index = CALC_INVALID_MSD;
  return ei;
}

static void
expr_info_free (ExprInfo *ei)
{
  calc_expr_free (ei->expr);
  g_clear_pointer (&ei->val, real_unref);
  expr_info_set_result_string (ei, NULL);
  g_free (ei);
}

static ExprInfo *
lookup (Evaluator *self, long index)
{
  return g_hash_table_lookup (self->exprs, index_key (index));
}

static void
set_main_expr (Evaluator *self, ExprInfo *ei)
{
  self->main_expr = ei;
  g_hash_table_replace (self->exprs, index_key (EVALUATOR_MAIN_INDEX), ei);
}

/* the expression at index, read from the database if needed (ensureExprIsCached) */
static ExprInfo *
ensure_expr_is_cached (Evaluator *self, long index)
{
  ExprInfo *ei = lookup (self, index);
  ExpressionRow row = { 0 };

  if (ei)
    return ei;
  CalcExpr *expr = NULL;
  if (expression_db_get_row (self->db, index, &row)) {
    gsize size;
    const guint8 *data = g_bytes_get_data (row.expression, &size);
    expr = calc_expr_new_from_bytes (data, size);
  }
  /* Java would crash on a missing or unreadable row; an empty expression just evaluates to an error */
  ei = expr_info_new (expr ? expr : calc_expr_new (), row.degree_mode);
  ei->time_stamp = row.time_stamp;
  ei->long_timeout = row.long_timeout;
  expression_row_clear (&row);
  g_hash_table_insert (self->exprs, index_key (index), ei);
  return ei;
}

static gboolean
is_mutable_index (long index)
{
  return index == EVALUATOR_MAIN_INDEX || index == EVALUATOR_HISTORY_MAIN_INDEX;
}

/* --- settings (Java's SharedPreferences) */

static void
set_memory_index (Evaluator *self, long index)
{
  self->memory_index = index;
  if (self->settings)
    g_settings_set_int64 (self->settings, "memory-index", index);
  if (self->callback && self->callback->memory_state_changed)
    self->callback->memory_state_changed (self->callback_data);
}

static void
set_saved_index (Evaluator *self, long index)
{
  self->saved_index = index;
  if (self->settings)
    g_settings_set_int64 (self->settings, "saved-index", index);
}

static void set_when_evaluated (Evaluator *self, long index, gboolean memory, gboolean persist);

Evaluator *
evaluator_new (const char *db_path, GSettings *settings)
{
  Evaluator *self = g_new0 (Evaluator, 1);

  self->exprs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) expr_info_free);
  self->db = expression_db_open (db_path);
  self->settings = settings ? g_object_ref (settings) : NULL;
  self->worker = g_thread_pool_new (job_run, NULL, 1, FALSE, NULL);
  self->link = g_new0 (EvaluatorLink, 1);
  self->link->ref_count = 1;
  self->link->evaluator = self;
  self->saved_name = g_strdup ("none");
  self->degree_mode = settings && g_settings_get_boolean (settings, "degree-mode");
  set_main_expr (self, expr_info_new (calc_expr_new (), self->degree_mode));

  if (settings) {
    long saved_index = g_settings_get_int64 (settings, "saved-index");
    long memory_index = g_settings_get_int64 (settings, "memory-index");
    if (saved_index != 0 && saved_index != -1)
      set_when_evaluated (self, saved_index, FALSE, FALSE);
    if (memory_index != 0 && memory_index != -1)
      set_when_evaluated (self, memory_index, TRUE, FALSE);
    g_free (self->saved_name);
    self->saved_name = g_settings_get_string (settings, "saved-name");
  }
  return self;
}

static void
link_unref (EvaluatorLink *link)
{
  if (g_atomic_int_dec_and_test (&link->ref_count))
    g_free (link);
}

void
evaluator_free (Evaluator *self)
{
  if (!self)
    return;
  evaluator_cancel_all (self, TRUE);
  self->link->evaluator = NULL;   /* jobs still ending find no evaluator */
  link_unref (self->link);
  g_thread_pool_free (self->worker, FALSE, TRUE);
  g_list_free_full (self->set_when_done, g_free);
  g_hash_table_unref (self->exprs);
  expression_db_close (self->db);
  g_clear_object (&self->settings);
  g_free (self->saved_name);
  g_free (self);
}

void
evaluator_set_callback (Evaluator *self, const EvaluatorCallback *callback, gpointer data)
{
  self->callback = callback;
  self->callback_data = data;
}

long
evaluator_get_min_index (Evaluator *self)
{
  return expression_db_get_min_index (self->db);
}

long
evaluator_get_max_index (Evaluator *self)
{
  return expression_db_get_max_index (self->db);
}

CalcExpr *
evaluator_get_expr (Evaluator *self, long index)
{
  return ensure_expr_is_cached (self, index)->expr;
}

gboolean
evaluator_get_degree_mode (Evaluator *self, long index)
{
  return ensure_expr_is_cached (self, index)->degree_mode;
}

UnifiedReal *
evaluator_get_result (Evaluator *self, long index)
{
  return ensure_expr_is_cached (self, index)->val;
}

gint64
evaluator_get_time_stamp (Evaluator *self, long index)
{
  return ensure_expr_is_cached (self, index)->time_stamp;
}

gboolean
evaluator_has_trig_funcs (Evaluator *self)
{
  return self->has_trig_funcs;
}

void
evaluator_set_degree_mode (Evaluator *self, gboolean degree_mode)
{
  self->changed_value = TRUE;
  self->main_expr->degree_mode = degree_mode;
  if (self->settings)
    g_settings_set_boolean (self->settings, "degree-mode", degree_mode);
}

void
evaluator_set_long_timeout (Evaluator *self)
{
  self->main_expr->long_timeout = TRUE;
}

/* --- jobs */

static void
job_expr_free (JobExpr *je)
{
  calc_expr_free (je->expr);
  g_clear_pointer (&je->val, real_unref);
  g_free (je);
}

static void
job_free (Job *job)
{
  link_unref (job->link);
  g_weak_ref_clear (&job->owner);
  g_clear_pointer (&job->exprs, g_hash_table_unref);
  g_clear_pointer (&job->value, real_unref);
  g_free (job->result_string);
  g_free (job);
}

static Job *
job_new (Evaluator *self, JobKind kind, long index, const EvaluationListener *listener)
{
  Job *job = g_new0 (Job, 1);

  job->link = self->link;
  g_atomic_int_inc (&self->link->ref_count);
  job->kind = kind;
  job->index = index;
  job->listener = *listener;
  g_weak_ref_init (&job->owner, listener->owner);
  return job;
}

/* is the listener still there? */
static gboolean
job_listener_alive (Job *job)
{
  if (!job->listener.owner)
    return TRUE;
  g_autoptr (GObject) owner = g_weak_ref_get (&job->owner);
  return owner != NULL;
}

/* the metrics a computation that no display waits for uses (DummyCharMetricsInfo) */
static void
dummy_metrics (gpointer data, CalcCharMetrics *m)
{
  memset (m, 0, sizeof *m);
  m->max_chars = 8 + 10;   /* SHORT_TARGET_LENGTH + 10 */
}

/* --- the computations themselves, on the worker thread */

static CalcExpr *
job_resolver_get_expr (gpointer data, long index)
{
  JobExpr *je = g_hash_table_lookup (((Job *) data)->exprs, index_key (index));
  return je ? je->expr : NULL;
}

static gboolean
job_resolver_get_degree_mode (gpointer data, long index)
{
  JobExpr *je = g_hash_table_lookup (((Job *) data)->exprs, index_key (index));
  return je && je->degree_mode;
}

static UnifiedReal *
job_resolver_get_result (gpointer data, long index)
{
  JobExpr *je = g_hash_table_lookup (((Job *) data)->exprs, index_key (index));
  return je && je->val ? real_ref (je->val) : NULL;
}

static UnifiedReal *
job_resolver_put_result_if_absent (gpointer data, long index, UnifiedReal *result)
{
  JobExpr *je = g_hash_table_lookup (((Job *) data)->exprs, index_key (index));

  if (!je)
    return real_ref (result);
  if (!je->val) {
    je->val = real_ref (result);
    je->computed = TRUE;
  }
  return real_ref (je->val);
}

/* is the result too big to print? (isTooBig) */
static gboolean
is_too_big (Job *job, UnifiedReal *res)
{
  int max_bits = job->required ? (job->long_timeout ? MAX_RESULT_BITS_LONG : MAX_RESULT_BITS_SHORT)
                               : CALC_QUICK_MAX_RESULT_BITS;
  return real_approx_whole_number_bits_greater_than (res, max_bits);
}

/* AsyncEvaluator.doInBackground */
static void
run_evaluate (Job *job)
{
  const CalcExprResolver resolver = {
    job_resolver_get_expr, job_resolver_get_degree_mode, job_resolver_get_result,
    job_resolver_put_result_if_absent, job,
  };
  JobExpr *main = g_hash_table_lookup (job->exprs, index_key (job->index));
  g_autoptr (UnifiedReal) res = main->val ? real_ref (main->val) : NULL;

  if (!res) {
    g_autoptr (UnifiedReal) value = calc_expr_eval (main->expr, main->degree_mode, &resolver);
    if (!value)
      return;
    res = job_resolver_put_result_if_absent (job, job->index, value);
  }
  if (is_too_big (job, res)) {
    /* don't start a long uninterruptible decimal conversion */
    job->too_big = TRUE;
    return;
  }
  int prec_offset = CALC_INIT_PREC;
  g_autofree char *init_result = real_to_string_truncated (res, prec_offset);
  int msd = calc_get_msd_index_of (init_result);
  if (msd == CALC_INVALID_MSD) {
    int leading_zero_bits = real_leading_binary_zeroes (res);
    if (leading_zero_bits < CALC_QUICK_MAX_RESULT_BITS) {
      /* enough initial non-zero digits for most displays */
      prec_offset = 30 + (int) ceil (log (2.0) / log (10.0) * leading_zero_bits);
    } else {
      /* just try once more at a higher fixed precision */
      prec_offset = MAX_MSD_PREC_OFFSET;
    }
    g_free (init_result);
    init_result = real_to_string_truncated (res, prec_offset);
    msd = calc_get_msd_index_of (init_result);
  }
  if (calc_failed ())
    return;
  int lsd_offset = calc_get_lsd_offset (res, init_result, strchr (init_result, '.') - init_result);
  int init_display_offset = calc_get_preferred_prec (init_result, msd, lsd_offset, &job->metrics);
  int new_prec_offset = init_display_offset + EXTRA_DIGITS;
  if (new_prec_offset > prec_offset) {
    prec_offset = new_prec_offset;
    g_free (init_result);
    init_result = real_to_string_truncated (res, prec_offset);
  }
  job->value = real_ref (res);
  job->result_string = g_steal_pointer (&init_result);
  job->result_string_offset = prec_offset;
  job->init_display_offset = init_display_offset;
}

/* AsyncReevaluator.doInBackground */
static void
run_reevaluate (Job *job)
{
  char *digits = real_to_string_truncated (job->value, job->prec_offset);

  if (calc_failed ()) {
    g_free (digits);
    return;
  }
  job->result_string = digits;
  job->result_string_offset = job->prec_offset;
}

static gboolean job_done (gpointer data);

static void
job_run (gpointer data, gpointer user_data)
{
  Job *job = data;

  calc_clear_failure ();
  calc_set_cancel_flag (&job->cancelled);
  if (g_atomic_int_get (&job->cancelled))
    calc_fail (CALC_ABORTED);   /* cancelled while it waited its turn */
  else if (job->kind == JOB_EVALUATE)
    run_evaluate (job);
  else if (job->value)
    run_reevaluate (job);
  job->failure = calc_failure ();
  calc_set_cancel_flag (NULL);
  calc_clear_failure ();
  g_idle_add (job_done, job);
}

/* --- jobs, back on the main thread */

static void
show_timeout (Evaluator *self, gboolean long_timeout)
{
  if (self->callback && self->callback->show_timeout)
    self->callback->show_timeout (self->callback_data, long_timeout);
}

/* AsyncEvaluator.onCancelled */
static void
evaluate_cancelled (Evaluator *self, Job *job)
{
  if (!job->quiet && self->callback && self->callback->show_cancelled)
    self->callback->show_cancelled (self->callback_data);
  if (job_listener_alive (job) && job->listener.funcs->on_cancelled)
    job->listener.funcs->on_cancelled (job->listener.data, job->index);
}

/* AsyncEvaluator.onPostExecute */
static void
evaluate_done (Evaluator *self, Job *job, ExprInfo *ei)
{
  gboolean alive = job_listener_alive (job);

  /* the values computed for this and the nested expressions (putResultIfAbsent) */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, job->exprs);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    JobExpr *je = value;
    ExprInfo *target = lookup (self, (gintptr) key);
    if (je->computed && target && !target->val)
      target->val = real_ref (je->val);
  }

  if (job->too_big || job->failure == CALC_TOO_BIG) {
    /* treated as a timeout */
    if (job->required && job->index == EVALUATOR_MAIN_INDEX)
      show_timeout (self, ei->long_timeout);
    if (alive && job->listener.funcs->on_cancelled)
      job->listener.funcs->on_cancelled (job->listener.data, job->index);
    return;
  }
  if (job->failure != CALC_OK) {
    if (job->required)
      expr_info_set_result_string (ei, ERRONEOUS_RESULT);
    if (alive && job->listener.funcs->on_error)
      job->listener.funcs->on_error (job->listener.data, job->index, job->failure);
    return;
  }

  expr_info_set_result_string (ei, g_steal_pointer (&job->result_string));
  ei->result_string_offset = job->result_string_offset;
  int dot_index = strchr (ei->result_string, '.') - ei->result_string;
  g_autofree char *truncated_whole_part = g_strndup (ei->result_string, dot_index);
  /* the display may have been measured since: recheck the precision */
  int init_prec_offset = job->init_display_offset;
  ei->msd_index = calc_get_msd_index_of (ei->result_string);
  int least_dig_offset = calc_get_lsd_offset (ei->val, ei->result_string, dot_index);
  CalcCharMetrics metrics;
  if (alive && job->metrics_func)
    job->metrics_func (job->metrics_data, &metrics);
  else
    metrics = job->metrics;
  int new_init_prec_offset = calc_get_preferred_prec (ei->result_string, ei->msd_index, least_dig_offset, &metrics);
  if (new_init_prec_offset < init_prec_offset)
    init_prec_offset = new_init_prec_offset;
  if (alive && job->listener.funcs->on_evaluate)
    job->listener.funcs->on_evaluate (job->listener.data, job->index, init_prec_offset, ei->msd_index,
                                      least_dig_offset, truncated_whole_part);
}

/* AsyncReevaluator.onPostExecute */
static void
reevaluate_done (Evaluator *self, Job *job, ExprInfo *ei)
{
  gboolean alive = job_listener_alive (job);

  if (!job->result_string) {
    expr_info_set_result_string (ei, ERRONEOUS_RESULT);
    if (alive && job->listener.funcs->on_error)
      job->listener.funcs->on_error (job->listener.data, job->index, CALC_DOMAIN_ERROR);
    return;
  }
  if (!ei->result_string || ei->result_string == ERRONEOUS_RESULT
      || job->result_string_offset < ei->result_string_offset)
    return;   /* the cache changed meanwhile ("Unexpected onPostExecute timing") */
  expr_info_set_result_string (ei, calc_unflip_zeroes (ei->result_string, ei->result_string_offset,
                                                       job->result_string, job->result_string_offset));
  ei->result_string_offset = job->result_string_offset;
  if (alive && job->listener.funcs->on_reevaluate)
    job->listener.funcs->on_reevaluate (job->listener.data, job->index);
}

static gboolean
job_done (gpointer data)
{
  Job *job = data;
  Evaluator *self = job->link->evaluator;

  if (job->timeout_id)
    g_source_remove (job->timeout_id);
  job->timeout_id = 0;
  if (self) {
    ExprInfo *ei = lookup (self, job->index);
    if (g_atomic_int_get (&job->cancelled)) {
      if (job->kind == JOB_EVALUATE)
        evaluate_cancelled (self, job);
    } else if (ei && ei->job == job) {
      ei->job = NULL;
      if (job->kind == JOB_EVALUATE && job->index == EVALUATOR_MAIN_INDEX && self->changed_value) {
        /* the formula changed while it was computed. The UI cancels before every edit
         * (cancelUnrequested), so this should not happen; were it to, the value would be stale */
        job->quiet = TRUE;
        evaluate_cancelled (self, job);
      } else if (job->kind == JOB_EVALUATE)
        evaluate_done (self, job, ei);
      else
        reevaluate_done (self, job, ei);
    }
  }
  job_free (job);
  return G_SOURCE_REMOVE;
}

/* AsyncEvaluator.handleTimeout */
static gboolean
job_timed_out (gpointer data)
{
  Job *job = data;
  Evaluator *self = job->link->evaluator;

  job->timeout_id = 0;
  if (!self || g_atomic_int_get (&job->cancelled))
    return G_SOURCE_REMOVE;
  g_atomic_int_set (&job->cancelled, 1);
  ExprInfo *ei = lookup (self, job->index);
  if (ei && ei->job == job)
    ei->job = NULL;
  if (job->required && job->index == EVALUATOR_MAIN_INDEX) {
    job->quiet = TRUE;
    show_timeout (self, ei && ei->long_timeout);
  }
  return G_SOURCE_REMOVE;
}


/* --- starting computations */

static CalcExpr *
main_resolver_get_expr (gpointer data, long index)
{
  return ensure_expr_is_cached (data, index)->expr;
}

static gboolean
main_resolver_get_degree_mode (gpointer data, long index)
{
  return ensure_expr_is_cached (data, index)->degree_mode;
}

static UnifiedReal *
main_resolver_no_result (gpointer data, long index)
{
  return NULL;
}

/* copies the expression at index and every one it refers to, with what is known of their results,
 * for a job (Java let the job read them, and write the results, from its thread) */
static void
job_snapshot (Evaluator *self, Job *job, long index)
{
  /* a resolver that knows no result: get_referenced then lists everything referred to */
  const CalcExprResolver all = {
    main_resolver_get_expr, main_resolver_get_degree_mode, main_resolver_no_result, NULL, self,
  };
  ExprInfo *ei = ensure_expr_is_cached (self, index);
  g_autoptr (GArray) referenced = calc_expr_get_referenced (ei->expr, &all);

  job->exprs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) job_expr_free);
  for (int i = -1; i < (int) referenced->len; i++) {
    long ref = i < 0 ? index : g_array_index (referenced, long, i);
    ExprInfo *rei = ensure_expr_is_cached (self, ref);
    JobExpr *je = g_new0 (JobExpr, 1);
    je->expr = calc_expr_copy (rei->expr);
    je->degree_mode = rei->degree_mode;
    je->val = rei->val ? real_ref (rei->val) : NULL;
    g_hash_table_replace (job->exprs, index_key (ref), je);
  }
}

static void
clear_main_cache (Evaluator *self)
{
  g_clear_pointer (&self->main_expr->val, real_unref);
  expr_info_set_result_string (self->main_expr, NULL);
  self->main_expr->result_string_offset = self->main_expr->result_string_offset_req = 0;
  self->main_expr->msd_index = CALC_INVALID_MSD;
}

/* evaluateResult */
static void
evaluate_result (Evaluator *self, long index, const EvaluationListener *listener, CharMetricsFunc metrics,
                 gpointer metrics_data, gboolean required)
{
  ExprInfo *ei = lookup (self, index);
  Job *job = job_new (self, JOB_EVALUATE, index, listener);

  if (index == EVALUATOR_MAIN_INDEX)
    clear_main_cache (self);   /* otherwise the expression is immutable */
  job->required = required;
  job->quiet = !required || index != EVALUATOR_MAIN_INDEX;
  job->long_timeout = ei->long_timeout;
  job->metrics_func = metrics;
  job->metrics_data = metrics_data;
  (metrics ? metrics : dummy_metrics) (metrics_data, &job->metrics);
  job_snapshot (self, job, index);

  /* onPreExecute */
  guint timeout = required ? (ei->long_timeout ? TIMEOUT_LONG : TIMEOUT_SHORT) : QUICK_TIMEOUT;
  if (index != EVALUATOR_MAIN_INDEX)
    timeout = NON_MAIN_TIMEOUT;   /* it was evaluated before: it should not time out */
  job->timeout_id = g_timeout_add (timeout, job_timed_out, job);
  ei->job = job;
  g_thread_pool_push (self->worker, job, NULL);
  if (index == EVALUATOR_MAIN_INDEX)
    self->changed_value = FALSE;
}

/* notifyImmediately: a result computed before */
static void
notify_immediately (Evaluator *self, long index, ExprInfo *ei, const EvaluationListener *listener,
                    CharMetricsFunc metrics, gpointer metrics_data)
{
  int dot_index = strchr (ei->result_string, '.') - ei->result_string;
  g_autofree char *truncated_whole_part = g_strndup (ei->result_string, dot_index);
  int least_dig_offset = calc_get_lsd_offset (ei->val, ei->result_string, dot_index);
  int msd_index;
  CalcCharMetrics m;

  /* getMsdIndex */
  if (ei->msd_index != CALC_INVALID_MSD) {
    /* 0.100000... can change to 0.0999999...: correct once by one digit */
    if (ei->result_string[ei->msd_index] == '0')
      ei->msd_index++;
    msd_index = ei->msd_index;
  } else if (real_definitely_zero (ei->val)) {
    msd_index = CALC_INVALID_MSD;
  } else {
    msd_index = ei->msd_index = calc_get_msd_index_of (ei->result_string);
  }
  (metrics ? metrics : dummy_metrics) (metrics_data, &m);
  int preferred = calc_get_preferred_prec (ei->result_string, msd_index, least_dig_offset, &m);
  if (listener->funcs->on_evaluate)
    listener->funcs->on_evaluate (listener->data, index, preferred, msd_index, least_dig_offset,
                                  truncated_whole_part);
}

void
evaluator_evaluate_and_notify (Evaluator *self, long index, const EvaluationListener *listener,
                               CharMetricsFunc metrics, gpointer metrics_data)
{
  CalcCharMetrics m;

  (metrics ? metrics : dummy_metrics) (metrics_data, &m);
  if (m.max_chars == 0)
    return;   /* not measured yet: nothing was promised */
  ExprInfo *ei = ensure_expr_is_cached (self, index);
  if (ei->result_string && ei->result_string != ERRONEOUS_RESULT
      && !(index == EVALUATOR_MAIN_INDEX && self->changed_value)) {
    /* done already. (Java notifies with the main expression whatever the index: a slip that only
     * the main expression ever reaches.) */
    notify_immediately (self, index, ei, listener, metrics, metrics_data);
    return;
  }
  if (ei->job)
    return;   /* one listener per expression: this request is redundant */
  evaluate_result (self, index, listener, metrics, metrics_data, FALSE);
}

static gboolean cancel_expr (Evaluator *self, ExprInfo *ei, gboolean quiet);

void
evaluator_require_result (Evaluator *self, long index, const EvaluationListener *listener,
                          CharMetricsFunc metrics, gpointer metrics_data)
{
  ExprInfo *ei = ensure_expr_is_cached (self, index);

  if (!ei->result_string || (index == EVALUATOR_MAIN_INDEX && self->changed_value)) {
    if (index == EVALUATOR_HISTORY_MAIN_INDEX) {
      /* no result for the history's copy that the main expression did not get: as if timed out */
      if (listener->funcs->on_cancelled)
        listener->funcs->on_cancelled (listener->data, index);
    } else if (ei->job && ei->job->kind == JOB_EVALUATE && ei->job->required) {
      /* a duplicate request */
    } else {
      /* (re)start in the requested mode, with the longer timeout */
      cancel_expr (self, ei, TRUE);
      evaluate_result (self, index, listener, metrics, metrics_data, TRUE);
    }
  } else if (ei->result_string == ERRONEOUS_RESULT) {
    /* evaluate again for a new notification */
    cancel_expr (self, ei, TRUE);
    evaluate_result (self, index, listener, metrics, metrics_data, TRUE);
  } else {
    notify_immediately (self, index, ei, listener, metrics, metrics_data);
  }
}

gboolean
evaluator_has_result (Evaluator *self, long index)
{
  return ensure_expr_is_cached (self, index)->result_string != NULL;
}

gboolean
evaluator_evaluation_in_progress (Evaluator *self, long index)
{
  ExprInfo *ei = lookup (self, index);
  return ei && ei->job;
}

/* cancel(ExprInfo, boolean): TRUE if an initial evaluation was cancelled */
static gboolean
cancel_expr (Evaluator *self, ExprInfo *ei, gboolean quiet)
{
  Job *job = ei->job;

  if (!job)
    return FALSE;
  if (quiet)
    job->quiet = TRUE;
  g_atomic_int_set (&job->cancelled, 1);
  ei->job = NULL;
  if (ei->val) {
    /* a reevaluation */
    ei->result_string_offset_req = ei->result_string_offset;
    return FALSE;
  }
  if (ei == self->main_expr)
    self->changed_value = TRUE;   /* the expected evaluation did not happen */
  return TRUE;
}

gboolean
evaluator_cancel (Evaluator *self, long index, gboolean quiet)
{
  ExprInfo *ei = lookup (self, index);
  return ei && cancel_expr (self, ei, quiet);
}

void
evaluator_cancel_all (Evaluator *self, gboolean quiet)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->exprs);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    cancel_expr (self, value, quiet);
}

void
evaluator_cancel_non_main (Evaluator *self)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->exprs);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (value != self->main_expr)
      cancel_expr (self, value, TRUE);
}

/* ensureCachePrec: starts computing more digits if needed; only after a successful evaluation */
static void
ensure_cache_prec (Evaluator *self, long index, int prec_offset, const EvaluationListener *listener)
{
  ExprInfo *ei = lookup (self, index);

  if ((ei->result_string && ei->result_string_offset >= prec_offset) || ei->result_string_offset_req >= prec_offset)
    return;
  if (!ei->val)
    return;   /* Java would crash here: there is nothing to compute more digits of yet */
  if (ei->job) {
    /* one computation at a time */
    g_atomic_int_set (&ei->job->cancelled, 1);
    ei->job = NULL;
  }
  Job *job = job_new (self, JOB_REEVALUATE, index, listener);
  job->value = real_ref (ei->val);
  ei->result_string_offset_req = prec_offset + PRECOMPUTE_DIGITS;
  if (ei->result_string)
    ei->result_string_offset_req += ei->result_string_offset_req / PRECOMPUTE_DIVISOR;
  job->prec_offset = ei->result_string_offset_req;
  ei->job = job;
  g_thread_pool_push (self->worker, job, NULL);
}

char *
evaluator_get_string (Evaluator *self, long index, int *prec_offset, int max_prec_offset, int max_digs,
                      gboolean *truncated, gboolean *negative, const EvaluationListener *listener)
{
  ExprInfo *ei = lookup (self, index);
  int current_prec_offset = *prec_offset;

  *truncated = *negative = FALSE;
  if (!ei || !ei->result_string || ei->result_string == ERRONEOUS_RESULT) {
    if (ei && !ei->result_string)
      ensure_cache_prec (self, index, current_prec_offset + EXTRA_DIGITS, listener);
    return g_strdup (" ");   /* repairs itself in a moment */
  }
  int len = strlen (ei->result_string);
  ensure_cache_prec (self, index, current_prec_offset + EXTRA_DIGITS + len / EXTRA_DIVISOR, listener);

  /* an appropriate part of the digits, padded if needed */
  gboolean my_negative = ei->result_string[0] == '-';
  *negative = my_negative;
  /* not left of the leftmost digits, unless that still leaves an integer */
  int integral_digits = len - ei->result_string_offset;   /* includes 1 for the point */
  if (my_negative)
    --integral_digits;
  int min_prec_offset = MIN (MIN_DISPLAYED_DIGS - integral_digits, -1);
  current_prec_offset = MIN (MAX (current_prec_offset, min_prec_offset), max_prec_offset);
  *prec_offset = current_prec_offset;
  int extra_digs = ei->result_string_offset - current_prec_offset;   /* trailing digits to drop */
  int deficit = 0;                                                    /* digits we are short */
  if (extra_digs < 0) {
    extra_digs = 0;
    deficit = MIN (current_prec_offset - ei->result_string_offset, max_digs);
  }
  int end_index = len - extra_digs;
  if (end_index < 1)
    return g_strdup (" ");
  int start_index = MAX (end_index + deficit - max_digs, 0);

  /* getMsdIndex */
  int msd;
  if (ei->msd_index != CALC_INVALID_MSD) {
    if (ei->result_string[ei->msd_index] == '0')
      ei->msd_index++;
    msd = ei->msd_index;
  } else if (real_definitely_zero (ei->val)) {
    msd = CALC_INVALID_MSD;
  } else {
    msd = ei->msd_index = calc_get_msd_index_of (ei->result_string);
  }
  *truncated = start_index > msd;
  GString *result = g_string_new_len (ei->result_string + start_index, end_index - start_index);
  /* blanks for the digits not known yet: never where the point goes, since there are always
   * decimals computed */
  for (int i = 0; i < deficit; i++)
    g_string_append_c (result, ' ');
  return g_string_free (result, FALSE);
}

/* --- editing */

static void
add_10_pow (Evaluator *self)
{
  g_autoptr (CalcExpr) ten = calc_expr_new ();

  calc_expr_add (ten, CALC_KEY_DIGIT_1);
  calc_expr_add (ten, CALC_KEY_DIGIT_0);
  self->changed_value = TRUE;
  calc_expr_append (self->main_expr->expr, ten);
  calc_expr_add (self->main_expr->expr, CALC_KEY_OP_POW);
}

gboolean
evaluator_append (Evaluator *self, CalcKey key)
{
  if (key == CALC_KEY_FUN_10POW) {
    add_10_pow (self);   /* a macro */
    return TRUE;
  }
  self->changed_value = self->changed_value || !calc_key_is_binary (key);
  if (!calc_expr_add (self->main_expr->expr, key))
    return FALSE;
  if (!self->has_trig_funcs)
    self->has_trig_funcs = calc_key_is_trig_func (key);
  return TRUE;
}

void
evaluator_delete (Evaluator *self)
{
  self->changed_value = TRUE;
  calc_expr_delete (self->main_expr->expr);
  if (calc_expr_is_empty (self->main_expr->expr))
    self->main_expr->long_timeout = FALSE;
  self->has_trig_funcs = calc_expr_has_trig_funcs (self->main_expr->expr);
}

void
evaluator_clear_main (Evaluator *self)
{
  calc_expr_clear (self->main_expr->expr);
  self->has_trig_funcs = FALSE;
  clear_main_cache (self);
  self->main_expr->long_timeout = FALSE;
}

void
evaluator_clear_everything (Evaluator *self)
{
  gboolean degree_mode = self->main_expr->degree_mode;

  evaluator_cancel_all (self, TRUE);
  set_saved_index (self, 0);
  set_memory_index (self, 0);
  expression_db_erase_all (self->db);
  g_hash_table_remove_all (self->exprs);
  set_main_expr (self, expr_info_new (calc_expr_new (), degree_mode));
}

void
evaluator_touch (Evaluator *self)
{
  self->changed_value = TRUE;
}

void
evaluator_add_exponent (Evaluator *self, const gunichar *s, int begin, int end)
{
  int sign = 1;
  int exp = 0;
  int i = begin + 1;

  if (calc_key_for_char (s[i]) == CALC_KEY_OP_SUB) {
    sign = -1;
    ++i;
  }
  for (; i < end; ++i)
    exp = 10 * exp + g_unichar_digit_value (s[i]);
  calc_expr_add_exponent (self->main_expr->expr, sign * exp);
  self->changed_value = TRUE;
}

/* copy: the expression at index without its trailing binary operators, with its result if asked */
static ExprInfo *
copy_expr_info (Evaluator *self, long index, gboolean copy_value)
{
  ExprInfo *from = lookup (self, index);
  ExprInfo *ei = expr_info_new (calc_expr_copy (from->expr), from->degree_mode);

  while (calc_expr_has_trailing_binary (ei->expr))
    calc_expr_delete (ei->expr);
  if (copy_value) {
    ei->val = from->val ? real_ref (from->val) : NULL;
    ei->result_string = from->result_string == ERRONEOUS_RESULT ? ERRONEOUS_RESULT : g_strdup (from->result_string);
    ei->result_string_offset = ei->result_string_offset_req = from->result_string_offset;
    ei->msd_index = from->msd_index;
  }
  ei->long_timeout = from->long_timeout;
  return ei;
}

/* addToDB: stores ei (taking it) under a new index */
static long
add_to_db (Evaluator *self, gboolean in_history, ExprInfo *ei)
{
  g_autoptr (GBytes) bytes = calc_expr_to_bytes (ei->expr);
  long index = expression_db_add_row (self->db, !in_history, bytes, ei->degree_mode, ei->long_timeout,
                                      &ei->time_stamp);

  g_hash_table_replace (self->exprs, index_key (index), ei);
  return index;
}

long
evaluator_preserve (Evaluator *self, long index, gboolean in_history)
{
  ExprInfo *ei = copy_expr_info (self, index, TRUE);

  if (!ei->result_string || ei->result_string == ERRONEOUS_RESULT)
    g_warning ("preserving an unevaluated expression");   /* Java: AssertionError */
  return add_to_db (self, in_history, ei);
}

void
evaluator_represerve (Evaluator *self)
{
  ensure_expr_is_cached (self, evaluator_get_max_index (self));
}

void
evaluator_copy_main_to_history (Evaluator *self)
{
  evaluator_cancel (self, EVALUATOR_HISTORY_MAIN_INDEX, TRUE);
  g_hash_table_replace (self->exprs, index_key (EVALUATOR_HISTORY_MAIN_INDEX),
                        copy_expr_info (self, EVALUATOR_MAIN_INDEX, TRUE));
}

/* getCollapsedExpr: one pre-evaluated token standing for the result at index; NULL if there is none */
static CalcExpr *
get_collapsed_expr (Evaluator *self, long index)
{
  long real_index = is_mutable_index (index) ? evaluator_preserve (self, index, FALSE) : index;
  ExprInfo *ei = lookup (self, real_index);
  const char *rs = ei->result_string;

  if (rs == ERRONEOUS_RESULT || !rs || !ei->val)
    return NULL;
  int dot_index = strchr (rs, '.') - rs;
  int least_dig_offset = calc_get_lsd_offset (ei->val, rs, dot_index);
  g_autofree char *short_rep = calc_get_short_string (rs, calc_get_msd_index_of (rs), least_dig_offset);
  return calc_expr_new_pre_evaluated (real_index, short_rep);
}

void
evaluator_collapse (Evaluator *self, long index)
{
  gboolean long_timeout = lookup (self, index)->long_timeout;
  g_autoptr (CalcExpr) abbreviated = get_collapsed_expr (self, index);

  evaluator_clear_main (self);
  if (abbreviated)
    calc_expr_append (self->main_expr->expr, abbreviated);
  self->main_expr->long_timeout = long_timeout;
  self->changed_value = TRUE;
  self->has_trig_funcs = FALSE;   /* the degree mode no longer matters */
}

void
evaluator_append_expr (Evaluator *self, long index)
{
  ExprInfo *ei = ensure_expr_is_cached (self, index);

  self->changed_value = TRUE;
  self->main_expr->long_timeout |= ei->long_timeout;
  g_autoptr (CalcExpr) collapsed = get_collapsed_expr (self, index);
  if (collapsed)
    calc_expr_append (self->main_expr->expr, collapsed);
}

/* --- memory and the clipboard's saved expression */

static void
set_when_done_finish (SetWhenDone *swd)
{
  swd->evaluator->set_when_done = g_list_remove (swd->evaluator->set_when_done, swd);
  g_free (swd);
}

static void
set_when_done_on_evaluate (gpointer data, long index, int init_prec_offset, int msd_index, int lsd_offset,
                           const char *truncated_whole_part)
{
  SetWhenDone *swd = data;
  Evaluator *self = swd->evaluator;

  if (swd->memory) {
    if (swd->persist)
      set_memory_index (self, swd->index);
    else
      self->memory_index = swd->index;
  } else {
    self->saved_index = swd->index;
  }
  set_when_done_finish (swd);
}

static void
set_when_done_on_cancelled (gpointer data, long index)
{
  set_when_done_finish (data);   /* "extremely unlikely; leave unset" */
}

static void
set_when_done_on_error (gpointer data, long index, CalcFailure failure)
{
  set_when_done_finish (data);
}

static const EvaluationListenerFuncs set_when_done_funcs = {
  set_when_done_on_cancelled, set_when_done_on_error, set_when_done_on_evaluate, NULL,
};

/* setMemoryIndexWhenEvaluated, setSavedIndexWhenEvaluated */
static void
set_when_evaluated (Evaluator *self, long index, gboolean memory, gboolean persist)
{
  SetWhenDone *swd = g_new0 (SetWhenDone, 1);
  EvaluationListener listener = { &set_when_done_funcs, swd, NULL };

  swd->evaluator = self;
  swd->index = index;
  swd->memory = memory;
  swd->persist = persist;
  self->set_when_done = g_list_prepend (self->set_when_done, swd);
  /* a notify_immediately may call it, and free it, right away */
  evaluator_require_result (self, index, &listener, NULL, NULL);
}

/* generalized_sum: memory op index, collapsed; NULL if either has no result */
static ExprInfo *
generalized_sum (Evaluator *self, long index1, long index2, CalcKey op)
{
  g_autoptr (CalcExpr) collapsed1 = get_collapsed_expr (self, index1);
  g_autoptr (CalcExpr) collapsed2 = get_collapsed_expr (self, index2);

  if (!collapsed1 || !collapsed2)
    return NULL;
  CalcExpr *result = calc_expr_new ();
  calc_expr_append (result, collapsed1);
  calc_expr_add (result, op);
  calc_expr_append (result, collapsed2);
  ExprInfo *ei = expr_info_new (result, FALSE);   /* degrees or radians don't matter */
  ei->long_timeout = lookup (self, index1)->long_timeout || lookup (self, index2)->long_timeout;
  return ei;
}

long
evaluator_get_memory_index (Evaluator *self)
{
  return self->memory_index;
}

long
evaluator_get_saved_index (Evaluator *self)
{
  return self->saved_index;
}

void
evaluator_copy_to_memory (Evaluator *self, long index)
{
  set_memory_index (self, is_mutable_index (index) ? evaluator_preserve (self, index, FALSE) : index);
}

static void
memory_op (Evaluator *self, long index, CalcKey op)
{
  ensure_expr_is_cached (self, self->memory_index);
  ExprInfo *ei = generalized_sum (self, self->memory_index, index, op);
  if (!ei)
    return;
  long new_index = add_to_db (self, FALSE, ei);
  self->memory_index = 0;   /* invalid while it is evaluated */
  set_when_evaluated (self, new_index, TRUE, TRUE);
}

void
evaluator_add_to_memory (Evaluator *self, long index)
{
  memory_op (self, index, CALC_KEY_OP_ADD);
}

void
evaluator_subtract_from_memory (Evaluator *self, long index)
{
  memory_op (self, index, CALC_KEY_OP_SUB);
}

char *
evaluator_capture (Evaluator *self, long index)
{
  ExprInfo *ei = lookup (self, index);

  /* copyToSaved */
  if (!ei || !ei->result_string || ei->result_string == ERRONEOUS_RESULT)
    return NULL;
  set_saved_index (self, is_mutable_index (index) ? evaluator_preserve (self, index, FALSE) : index);

  /* a new, entirely private name for this result, after RFC 4151 */
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *iso_date = g_date_time_format (now, "%Y-%m-%d");
  g_free (self->saved_name);
  self->saved_name = g_strdup_printf ("calculator2.android.com,%s:%u", iso_date,
                                      g_random_int () & 0x3fffffff);
  if (self->settings)
    g_settings_set_string (self->settings, "saved-name", self->saved_name);
  return g_strdup_printf ("tag:%s", self->saved_name);
}

gboolean
evaluator_is_last_saved (Evaluator *self, const char *name)
{
  g_autofree char *mine = g_strdup_printf ("tag:%s", self->saved_name);
  return self->saved_index != 0 && name && strcmp (name, mine) == 0;
}

/* --- across starts */

GBytes *
evaluator_save_state (Evaluator *self)
{
  g_autoptr (GBytes) expr = calc_expr_to_bytes (self->main_expr->expr);
  GByteArray *out = g_byte_array_new ();
  guint8 flags[2] = { self->main_expr->degree_mode ? 1 : 0, self->main_expr->long_timeout ? 1 : 0 };
  gsize size;
  const guint8 *data = g_bytes_get_data (expr, &size);

  g_byte_array_append (out, flags, 2);
  g_byte_array_append (out, data, size);
  return g_byte_array_free_to_bytes (out);
}

void
evaluator_restore_state (Evaluator *self, GBytes *state)
{
  gsize size;
  const guint8 *data = g_bytes_get_data (state, &size);

  self->changed_value = TRUE;
  if (size < 2)
    return;
  CalcExpr *expr = calc_expr_new_from_bytes (data + 2, size - 2);
  if (!expr)
    return;   /* Java logs it and goes on */
  self->main_expr->degree_mode = data[0] != 0;
  self->main_expr->long_timeout = data[1] != 0;
  calc_expr_free (self->main_expr->expr);
  self->main_expr->expr = expr;
  self->has_trig_funcs = calc_expr_has_trig_funcs (expr);
}
