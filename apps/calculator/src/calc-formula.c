/* CalcFormula: see calc-formula.h */
#include "calc-formula.h"
#include "calc-text.h"

#include <math.h>
#include <neo.h>

#define LONG_PRESS_MS      400          /* held this long, a press is a long press */
#define SIZE_CHANGE_MS     400          /* the text size change's animation */
#define UNPROCESSED_COLOR  0xff0000     /* red */

struct _CalcFormula {
  GtkWidget       parent;
  char           *text;                 /* the expression and the unprocessed characters */
  int             processed_len;        /* bytes of text that are the expression */
  double          min_size, max_size, step, size;
  double          pad_top, pad_bottom, pad_start, pad_end;
  double          exact_height;
  GdkRGBA         color;
  PangoLayout    *layout;
  double          layout_size;
  gboolean        layout_dirty;
  double          width_constraint;     /* -1 until measured */

  /* the scroll view: how far the text view is scrolled from its left edge, px */
  double          scroll;
  gboolean        to_end, pending_end;
  SpongyFling     fling;
  SpongyVelocity  velocity;
  guint           fling_tick;
  gboolean        dragging;
  double          drag_start_scroll;
  guint           long_press_id;

  /* the size change: scale from `anim_from` to 1 */
  NeoAnim        *anim;
  double          anim_from, anim_scale;
};

G_DEFINE_FINAL_TYPE (CalcFormula, calc_formula, GTK_TYPE_WIDGET)

enum { TEXT_SIZE_CHANGED, LONG_PRESSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static double
view_height (CalcFormula *self)
{
  return self->exact_height > 0 ? self->exact_height : gtk_widget_get_height (GTK_WIDGET (self));
}

static void
ensure_layout (CalcFormula *self)
{
  if (!self->layout) {
    self->layout = calc_text_layout (GTK_WIDGET (self), "", self->size, CALC_WEIGHT_REGULAR);
    self->layout_dirty = TRUE;
  }
  if (self->layout_dirty) {
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs, pango_attr_font_features_new ("tnum, cv03, cv04"));
    if (self->text && self->text[self->processed_len]) {
      PangoAttribute *red = pango_attr_foreground_new ((UNPROCESSED_COLOR >> 16) * 257, ((UNPROCESSED_COLOR >> 8) & 0xff) * 257,
                                                       (UNPROCESSED_COLOR & 0xff) * 257);
      red->start_index = self->processed_len;
      pango_attr_list_insert (attrs, red);
    }
    pango_layout_set_text (self->layout, self->text ? self->text : "", -1);
    pango_layout_set_attributes (self->layout, attrs);
    pango_attr_list_unref (attrs);
    self->layout_dirty = FALSE;
  }
  if (self->layout_size != self->size) {
    calc_text_set_size (self->layout, self->size, CALC_WEIGHT_REGULAR);
    self->layout_size = self->size;
  }
}

static double
text_width (CalcFormula *self)
{
  ensure_layout (self);
  return calc_text_width (self->layout);
}

/* the text view's width: the scroll view's, or the text's with its padding when that is wider */
static double
text_view_width (CalcFormula *self)
{
  return MAX (gtk_widget_get_width (GTK_WIDGET (self)), text_width (self) + self->pad_start + self->pad_end);
}

static double
max_scroll (CalcFormula *self)
{
  return MAX (0, text_view_width (self) - gtk_widget_get_width (GTK_WIDGET (self)));
}

double
calc_formula_get_variable_text_size (CalcFormula *self, const char *text)
{
  PangoLayout *probe;
  double last_fit = self->min_size;

  if (self->width_constraint < 0 || self->max_size <= self->min_size)
    return self->size;
  probe = calc_text_layout (GTK_WIDGET (self), text ? text : "", self->min_size, CALC_WEIGHT_REGULAR);
  while (last_fit < self->max_size) {
    double size = MIN (last_fit + self->step, self->max_size);

    calc_text_set_size (probe, size, CALC_WEIGHT_REGULAR);
    if (calc_text_width (probe) > self->width_constraint)
      break;
    last_fit = size;
  }
  g_object_unref (probe);
  return last_fit;
}

/* --- drawing */

