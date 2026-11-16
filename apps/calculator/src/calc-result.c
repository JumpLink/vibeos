/* CalcResult: see calc-result.h. Ported from ExactCalculator's CalculatorResult, whose names it keeps: positions
 * are in device pixels (a gesture's logical pixels are multiplied up), and a scroll changes the text a whole
 * character at a time. */
#include "calc-result.h"
#include "calc-format.h"
#include "calc-keys.h"
#include "calc-text.h"
#include "unified-real.h"

#include <math.h>
#include <string.h>
#include <vibeos.h>

#define MAX_COPY_EXTRA        100
#define MAX_RECOMPUTE_DIGITS  2000
#define LONG_PRESS_MS         400
#define SCROLL_INVALID        (CALC_MAX_RIGHT_SCROLL + 10000)

struct _CalcResult {
  GtkWidget          parent;
  Evaluator         *evaluator;
  long               index;
  CalcResultRequest  request;
  EvaluationListener own_listener, listener;
  gboolean           evaluate_pending;
  gboolean           valid;
  gboolean           store_requested;

  CalcResultLayout   l;                 /* the positions, the metrics and the scrollable flag */
  int                width_constraint;  /* px */
  int                measured_width;    /* the width the metrics are for, px; -1: none */
  double             measured_size;

  double             size, pad_top, pad_bottom, pad_start, pad_end, exact_height;
  char              *text;              /* as shown */
  int                exp_index;         /* the exponent's first character, coloured; -1: none */
  double             text_scale;        /* an error message too wide is drawn smaller */
  gboolean           highlight;
  GdkRGBA            color, exponent_color;
  PangoLayout       *layout;
  gboolean           layout_dirty;

  SpongyFling        fling;
  SpongyVelocity     velocity;
  guint              fling_tick;
  gboolean           dragging;
  int                drag_start_pos;
  guint              long_press_id;
};

G_DEFINE_FINAL_TYPE (CalcResult, calc_result, GTK_TYPE_WIDGET)

