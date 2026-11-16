/* VibeosPageStack: see vibeos-page-stack.h.
 *
 * A transition runs on one VibeosAnim, linear from 0 to 1 over its duration: the slide follows it through the
 * fast-out-slow-in curve, the fades follow the time itself. Two pages are drawn while it runs, the incoming one (the
 * new top) over the outgoing one (the page it covers, or the one going away, which stays a child until the end). */
#define G_LOG_DOMAIN "vibeos-page-stack"

#include "vibeos-page-stack.h"
#include "vibeos-anim.h"

#define PUSH_MS            250
#define BACK_MS            170
#define PANE_PUSH_MS       250
#define PANE_BACK_MS        90
#define FADE_THROUGH_MS    300
#define SHIFT              0.1    /* of the width: how far a page slides */
#define PANE_SHIFT         12     /* px: the same inside a pane */
#define OUT_FADE_END       0.35   /* of the time: the outgoing page is gone, the incoming one starts to show */
#define THROUGH_OUT_END    0.2    /* fade through: the outgoing page is gone after 60 of the 300 ms */
#define THROUGH_SCALE      0.92   /* fade through: the incoming page grows from this */
#define HIGHLIGHT_MS       300

struct _VibeosPageStack {
  GtkWidget             parent;
  GPtrArray            *pages;          /* bottom first */
  GtkWidget            *leaving;        /* a page on its way out (closed, replaced), drawn until the end */
  VibeosAnim           *anim;
  double                t;              /* 0 -> 1 through the transition */
  gboolean              back;           /* the transition running is a Back */
  VibeosPageTransition  transition;
  VibeosPageTransition  running;        /* the kind of the transition running (the stack's may change meanwhile) */
  gint64                trace_start;    /* the frame time of the transition's first frame, for the trace */

  guint                 highlight_ms;
  GtkWidget            *highlighted;    /* weak: shows the press highlight until the push's transition ends */
  GtkWidget            *pending;        /* the page to push once the highlight has shown */
  guint                 highlight_timeout;
};

G_DEFINE_FINAL_TYPE (VibeosPageStack, vibeos_page_stack, GTK_TYPE_WIDGET)

enum { TOP_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void flush_pending (VibeosPageStack *self);

GtkWidget *
vibeos_page_stack_get_top (VibeosPageStack *self)
{
  return self->pages->len ? self->pages->pdata[self->pages->len - 1] : NULL;
}

GtkWidget *
vibeos_page_stack_get_page (VibeosPageStack *self, guint index)
{
  return index < self->pages->len ? self->pages->pdata[index] : NULL;
}

guint
vibeos_page_stack_get_depth (VibeosPageStack *self)
{
  return self->pages->len;
}

gboolean
vibeos_page_stack_is_animating (VibeosPageStack *self)
{
  return vibeos_anim_is_running (self->anim);
}

/* the page the incoming one covers or replaces while a transition runs: the one leaving, else (an open) the one
 * under the top */
static GtkWidget *
outgoing (VibeosPageStack *self)
{
  if (self->leaving)
    return self->leaving;
  if (!self->back && self->pages->len > 1)
    return self->pages->pdata[self->pages->len - 2];
  return NULL;
}

/* where the two pages are at time t: the incoming page's offset, opacity and scale, the outgoing one's offset and
 * opacity */
typedef struct {
  double in_x, in_alpha, in_scale, out_x, out_alpha;
} Frame;

static Frame
frame_at (VibeosPageStack *self, double t)
{
  Frame f = { 0, 1, 1, 0, 0 };
  double width = gtk_widget_get_width (GTK_WIDGET (self));

  switch (self->running)
    {
    case VIBEOS_PAGE_TRANSITION_SHARED_AXIS:
    case VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE:
      {
        double shift = self->running == VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE ? PANE_SHIFT : width * SHIFT;
        double e = spongy_curve (SPONGY_FAST_OUT_SLOW_IN, t);
        double dir = self->back ? -1 : 1;    /* an open comes from the right; Back from the left */

        f.in_x = dir * shift * (1 - e);
        f.out_x = -dir * shift * e;
        f.out_alpha = 1 - CLAMP (t / OUT_FADE_END, 0, 1);
        f.in_alpha = CLAMP ((t - OUT_FADE_END) / (1 - OUT_FADE_END), 0, 1);
        break;
      }
    case VIBEOS_PAGE_TRANSITION_FADE_THROUGH:
      {
        double in = CLAMP ((t - THROUGH_OUT_END) / (1 - THROUGH_OUT_END), 0, 1);

        f.out_alpha = 1 - CLAMP (t / THROUGH_OUT_END, 0, 1);
        f.in_alpha = in;
        f.in_scale = THROUGH_SCALE + (1 - THROUGH_SCALE) * spongy_curve (SPONGY_DECELERATE_CUBIC, in);
        break;
      }
    case VIBEOS_PAGE_TRANSITION_NONE:
    default:
      break;
    }
  return f;
}

static void
finish_leaving (VibeosPageStack *self)
{
  if (!self->leaving)
    return;
  gtk_widget_unparent (self->leaving);
  g_clear_object (&self->leaving);
}

static void
clear_highlight (VibeosPageStack *self)
{
  if (!self->highlighted)
    return;
  gtk_widget_remove_css_class (self->highlighted, "vibeos-highlighted");
  g_object_remove_weak_pointer (G_OBJECT (self->highlighted), (gpointer *) &self->highlighted);
  self->highlighted = NULL;
}

/* only the top page (and in a transition the outgoing one) is drawn; only the top one takes input */
static void
sync_visibility (VibeosPageStack *self)
{
  GtkWidget *top = vibeos_page_stack_get_top (self);
  GtkWidget *out = vibeos_anim_is_running (self->anim) ? outgoing (self) : NULL;

  for (guint i = 0; i < self->pages->len; i++)
    {
      GtkWidget *page = self->pages->pdata[i];

      gtk_widget_set_child_visible (page, page == top || page == out);
      gtk_widget_set_can_target (page, page == top);
    }
  if (self->leaving)
    gtk_widget_set_can_target (self->leaving, FALSE);
}

static gboolean
tracing (void)
{
  static gsize done;
  static gboolean on;

  if (g_once_init_enter (&done))
    {
      on = !g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN);
      g_once_init_leave (&done, 1);
    }
  return on;
}