static void
calc_formula_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalcFormula *self = CALC_FORMULA (widget);
  double w = gtk_widget_get_width (widget), h = view_height (self);
  double tv_width = text_view_width (self);
  double tv_left = -self->scroll;
  double baseline = calc_text_bottom_baseline (h, self->pad_bottom, self->size, neo_device_scale (widget));
  double x = tv_left + tv_width - self->pad_end - text_width (self);

  if (!self->text || !*self->text)
    return;
  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, w, gtk_widget_get_height (widget)));
  gtk_snapshot_save (snapshot);
  if (self->anim_scale != 1) {
    /* about the text's bottom end, less its padding: the point the scale and the translation leave fixed */
    graphene_point_t pivot = GRAPHENE_POINT_INIT (tv_left + tv_width - self->pad_end, h - self->pad_bottom);

    gtk_snapshot_translate (snapshot, &pivot);
    gtk_snapshot_scale (snapshot, self->anim_scale, self->anim_scale);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-pivot.x, -pivot.y));
  }
  calc_text_draw (snapshot, self->layout, x, baseline, &self->color);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

void
calc_formula_get_text_bounds (CalcFormula *self, graphene_rect_t *bounds)
{
  double h = view_height (self);
  double tv_left = -self->scroll, tv_width = text_view_width (self);
  double tw = text_width (self);

  graphene_rect_init (bounds, tv_left + tv_width - self->pad_end - tw, self->pad_top, tw,
                      h - self->pad_top - self->pad_bottom);
}

/* --- layout */

static void
calc_formula_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  CalcFormula *self = CALC_FORMULA (widget);

  *minimum = *natural = 0;
  if (orientation == GTK_ORIENTATION_VERTICAL) {
    /* a history row's: as tall as its line */
    double height = calc_text_view_height (self->pad_top, self->pad_bottom, self->size, neo_device_scale (widget));

    *minimum = *natural = (int) ceil (height - 0.001);
  }
}

static void
calc_formula_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcFormula *self = CALC_FORMULA (widget);
  double constraint = width - self->pad_start - self->pad_end;

  if (constraint != self->width_constraint) {
    /* the size follows the width, without telling anyone */
    self->width_constraint = constraint;
    self->size = calc_formula_get_variable_text_size (self, self->text);
  }
  if (self->pending_end) {
    self->scroll = max_scroll (self);
    self->pending_end = FALSE;
  }
  self->scroll = CLAMP (self->scroll, 0, max_scroll (self));
}

/* --- scrolling */

static double
now_ms (void)
{
  return g_get_monotonic_time () / 1000.0;
}

static gboolean
on_fling_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  CalcFormula *self = CALC_FORMULA (widget);
  gboolean more = spongy_fling_update (&self->fling, now_ms ());

  self->scroll = CLAMP (self->fling.position, 0, max_scroll (self));
  gtk_widget_queue_draw (widget);
  if (!more) {
    self->fling_tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
stop_fling (CalcFormula *self)
{
  if (self->fling_tick)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->fling_tick);
  self->fling_tick = 0;
  spongy_fling_abort (&self->fling);
}

static void
stop_long_press (CalcFormula *self)
{
  if (self->long_press_id)
    g_source_remove (self->long_press_id);
  self->long_press_id = 0;
}

static gboolean
on_long_press (gpointer data)
{
  CalcFormula *self = data;

  self->long_press_id = 0;
  g_signal_emit (self, signals[LONG_PRESSED], 0);
  return G_SOURCE_REMOVE;
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, CalcFormula *self)
{
  stop_fling (self);
  self->dragging = FALSE;
  self->drag_start_scroll = self->scroll;
  spongy_velocity_clear (&self->velocity);
  spongy_velocity_add (&self->velocity, now_ms (), x, y);
  stop_long_press (self);
  self->long_press_id = g_timeout_add (LONG_PRESS_MS, on_long_press, self);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, CalcFormula *self)
{
  double slop = 8 * spongy_density ();
  double x, y;

  gtk_gesture_drag_get_start_point (drag, &x, &y);
  spongy_velocity_add (&self->velocity, now_ms (), x + dx, y + dy);
  if (fabs (dx) > slop || fabs (dy) > slop)
    stop_long_press (self);
  if (!self->dragging) {
    /* a drag past the slop sideways scrolls it, and keeps the window from taking it */
    if (fabs (dx) <= slop || fabs (dx) <= fabs (dy) || max_scroll (self) <= 0)
      return;
    self->dragging = TRUE;
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  }
  self->scroll = CLAMP (self->drag_start_scroll - dx, 0, max_scroll (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, CalcFormula *self)
{
  float vx, vy;
  double min_fling = 50 * spongy_density (), max_fling = 8000 * spongy_density ();

  stop_long_press (self);
  if (!self->dragging)
    return;
  self->dragging = FALSE;
  spongy_velocity_get (&self->velocity, now_ms (), &vx, &vy);
  vx = CLAMP (vx, -max_fling, max_fling);
  if (fabs (vx) < min_fling)
    return;
  spongy_fling_start (&self->fling, self->scroll, -vx, 0, max_scroll (self), now_ms ());
  self->fling_tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_fling_tick, NULL, NULL);
}

static void
on_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, CalcFormula *self)
{
  stop_long_press (self);
  self->dragging = FALSE;
}