enum { LONG_PRESSED, COPIED, SCROLL_STARTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static int
device_scale (CalcResult *self)
{
  return vibeos_device_scale (GTK_WIDGET (self));
}

static double
view_height (CalcResult *self)
{
  return self->exact_height > 0 ? self->exact_height : gtk_widget_get_height (GTK_WIDGET (self));
}

/* --- the listener: the result's own */

static void
own_on_evaluate (gpointer data, long index, int init_prec_offset, int msd_index, int lsd_offset,
                 const char *truncated_whole_part)
{
  calc_result_on_evaluate (data, index, init_prec_offset, msd_index, lsd_offset, truncated_whole_part);
}

static void
own_on_error (gpointer data, long index, CalcFailure failure)
{
  calc_result_on_error (data, index, failure);
}

static void
own_on_cancelled (gpointer data, long index)
{
  calc_result_on_cancelled (data, index);
}

static void
own_on_reevaluate (gpointer data, long index)
{
  calc_result_on_reevaluate (data, index);
}

static const EvaluationListenerFuncs own_funcs = {
  own_on_cancelled, own_on_error, own_on_evaluate, own_on_reevaluate,
};

const EvaluationListener *
calc_result_get_listener (CalcResult *self)
{
  return &self->own_listener;
}

/* --- measuring */

static double
string_width (CalcResult *self, PangoLayout *probe, const char *s)
{
  pango_layout_set_text (probe, s, -1);
  return calc_text_width (probe) * device_scale (self);
}

static void
measure_chars (CalcResult *self, int width_px)
{
  int scale = device_scale (self);
  PangoLayout *probe = calc_text_layout (GTK_WIDGET (self), "", self->size, CALC_WEIGHT_REGULAR);
  float char_width = 0;
  char digit[2] = { 0, 0 };

  for (char c = '0'; c <= '9'; c++) {
    digit[0] = c;
    char_width = MAX (char_width, string_width (self, probe, digit));
  }
  float decimal_width = string_width (self, probe, calc_decimal_point ());
  float minus_extra = MAX (string_width (self, probe, CALC_MINUS_SIGN) - char_width, 0);
  float ellipsis_extra = MAX (string_width (self, probe, CALC_ELLIPSIS) - char_width, 0);
  /* calc_translate_result ("e") drops a trailing 'e', so the exponent marker measures as nothing */
  float exp_extra = 0;
  float type1_extra = 2 * minus_extra + exp_extra + decimal_width;
  float type2_extra = ellipsis_extra + exp_extra + minus_extra;
  float extra = MAX (type1_extra, type2_extra);
  int int_extra = (int) ceilf (extra) + 1;   /* to cover rounding sins */
  float grouping = string_width (self, probe, calc_grouping_separator ());

  self->width_constraint = width_px - (int) round ((self->pad_start + self->pad_end) * scale) - int_extra;
  self->l.char_width = char_width;
  self->l.metrics.no_exponent_credit = (extra - MAX (ellipsis_extra, minus_extra)) / char_width;
  self->l.metrics.no_ellipsis_credit = (extra - minus_extra) / char_width;
  self->l.metrics.decimal_credit = MAX (char_width - decimal_width, 0) / char_width;
  self->l.metrics.grouping_separator_width_ratio = grouping / char_width;
  self->l.metrics.max_chars = self->width_constraint > 0 ? (int) floorf (self->width_constraint / char_width) : 0;
  self->measured_width = width_px;
  self->measured_size = self->size;
  g_object_unref (probe);
}

void
calc_result_char_metrics (gpointer data, CalcCharMetrics *metrics)
{
  CalcResult *self = data;

  *metrics = self->l.metrics;
}

static int
char_offset (CalcResult *self, int pos)
{
  return calc_result_layout_char_offset (&self->l, pos);
}

/* --- the text */

static void
set_text (CalcResult *self, const char *text, int exp_index, double scale)
{
  g_free (self->text);
  self->text = g_strdup (text ? text : "");
  self->exp_index = exp_index;
  self->text_scale = scale;
  self->layout_dirty = TRUE;
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, self->text, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
ensure_layout (CalcResult *self)
{
  double size = self->size * self->text_scale;

  if (!self->layout)
    self->layout = calc_text_layout (GTK_WIDGET (self), "", size, CALC_WEIGHT_REGULAR);
  if (!self->layout_dirty)
    return;
  calc_text_set_size (self->layout, size, CALC_WEIGHT_REGULAR);
  {
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs, pango_attr_font_features_new ("tnum, cv03, cv04"));
    if (self->exp_index >= 0) {
      /* the exponent, when it only says where the digits are: in exponent_color */
      PangoAttribute *exp = pango_attr_foreground_new (self->exponent_color.red * 65535,
                                                       self->exponent_color.green * 65535,
                                                       self->exponent_color.blue * 65535);
      exp->start_index = g_utf8_offset_to_pointer (self->text, self->exp_index) - self->text;
      pango_attr_list_insert (attrs, exp);
    }
    pango_layout_set_attributes (self->layout, attrs);
    pango_attr_list_unref (attrs);
  }
  pango_layout_set_text (self->layout, self->text ? self->text : "", -1);
  self->layout_dirty = FALSE;
}

/* the text's left edge and baseline, px */
static void
text_position (CalcResult *self, double *x, double *baseline, double *width)
{
  int scale = device_scale (self);
  double w = gtk_widget_get_width (GTK_WIDGET (self)), h = view_height (self);
  double size = self->size * self->text_scale;
  /* the compound padding is the view's (its own text size); the line is the text's (a smaller span's) */
  double padding = round (self->pad_bottom * scale);
  double compound = padding - MIN (padding, ceil (CALC_DESCENT * self->size * scale));

  ensure_layout (self);
  *width = calc_text_width (self->layout);
  *x = w - self->pad_end - *width;
  *baseline = (round (h * scale) - compound - round (CALC_DESCENT * size * scale)) / scale;
}

static void
calc_result_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcResult *self = CALC_RESULT (widget);
  double x, baseline, width;

  if (!self->text || !*self->text)
    return;
  text_position (self, &x, &baseline, &width);
  if (self->highlight) {
    /* the highlight behind the result while its copy toolbar is open */
    GdkRGBA color = vibeos_color_alpha (VIBEOS_PRIMARY, 0.4f);
    double size = self->size * self->text_scale;

    gtk_snapshot_append_color (snapshot, &color, &GRAPHENE_RECT_INIT (x, baseline - CALC_ASCENT * size, width,
                                                                      (CALC_ASCENT + CALC_DESCENT) * size));
  }
  calc_text_draw (snapshot, self->layout, x, baseline, &self->color);
}