static void
trace (VibeosPageStack *self, double t)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  gint64 now = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  static const char *const kinds[] = { "shared-axis", "shared-axis-pane", "fade-through", "none" };
  Frame f = frame_at (self, t);

  if (t == 0 || !self->trace_start)
    self->trace_start = now;
  g_debug ("%s %s: frame %.1f ms, t %.3f, incoming x %.2f alpha %.3f scale %.3f, outgoing x %.2f alpha %.3f",
           kinds[self->running], self->back ? "back" : "open", (now - self->trace_start) / 1000.0, t,
           f.in_x, f.in_alpha, f.in_scale, f.out_x, f.out_alpha);
}

static void
on_anim (VibeosAnim *anim, double value, gpointer data)
{
  VibeosPageStack *self = data;

  self->t = value;
  if (tracing ())
    trace (self, value);
  if (!vibeos_anim_is_running (anim))
    {
      finish_leaving (self);
      sync_visibility (self);
      clear_highlight (self);
    }
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static guint
duration (VibeosPageStack *self)
{
  switch (self->running)
    {
    case VIBEOS_PAGE_TRANSITION_SHARED_AXIS:
      return self->back ? BACK_MS : PUSH_MS;
    case VIBEOS_PAGE_TRANSITION_SHARED_AXIS_PANE:
      return self->back ? PANE_BACK_MS : PANE_PUSH_MS;
    case VIBEOS_PAGE_TRANSITION_FADE_THROUGH:
      return FADE_THROUGH_MS;
    case VIBEOS_PAGE_TRANSITION_NONE:
    default:
      return 0;
    }
}

static void
start (VibeosPageStack *self, gboolean back)
{
  GtkWidget *top = vibeos_page_stack_get_top (self);

  self->back = back;
  self->running = self->transition;
  self->trace_start = 0;
  if (duration (self) && gtk_widget_get_mapped (GTK_WIDGET (self)))
    vibeos_anim_start (self->anim, 0, 1, duration (self), 0, SPONGY_LINEAR);
  else
    on_anim (self->anim, 1, self);      /* a cut, or nothing on screen to animate */
  sync_visibility (self);
  if (top)
    gtk_widget_grab_focus (top);
  g_signal_emit (self, signals[TOP_CHANGED], 0);
}

/* ends the transition running now, at once, before another starts */
static void
settle (VibeosPageStack *self)
{
  vibeos_anim_finish (self->anim);
  finish_leaving (self);
}

static void
add (VibeosPageStack *self, GtkWidget *page)
{
  g_ptr_array_add (self->pages, g_object_ref_sink (page));
  gtk_widget_set_parent (page, GTK_WIDGET (self));
}

void
vibeos_page_stack_push (VibeosPageStack *self, GtkWidget *page)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));
  g_return_if_fail (GTK_IS_WIDGET (page));

  flush_pending (self);
  settle (self);
  add (self, page);
  start (self, FALSE);
}

static gboolean
on_highlight_timeout (gpointer data)
{
  VibeosPageStack *self = data;

  self->highlight_timeout = 0;
  flush_pending (self);
  return G_SOURCE_REMOVE;
}