void
calc_formula_scroll_to_end (CalcFormula *self)
{
  stop_fling (self);
  self->pending_end = TRUE;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* --- the size change */

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  CalcFormula *self = data;

  self->anim_scale = self->anim_from + (1 - self->anim_from) * value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
calc_formula_animate_size_change (CalcFormula *self, double old_size)
{
  self->anim_from = old_size / self->size;
  self->anim_scale = self->anim_from;
  neo_anim_start (self->anim, 0, 1, SIZE_CHANGE_MS, 0, SPONGY_ACCELERATE_DECELERATE);
}

/* --- the object */

static void
calc_formula_dispose (GObject *object)
{
  CalcFormula *self = CALC_FORMULA (object);

  stop_fling (self);
  stop_long_press (self);
  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_object (&self->layout);
  G_OBJECT_CLASS (calc_formula_parent_class)->dispose (object);
}

static void
calc_formula_finalize (GObject *object)
{
  g_free (CALC_FORMULA (object)->text);
  G_OBJECT_CLASS (calc_formula_parent_class)->finalize (object);
}

static void
calc_formula_class_init (CalcFormulaClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_formula_dispose;
  G_OBJECT_CLASS (klass)->finalize = calc_formula_finalize;
  widget_class->measure = calc_formula_measure;
  widget_class->size_allocate = calc_formula_size_allocate;
  widget_class->snapshot = calc_formula_snapshot;
  gtk_widget_class_set_css_name (widget_class, "calc-formula");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_LABEL);
  signals[TEXT_SIZE_CHANGED] = g_signal_new ("text-size-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[LONG_PRESSED] = g_signal_new ("long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
}

static void
calc_formula_init (CalcFormula *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();

  self->width_constraint = -1;
  self->anim_scale = 1;
  self->to_end = TRUE;
  self->color = neo_color (NEO_ON_SURFACE);
  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
calc_formula_new (double min_size, double max_size, double step)
{
  CalcFormula *self = g_object_new (CALC_TYPE_FORMULA, NULL);

  self->min_size = min_size;
  self->max_size = max_size;
  self->step = step > 0 ? step : (max_size - min_size) / 3;
  /* the first measure starts from the largest size */
  self->size = max_size;
  return GTK_WIDGET (self);
}

void
calc_formula_set_padding (CalcFormula *self, double top, double bottom, double start, double end)
{
  self->pad_top = top;
  self->pad_bottom = bottom;
  self->pad_start = start;
  self->pad_end = end;
  self->width_constraint = -1;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
calc_formula_set_exact_height (CalcFormula *self, double height)
{
  if (self->exact_height == height)
    return;
  self->exact_height = height;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
calc_formula_set_scroll_to_end (CalcFormula *self, gboolean to_end)
{
  self->to_end = to_end;
}

void
calc_formula_set_text (CalcFormula *self, const char *text, const char *unprocessed)
{
  g_autofree char *full = g_strconcat (text ? text : "", unprocessed ? unprocessed : "", NULL);
  double old_size = self->size, size;

  if (g_strcmp0 (self->text, full) == 0 && self->processed_len == (int) strlen (text ? text : ""))
    return;
  g_free (self->text);
  self->text = g_steal_pointer (&full);
  self->processed_len = strlen (text ? text : "");
  self->layout_dirty = TRUE;
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, self->text, -1);
  /* a new text: the size it fits at, and "text-size-changed" when that is new */
  size = calc_formula_get_variable_text_size (self, self->text);
  if (size != old_size) {
    self->size = size;
    g_signal_emit (self, signals[TEXT_SIZE_CHANGED], 0, old_size);
  }
  if (self->to_end) {
    stop_fling (self);
    self->pending_end = TRUE;
  } else {
    self->scroll = 0;
  }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

const char *
calc_formula_get_text (CalcFormula *self)
{
  return self->text ? self->text : "";
}

void
calc_formula_set_color (CalcFormula *self, const GdkRGBA *color)
{
  if (gdk_rgba_equal (&self->color, color))
    return;
  self->color = *color;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

GdkRGBA
calc_formula_get_color (CalcFormula *self)
{
  return self->color;
}

double
calc_formula_get_text_size (CalcFormula *self)
{
  return self->size;
}

double
calc_formula_get_min_text_size (CalcFormula *self)
{
  return self->min_size;
}