void
calc_result_get_text_bounds (CalcResult *self, graphene_rect_t *bounds)
{
  double x, baseline, width;
  double w = gtk_widget_get_width (GTK_WIDGET (self));

  text_position (self, &x, &baseline, &width);
  /* the view less its padding, narrowed to the text from the end */
  graphene_rect_init (bounds, MAX (self->pad_start, x), self->pad_top, MIN (width, w - self->pad_start - self->pad_end),
                      view_height (self) - self->pad_top - self->pad_bottom);
}

/* --- layout: measuring, and the evaluation a layout starts */

static void
calc_result_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  CalcResult *self = CALC_RESULT (widget);

  *minimum = *natural = 0;
  if (orientation == GTK_ORIENTATION_VERTICAL) {
    double height = calc_text_view_height (self->pad_top, self->pad_bottom, self->size, vibeos_device_scale (widget));

    *minimum = *natural = (int) ceil (height - 0.001);
  }
}

static void
evaluate_if_wanted (CalcResult *self)
{
  CalcExpr *expr;

  self->evaluate_pending = FALSE;
  if (!self->evaluator || self->request == CALC_RESULT_SHOULD_NOT_EVALUATE)
    return;
  expr = evaluator_get_expr (self->evaluator, self->index);
  if (!expr || !calc_expr_has_interesting_ops (expr))
    return;
  if (self->request == CALC_RESULT_SHOULD_REQUIRE)
    evaluator_require_result (self->evaluator, self->index, &self->listener, calc_result_char_metrics, self);
  else
    evaluator_evaluate_and_notify (self->evaluator, self->index, &self->listener, calc_result_char_metrics, self);
}

static gboolean
evaluate_idle (gpointer data)
{
  CalcResult *self = data;

  if (self->evaluate_pending)
    evaluate_if_wanted (self);
  return G_SOURCE_REMOVE;
}

static void
calc_result_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcResult *self = CALC_RESULT (widget);
  int width_px = width * device_scale (self);

  if (width_px != self->measured_width || self->size != self->measured_size) {
    measure_chars (self, width_px);
    /* a new width is a new layout, and a new layout evaluates */
    self->evaluate_pending = TRUE;
  }
  if (self->evaluate_pending)
    /* after this allocation: the listener may change the widgets being laid out */
    g_idle_add_full (G_PRIORITY_HIGH_IDLE, evaluate_idle, g_object_ref (self), g_object_unref);
}

/* --- evaluation results */

static char *
get_formatted_result (CalcResult *self, int prec_offset, int max_size, int *last_displayed_offset,
                      gboolean force_precision, gboolean force_sci_notation, gboolean insert_commas)
{
  gboolean truncated = FALSE, negative = FALSE;
  int requested = prec_offset;
  g_autofree char *raw = evaluator_get_string (self->evaluator, self->index, &requested, self->l.max_char_offset,
                                               max_size, &truncated, &negative, &self->own_listener);

  return calc_result_layout_format (&self->l, raw, requested, max_size, truncated, negative, last_displayed_offset,
                                    force_precision, force_sci_notation, insert_commas);
}

void
calc_result_on_evaluate (CalcResult *self, long index, int init_prec_offset, int msd_index, int lsd_offset,
                         const char *truncated_whole_part)
{
  calc_result_layout_init (&self->l, init_prec_offset, msd_index, lsd_offset, truncated_whole_part);
  if (self->store_requested) {
    evaluator_copy_to_memory (self->evaluator, index);
    self->store_requested = FALSE;
  }
  calc_result_redisplay (self);
}

void
calc_result_on_error (CalcResult *self, long index, CalcFailure failure)
{
  const char *msg = calc_failure_message (failure);
  PangoLayout *probe = calc_text_layout (GTK_WIDGET (self), msg, self->size, CALC_WEIGHT_REGULAR);
  double measured = calc_text_width (probe) * device_scale (self);

  g_object_unref (probe);
  self->store_requested = FALSE;
  self->valid = FALSE;
  self->l.scrollable = FALSE;
  /* a message too wide is shrunk to fit */
  if (self->width_constraint > 0 && measured > self->width_constraint)
    set_text (self, msg, -1, 0.99 * self->width_constraint / measured);
  else
    set_text (self, msg, -1, 1);
}