/* the page waiting behind a press highlight goes on now */
static void
flush_pending (VibeosPageStack *self)
{
  g_autoptr(GtkWidget) page = g_steal_pointer (&self->pending);

  g_clear_handle_id (&self->highlight_timeout, g_source_remove);
  if (!page)
    return;
  settle (self);
  add (self, page);
  start (self, FALSE);
}

void
vibeos_page_stack_push_from (VibeosPageStack *self, GtkWidget *page, GtkWidget *pressed)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));
  g_return_if_fail (GTK_IS_WIDGET (page));

  /* the transition running ends first: its end takes its own highlight away, and would take this one too */
  flush_pending (self);
  settle (self);
  clear_highlight (self);
  if (pressed)
    {
      self->highlighted = pressed;
      g_object_add_weak_pointer (G_OBJECT (pressed), (gpointer *) &self->highlighted);
      gtk_widget_add_css_class (pressed, "vibeos-highlighted");
    }
  if (!pressed || !self->highlight_ms || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      vibeos_page_stack_push (self, page);
      return;
    }
  self->pending = g_object_ref_sink (page);
  self->highlight_timeout = g_timeout_add (self->highlight_ms, on_highlight_timeout, self);
}

void
vibeos_page_stack_set_highlight_time (VibeosPageStack *self, guint ms)
{
  self->highlight_ms = ms;
}

gboolean
vibeos_page_stack_pop (VibeosPageStack *self)
{
  g_return_val_if_fail (VIBEOS_IS_PAGE_STACK (self), FALSE);

  flush_pending (self);
  if (self->pages->len < 2)
    return FALSE;
  settle (self);
  self->leaving = g_ptr_array_steal_index (self->pages, self->pages->len - 1);
  start (self, TRUE);
  return TRUE;
}

void
vibeos_page_stack_replace (VibeosPageStack *self, GtkWidget *page)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));
  g_return_if_fail (GTK_IS_WIDGET (page));

  flush_pending (self);
  settle (self);
  if (self->pages->len)
    self->leaving = g_ptr_array_steal_index (self->pages, self->pages->len - 1);
  add (self, page);
  start (self, FALSE);
}

/* takes the pages under the top off at once */
static void
drop_under_top (VibeosPageStack *self)
{
  while (self->pages->len > 1)
    {
      GtkWidget *page = g_ptr_array_steal_index (self->pages, self->pages->len - 2);

      gtk_widget_unparent (page);
      g_object_unref (page);
    }
}

void
vibeos_page_stack_set_root (VibeosPageStack *self, GtkWidget *page)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));
  g_return_if_fail (GTK_IS_WIDGET (page));

  flush_pending (self);
  settle (self);
  drop_under_top (self);
  if (self->pages->len)
    self->leaving = g_ptr_array_steal_index (self->pages, 0);
  add (self, page);
  start (self, FALSE);
}

void
vibeos_page_stack_pop_to_root (VibeosPageStack *self)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));

  flush_pending (self);
  settle (self);
  /* the ones in between go at once; the top one leaves with Back's transition */
  while (self->pages->len > 2)
    {
      GtkWidget *page = g_ptr_array_steal_index (self->pages, self->pages->len - 2);

      gtk_widget_unparent (page);
      g_object_unref (page);
    }
  vibeos_page_stack_pop (self);
}

void
vibeos_page_stack_clear (VibeosPageStack *self)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (self));

  flush_pending (self);
  settle (self);
  if (!self->pages->len)
    return;
  for (guint i = 0; i < self->pages->len; i++)
    gtk_widget_unparent (self->pages->pdata[i]);
  g_ptr_array_set_size (self->pages, 0);
  g_signal_emit (self, signals[TOP_CHANGED], 0);
}

void
vibeos_page_stack_move_pages (VibeosPageStack *from, guint index, VibeosPageStack *to)
{
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (from));
  g_return_if_fail (VIBEOS_IS_PAGE_STACK (to));

  flush_pending (from);
  flush_pending (to);
  settle (from);
  settle (to);
  if (index >= from->pages->len)
    return;
  while (from->pages->len > index)
    {
      GtkWidget *page = g_ptr_array_steal_index (from->pages, index);

      gtk_widget_unparent (page);
      g_ptr_array_add (to->pages, page);
      gtk_widget_set_parent (page, GTK_WIDGET (to));
    }
  sync_visibility (from);
  sync_visibility (to);
  g_signal_emit (from, signals[TOP_CHANGED], 0);
  g_signal_emit (to, signals[TOP_CHANGED], 0);
}

void
vibeos_page_stack_set_transition (VibeosPageStack *self, VibeosPageTransition transition)
{
  self->transition = transition;
}

VibeosPageTransition
vibeos_page_stack_get_transition (VibeosPageStack *self)
{
  return self->transition;
}

/* ---- layout and drawing ---------------------------------------------------------------------------------------- */

static void
vibeos_page_stack_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                           int *min_baseline, int *nat_baseline)
{
  *min = *nat = 0;
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    {
      int child_min, child_nat;

      gtk_widget_measure (child, orientation, for_size, &child_min, &child_nat, NULL, NULL);
      *min = MAX (*min, child_min);
      *nat = MAX (*nat, child_nat);
    }
}

static void
place (GtkWidget *page, int width, int height, double x)
{
  gtk_widget_allocate (page, width, height, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, 0)));
}

static void
vibeos_page_stack_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosPageStack *self = VIBEOS_PAGE_STACK (widget);
  GtkWidget *top = vibeos_page_stack_get_top (self);
  GtkWidget *out = vibeos_anim_is_running (self->anim) ? outgoing (self) : NULL;
  Frame f = frame_at (self, self->t);

  for (guint i = 0; i < self->pages->len; i++)
    {
      GtkWidget *page = self->pages->pdata[i];

      place (page, width, height, !out ? 0 : page == top ? f.in_x : page == out ? f.out_x : 0);
    }
  if (self->leaving)
    place (self->leaving, width, height, out == self->leaving ? f.out_x : 0);
}

static void
snapshot_page (GtkWidget *widget, GtkWidget *page, GtkSnapshot *snapshot, double alpha, double scale)
{
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);

  if (alpha <= 0)
    return;
  gtk_snapshot_push_opacity (snapshot, alpha);
  gtk_snapshot_save (snapshot);
  if (scale != 1)
    {
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (w / 2, h / 2));
      gtk_snapshot_scale (snapshot, scale, scale);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-w / 2, -h / 2));
    }
  gtk_widget_snapshot_child (widget, page, snapshot);
  gtk_snapshot_restore (snapshot);
  gtk_snapshot_pop (snapshot);
}

static void
vibeos_page_stack_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  VibeosPageStack *self = VIBEOS_PAGE_STACK (widget);
  GtkWidget *top = vibeos_page_stack_get_top (self);
  GtkWidget *out;
  Frame f;

  if (!vibeos_anim_is_running (self->anim))
    {
      if (top)
        gtk_widget_snapshot_child (widget, top, snapshot);
      return;
    }
  f = frame_at (self, self->t);
  out = outgoing (self);
  if (out)
    snapshot_page (widget, out, snapshot, f.out_alpha, 1);
  if (top)
    snapshot_page (widget, top, snapshot, f.in_alpha, f.in_scale);
}

static gboolean
vibeos_page_stack_grab_focus (GtkWidget *widget)
{
  GtkWidget *top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (widget));

  return top ? gtk_widget_grab_focus (top) : FALSE;
}

static gboolean
vibeos_page_stack_focus (GtkWidget *widget, GtkDirectionType direction)
{
  GtkWidget *top = vibeos_page_stack_get_top (VIBEOS_PAGE_STACK (widget));

  return top ? gtk_widget_child_focus (top, direction) : FALSE;
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_page_stack_dispose (GObject *object)
{
  VibeosPageStack *self = VIBEOS_PAGE_STACK (object);

  g_clear_handle_id (&self->highlight_timeout, g_source_remove);
  g_clear_object (&self->pending);
  clear_highlight (self);
  g_clear_pointer (&self->anim, vibeos_anim_free);
  finish_leaving (self);
  if (self->pages)
    {
      for (guint i = 0; i < self->pages->len; i++)
        gtk_widget_unparent (self->pages->pdata[i]);
      g_clear_pointer (&self->pages, g_ptr_array_unref);
    }
  G_OBJECT_CLASS (vibeos_page_stack_parent_class)->dispose (object);
}

static void
vibeos_page_stack_class_init (VibeosPageStackClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_page_stack_dispose;
  widget_class->measure = vibeos_page_stack_measure;
  widget_class->size_allocate = vibeos_page_stack_size_allocate;
  widget_class->snapshot = vibeos_page_stack_snapshot;
  widget_class->grab_focus = vibeos_page_stack_grab_focus;
  widget_class->focus = vibeos_page_stack_focus;
  gtk_widget_class_set_css_name (widget_class, "vibeos-page-stack");

  signals[TOP_CHANGED] = g_signal_new ("top-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                       NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vibeos_page_stack_init (VibeosPageStack *self)
{
  self->pages = g_ptr_array_new_with_free_func (g_object_unref);
  self->anim = vibeos_anim_new (GTK_WIDGET (self), on_anim, self);
  self->t = 1;
  self->highlight_ms = HIGHLIGHT_MS;
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
vibeos_page_stack_new (void)
{
  return g_object_new (VIBEOS_TYPE_PAGE_STACK, NULL);
}