void
calc_result_on_cancelled (CalcResult *self, long index)
{
  calc_result_clear (self);
  self->store_requested = FALSE;
}

void
calc_result_on_reevaluate (CalcResult *self, long index)
{
  calc_result_redisplay (self);
}

void
calc_result_redisplay (CalcResult *self)
{
  int max_chars = self->l.metrics.max_chars;
  int current_char_offset, last_displayed = 0, exp_index;
  g_autofree char *result = NULL;
  g_autofree char *shown = NULL;

  if (max_chars < 4 || !self->evaluator)
    return;   /* too small to show a reasonable result */
  current_char_offset = char_offset (self, self->l.current_pos);
  result = get_formatted_result (self, current_char_offset, max_chars, &last_displayed,
                                 self->l.append_exponent /* force precision: keep the whole result */,
                                 !self->l.whole_part_fits
                                 && current_char_offset == char_offset (self, self->l.min_pos) /* force sci */,
                                 self->l.whole_part_fits /* insert commas */);
  exp_index = strchr (result, 'E') ? strchr (result, 'E') - result : -1;
  shown = calc_translate_result (result);
  /* an exponent with no point before it only says where the digits are: greyed out (the exponent colour) */
  set_text (self, shown, exp_index > 0 && !strchr (shown, '.') ? exp_index : -1, 1);
  self->l.last_displayed_offset = last_displayed;
  self->valid = TRUE;
}

void
calc_result_clear (CalcResult *self)
{
  self->valid = FALSE;
  self->l.scrollable = FALSE;
  set_text (self, "", -1, 1);
}

gboolean
calc_result_is_scrollable (CalcResult *self)
{
  return self->l.scrollable;
}

gboolean
calc_result_is_valid (CalcResult *self)
{
  return self->valid;
}

const char *
calc_result_get_text (CalcResult *self)
{
  return self->text ? self->text : "";
}

char *
calc_result_get_full_text (CalcResult *self, gboolean with_separators)
{
  g_autofree char *formatted = NULL;

  if (!self->valid)
    return g_strdup ("");
  if (!self->l.scrollable)
    return g_strdup (calc_result_get_text (self));
  formatted = get_formatted_result (self, self->l.last_displayed_offset, CALC_RESULT_MAX_COPY_SIZE, NULL,
                                    TRUE /* force precision */, FALSE, with_separators);
  return calc_translate_result (formatted);
}

gboolean
calc_result_full_text_is_exact (CalcResult *self)
{
  return !self->l.scrollable || (char_offset (self, self->l.max_pos) == char_offset (self, self->l.current_pos)
                                 && self->l.max_char_offset != CALC_MAX_RIGHT_SCROLL);
}

char *
calc_result_get_full_copy_text (CalcResult *self)
{
  UnifiedReal *value;
  g_autofree char *raw = NULL;
  g_autofree char *formatted = NULL;
  int fraction_lsd_offset;

  if (!self->valid || self->l.lsd_offset == G_MAXINT || calc_result_full_text_is_exact (self)
      || self->l.whole_len > MAX_RECOMPUTE_DIGITS || self->l.whole_len + self->l.lsd_offset > MAX_RECOMPUTE_DIGITS
      || self->l.lsd_offset - self->l.last_displayed_offset > MAX_COPY_EXTRA
      || !(value = evaluator_get_result (self->evaluator, self->index)))
    return calc_result_get_full_text (self, FALSE);
  /* the exact result instead: it is rational and its digits end (lsd known), so this is quick */
  fraction_lsd_offset = MAX (0, self->l.lsd_offset);
  raw = real_to_string_truncated (value, fraction_lsd_offset);
  if (self->l.lsd_offset <= -1) {
    raw[strlen (raw) - 1] = '\0';   /* the trailing decimal point */
    fraction_lsd_offset = -1;
  }
  formatted = calc_result_layout_format (&self->l, raw, fraction_lsd_offset, CALC_RESULT_MAX_COPY_SIZE, FALSE,
                                         raw[0] == '-', NULL, TRUE, FALSE, FALSE);
  return calc_translate_result (formatted);
}

/* --- memory and the clipboard */

void
calc_result_memory_store (CalcResult *self)
{
  if (evaluator_has_result (self->evaluator, self->index)) {
    evaluator_copy_to_memory (self->evaluator, self->index);
  } else {
    self->store_requested = TRUE;
    evaluator_require_result (self->evaluator, self->index, &self->own_listener, calc_result_char_metrics, self);
  }
}

void
calc_result_memory_add (CalcResult *self)
{
  evaluator_add_to_memory (self->evaluator, self->index);
}

void
calc_result_memory_subtract (CalcResult *self)
{
  evaluator_subtract_from_memory (self->evaluator, self->index);
}

gboolean
calc_result_copy (CalcResult *self)
{
  g_autofree char *text = NULL;
  g_autofree char *tag = NULL;
  GdkContentProvider *providers[2];
  GdkContentProvider *union_provider;
  int n = 0;

  if (evaluator_evaluation_in_progress (self->evaluator, self->index))
    return FALSE;   /* refuse to copy placeholder characters */
  text = calc_result_get_full_copy_text (self);
  /* the result's text, and the tag URI that lets a paste back here recognise it (capture) */
  tag = evaluator_capture (self->evaluator, self->index);
  providers[n++] = gdk_content_provider_new_typed (G_TYPE_STRING, text);
  if (tag) {
    g_autoptr (GBytes) bytes = g_bytes_new (tag, strlen (tag));
    providers[n++] = gdk_content_provider_new_for_bytes (CALC_CLIPBOARD_TAG_MIME, bytes);
  }
  union_provider = gdk_content_provider_new_union (providers, n);
  gdk_clipboard_set_content (gtk_widget_get_clipboard (GTK_WIDGET (self)), union_provider);
  g_object_unref (union_provider);
  g_signal_emit (self, signals[COPIED], 0);
  return TRUE;
}

/* --- scrolling: drags and flings */

static double
now_ms (void)
{
  return g_get_monotonic_time () / 1000.0;
}

static void
scroll_to (CalcResult *self, int pos)
{
  self->l.current_pos = pos;
  if (char_offset (self, self->l.current_pos) != char_offset (self, self->l.last_pos)) {
    self->l.last_pos = self->l.current_pos;
    calc_result_redisplay (self);
  }
}

static gboolean
on_fling_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalcResult *self = CALC_RESULT (widget);
  gboolean more = spongy_fling_update (&self->fling, now_ms ());

  if (self->l.scrollable)
    scroll_to (self, (int) round (self->fling.position * device_scale (self)));
  if (!more || !self->l.scrollable) {
    self->fling_tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/* a fling stops where it was going */
static void
stop_fling (CalcResult *self)
{
  if (!self->fling_tick)
    return;
  gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->fling_tick);
  self->fling_tick = 0;
  self->l.current_pos = (int) round (self->fling.final * device_scale (self));
  spongy_fling_abort (&self->fling);
}

static void
stop_long_press (CalcResult *self)
{
  if (self->long_press_id)
    g_source_remove (self->long_press_id);
  self->long_press_id = 0;
}

static gboolean
on_long_press (gpointer data)
{
  CalcResult *self = data;

  self->long_press_id = 0;
  if (self->valid)
    g_signal_emit (self, signals[LONG_PRESSED], 0);
  return G_SOURCE_REMOVE;
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CalcResult *self)
{
  self->dragging = FALSE;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, now_ms (), x, y);
  stop_long_press (self);
  self->long_press_id = g_timeout_add (LONG_PRESS_MS, on_long_press, self);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CalcResult *self)
{
  double slop = 8 * spongy_density ();
  double x, y;
  int pos;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  spongy_velocity_add (&self->velocity, now_ms (), x + dx, y + dy);
  if (!self->dragging) {
    if (hypot (dx, dy) <= slop)
      return;
    /* past the slop: no long press. Sideways, the result keeps the touch; a vertical one is the window's, to
     * pull the history down */
    stop_long_press (self);
    if (fabs (dx) <= slop || fabs (dx) <= fabs (dy))
      return;
    self->dragging = TRUE;
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    stop_fling (self);
    self->drag_start_pos = self->l.current_pos;
    g_signal_emit (self, signals[SCROLL_STARTED], 0);
    /* the first move goes the whole distance from the touch down */
  }
  if (!self->l.scrollable)
    return;
  pos = self->drag_start_pos - (int) round (dx * device_scale (self));
  scroll_to (self, CLAMP (pos, self->l.min_pos, self->l.max_pos));
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CalcResult *self)
{
  float vx, vy;
  double min_fling = 50 * spongy_density (), max_fling = 8000 * spongy_density ();
  int scale = device_scale (self);

  stop_long_press (self);
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  spongy_velocity_get (&self->velocity, now_ms (), &vx, &vy);
  vx = CLAMP (vx, -max_fling, max_fling);
  if (!self->l.scrollable || fabs (vx) < min_fling)
    return;
  /* a fling from the position between min_pos and max_pos, in px for the spline */
  spongy_fling_start (&self->fling, (float) self->l.current_pos / scale, -vx, (float) self->l.min_pos / scale,
                      (float) self->l.max_pos / scale, now_ms ());
  self->fling_tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_fling_tick, NULL, NULL);
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, CalcResult *self)
{
  stop_long_press (self);
  self->dragging = FALSE;
}

/* --- the object */

static void
calc_result_dispose (GObject *object)
{
  CalcResult *self = CALC_RESULT (object);

  if (self->fling_tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->fling_tick);
  self->fling_tick = 0;
  stop_long_press (self);
  g_clear_object (&self->layout);
  G_OBJECT_CLASS (calc_result_parent_class)->dispose (object);
}

static void
calc_result_finalize (GObject *object)
{
  g_free (CALC_RESULT (object)->text);
  G_OBJECT_CLASS (calc_result_parent_class)->finalize (object);
}

static void
calc_result_class_init (CalcResultClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_result_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_result_finalize;
  widget_class->measure = calc_result_measure;
  widget_class->size_allocate = calc_result_size_allocate;
  widget_class->snapshot = calc_result_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-result");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_LABEL);
  signals[LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
  signals[COPIED] = g_signal_new ("copied", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SCROLL_STARTED] = g_signal_new ("scroll-started", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
}

static void
calc_result_init (CalcResult *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->request = CALC_RESULT_SHOULD_REQUIRE;
  self->own_listener = (EvaluationListener) { &own_funcs, self, G_OBJECT (self) };
  self->listener = self->own_listener;
  self->measured_width = -1;
  self->exp_index = -1;
  self->text_scale = 1;
  self->l.char_width = 1;
  self->l.last_pos = SCROLL_INVALID;
  self->color = vibeos_color (VIBEOS_TERTIARY);            /* the result's colour */
  self->exponent_color = vibeos_color (VIBEOS_TERTIARY);   /* the exponent's, the same here */
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
calc_result_new (double text_size)
{
  CalcResult *self = g_object_new (CALC_TYPE_RESULT, NULL);

  self->size = text_size;
  return GTK_WIDGET (self);
}

void
calc_result_set_padding (CalcResult *self, double top, double bottom, double start, double end)
{
  self->pad_top = top;
  self->pad_bottom = bottom;
  self->pad_start = start;
  self->pad_end = end;
  self->measured_width = -1;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
calc_result_set_exact_height (CalcResult *self, double height)
{
  if (self->exact_height == height)
    return;
  self->exact_height = height;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
calc_result_set_evaluator (CalcResult *self, Evaluator *evaluator, long index)
{
  self->evaluator = evaluator;
  self->index = index;
  /* the next layout evaluates */
  self->evaluate_pending = TRUE;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_result_set_should_evaluate (CalcResult *self, CalcResultRequest request, const EvaluationListener *listener)
{
  self->listener = listener ? *listener : self->own_listener;
  self->request = request;
}

void
calc_result_set_color (CalcResult *self, const GdkRGBA *color)
{
  if (gdk_rgba_equal (&self->color, color))
    return;
  self->color = *color;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

GdkRGBA
calc_result_get_color (CalcResult *self)
{
  return self->color;
}

double
calc_result_get_text_size (CalcResult *self)
{
  return self->size;
}

void
calc_result_get_padding (CalcResult *self, double *bottom, double *end)
{
  *bottom = self->pad_bottom;
  *end = self->pad_end;
}

void
calc_result_set_highlight (CalcResult *self, gboolean highlight)
{
  if (self->highlight == highlight)
    return;
  self->highlight = highlight;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
