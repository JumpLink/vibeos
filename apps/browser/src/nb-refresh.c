/* Pull to refresh: pulling a page down from its top reloads it. Jelly has none; the circle, its arrow and their
 * motion follow androidx's SwipeRefreshLayout, and the rule for a web page is Chrome's: the pull starts only at the
 * page's top and only from a touch the page leaves to scrolling.
 *
 * WebKit runs the page's touches in its web process, so a script there watches them, in a world of its own (the page
 * neither sees it nor can post to it). It arms a pull when one finger lands with the page scrolled to its top,
 * outside an inner scroller that could still go up, where the page lets the finger pan vertically (touch-action) and
 * has not opted out (overscroll-behavior: none / contain on the root, as in Chrome). It drops the pull when the page
 * cancels the move (preventDefault), when the page scrolls after all, or when a second finger lands. It posts the
 * finger's travel; the circle is drawn here, over the top of the view. */
#include "nb.h"

#include <math.h>
#include <neo.h>
#include <spongy.h>

#define WORLD "nbpull"

/* the circle and its ring, px */
#define CIRCLE_D        40.0
#define SPINNER_END     64.0    /* the drag that triggers, and the slingshot */
#define DRAG_RATE       0.5     /* the circle moves half the finger's travel */
#define TOUCH_SLOP      8.0     /* px the finger moves before a pull starts */
#define RING_RADIUS     7.5
#define RING_STROKE     2.5
#define ARROW_W         10.0
#define ARROW_H         5.0
#define MAX_ARC         0.8     /* the longest arc, of a turn */
#define MIN_ARC         0.01
#define ALPHA_START     (76 / 255.0)
#define TO_START_MS     200     /* the circle going back up */
#define TO_TRIGGER_MS   200     /* the circle going to where it rests while the page loads */
#define SCALE_DOWN_MS   150     /* the circle scaling away */
#define ALPHA_MS        300     /* the ring's fade */
#define SPIN_MS         1332.0  /* one cycle of the spinning ring */

typedef enum { IDLE, PULL, TO_START, TO_TRIGGER, SPIN, SCALE_DOWN } State;

typedef struct _NbRefresh {
  GtkWidget parent;
  NbTab    *tab;
  State     state;
  /* the circle: its top (px from the view's top, -CIRCLE_D = hidden above it) and scale */
  double    top, scale, pull;
  /* the ring: its trims and rotations in turns, the arrow's scale, the alpha */
  double    start_trim, end_trim, rotation, group_rotation, arrow, alpha, alpha_from, alpha_to;
  gint64    alpha_t0;
  /* the animation in progress */
  gint64    t0;
  double    from;
  guint     tick;
  /* the spinner's cycle */
  double    spin_start, spin_rotation;
  int       spin_count;
  gint64    spin_t0;
  gboolean  saw_loading;
} NbRefresh;

typedef struct { GtkWidgetClass parent_class; } NbRefreshClass;

G_DEFINE_TYPE (NbRefresh, nb_refresh, GTK_TYPE_WIDGET)

static double
decelerate (double t)                       /* a decelerating curve, factor 2 */
{
  return 1 - pow (1 - CLAMP (t, 0, 1), 4);
}

static void set_alpha (NbRefresh *self, double to);
static void finish (NbRefresh *self);

static void
reset (NbRefresh *self)
{
  self->state = IDLE;
  self->top = -CIRCLE_D;
  self->scale = 1;
  self->pull = 0;
  self->start_trim = self->end_trim = self->rotation = self->group_rotation = self->arrow = 0;
  self->alpha = self->alpha_from = self->alpha_to = ALPHA_START;
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

/* the circle follows the pull: `pull` is the overscroll, the finger's travel past the slop times DRAG_RATE */
static void
move_spinner (NbRefresh *self, double pull)
{
  double percent = MIN (1, fabs (pull / SPINNER_END));
  double adjusted = MAX (percent - 0.4, 0) * 5 / 3;
  double extra = fabs (pull) - SPINNER_END;
  double sling = SPINNER_END;
  double tension_sling = MAX (0, MIN (extra, sling * 2) / sling);
  double tension = (tension_sling / 4 - pow (tension_sling / 4, 2)) * 2;
  double extra_move = sling * tension * 2;

  self->pull = pull;
  self->top = -CIRCLE_D + (int) (sling * percent + extra_move);
  self->scale = 1;
  self->arrow = MIN (1, adjusted);
  self->start_trim = 0;
  self->end_trim = MIN (MAX_ARC, adjusted * MAX_ARC);
  self->rotation = (-0.25 + 0.4 * adjusted + tension * 2) * 0.5;
  self->group_rotation = 0;
  set_alpha (self, pull < SPINNER_END ? ALPHA_START : 1);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
set_alpha (NbRefresh *self, double to)
{
  if (self->alpha_to == to)
    return;
  self->alpha_from = self->alpha;
  self->alpha_to = to;
  self->alpha_t0 = g_get_monotonic_time ();
}

static double
ms_since (gint64 t0)
{
  return (g_get_monotonic_time () - t0) / 1000.0;
}

/* the indeterminate ring: the arc grows to 0.8 of a turn and shrinks back over each 1332 ms cycle, while the
 * whole ring turns 216 degrees a cycle */
static void
spin (NbRefresh *self)
{
  double t = ms_since (self->spin_t0) / SPIN_MS;
  while (t >= 1) {
    /* at the repeat: the next cycle starts where this one's end left off */
    self->spin_start += MAX_ARC - MIN_ARC;
    self->spin_rotation += 1 - MAX_ARC;
    self->spin_count = (self->spin_count + 1) % 5;
    self->spin_t0 += (gint64) (SPIN_MS * 1000);
    t -= 1;
  }
  if (t < 0.5) {
    self->start_trim = self->spin_start;
    self->end_trim = self->spin_start + (MAX_ARC - MIN_ARC) * spongy_curve (SPONGY_FAST_OUT_SLOW_IN, t / 0.5) + MIN_ARC;
  } else {
    self->start_trim = self->spin_start + (MAX_ARC - MIN_ARC) * spongy_curve (SPONGY_FAST_OUT_SLOW_IN, (t - 0.5) / 0.5);
    self->end_trim = self->spin_start + (MAX_ARC - MIN_ARC) + MIN_ARC;
  }
  self->rotation = self->spin_rotation + (1 - MAX_ARC) * t;
  self->group_rotation = (216.0 / 360) * (t + self->spin_count);
}

static void
start_spin (NbRefresh *self)
{
  self->state = SPIN;
  self->arrow = 0;
  self->spin_start = self->start_trim;
  self->spin_rotation = self->rotation;
  self->spin_count = 0;
  self->spin_t0 = g_get_monotonic_time ();
  self->alpha = self->alpha_from = self->alpha_to = 1;
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  NbRefresh *self = (NbRefresh *) widget;
  double ms = ms_since (self->t0);

  if (self->alpha != self->alpha_to) {
    double a = ms_since (self->alpha_t0) / ALPHA_MS;
    self->alpha = a >= 1 ? self->alpha_to : self->alpha_from + (self->alpha_to - self->alpha_from) * MAX (a, 0);
  }

  switch (self->state) {
  case TO_START:
    self->top = self->from + (-CIRCLE_D - self->from) * decelerate (ms / TO_START_MS);
    if (ms >= TO_START_MS)
      reset (self);
    break;
  case TO_TRIGGER:
    /* to SPINNER_END - CIRCLE_D, where the circle rests while the page loads */
    self->top = self->from + (SPINNER_END - CIRCLE_D - self->from) * decelerate (ms / TO_TRIGGER_MS);
    if (ms >= TO_TRIGGER_MS)
      start_spin (self);
    break;
  case SPIN: {
    WebKitWebView *v = self->tab->view;
    gboolean loading = v && webkit_web_view_is_loading (v);
    self->saw_loading |= loading;
    spin (self);
    /* over; or a reload that never got going (WebKit had nothing to reload) must not spin for ever */
    if (!loading && (self->saw_loading || ms_since (self->spin_t0) > 1000))
      finish (self);
    break;
  }
  case SCALE_DOWN:
    if (self->spin_t0)
      spin (self);
    self->scale = 1 - CLAMP (ms / SCALE_DOWN_MS, 0, 1);
    if (ms >= SCALE_DOWN_MS)
      reset (self);
    break;
  case PULL:
  case IDLE:
    break;
  }
  gtk_widget_queue_draw (widget);
  if (self->state == IDLE || (self->state == PULL && self->alpha == self->alpha_to)) {
    self->tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
animate (NbRefresh *self, State state)
{
  self->state = state;
  self->t0 = g_get_monotonic_time ();
  self->from = self->top;
  if (!self->tick)
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}

/* the reload is over: the circle scales away */
static void
finish (NbRefresh *self)
{
  if (self->state == TO_TRIGGER)
    self->spin_t0 = 0;                      /* done before the circle got to its place: it goes from where it is */
  animate (self, SCALE_DOWN);
}

static void
nb_refresh_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  NbRefresh *self = (NbRefresh *) widget;
  if (self->state == IDLE || self->scale <= 0)
    return;

  double w = gtk_widget_get_width (widget);
  double cx = w / 2, cy = self->top + CIRCLE_D / 2, r = CIRCLE_D / 2;

  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx, cy));
  gtk_snapshot_scale (snapshot, self->scale, self->scale);

  /* the disc, raised over the page by its shadow */
  GskRoundedRect disc;
  gsk_rounded_rect_init_from_rect (&disc, &GRAPHENE_RECT_INIT (-r, -r, 2 * r, 2 * r), r);
  gtk_snapshot_append_outset_shadow (snapshot, &disc, &(GdkRGBA) { 0, 0, 0, 0.28 }, 0, 1.75, 0, 3.5);
  gtk_snapshot_push_rounded_clip (snapshot, &disc);
  GdkRGBA bg = neo_color (NEO_SURFACE_CONTAINER_HIGH);
  gtk_snapshot_append_color (snapshot, &bg, &disc.bounds);
  gtk_snapshot_pop (snapshot);

  /* the ring: angles start at 3 o'clock and grow clockwise (y down) */
  double sweep = (self->end_trim - self->start_trim) * 360;
  if (sweep > 0.1) {
    GdkRGBA color = neo_color_alpha (NEO_PRIMARY, self->alpha);
    double a0 = ((self->start_trim + self->rotation) * 360 + self->group_rotation * 360) * G_PI / 180;
    double a1 = a0 + sweep * G_PI / 180;

    g_autoptr (GskPathBuilder) b = gsk_path_builder_new ();
    gsk_path_builder_move_to (b, RING_RADIUS * cos (a0), RING_RADIUS * sin (a0));
    gsk_path_builder_svg_arc_to (b, RING_RADIUS, RING_RADIUS, 0, sweep > 180, TRUE,
                                 RING_RADIUS * cos (a1), RING_RADIUS * sin (a1));
    g_autoptr (GskPath) ring = gsk_path_builder_to_path (b);
    g_autoptr (GskStroke) stroke = gsk_stroke_new (RING_STROKE);
    gsk_stroke_set_line_cap (stroke, GSK_LINE_CAP_SQUARE);
    gtk_snapshot_append_stroke (snapshot, ring, stroke, &color);

    /* the arrow: a triangle across the ring at its end, pointing on clockwise */
    if (self->arrow > 0) {
      double aw = ARROW_W * self->arrow, ah = ARROW_H * self->arrow;
      gtk_snapshot_save (snapshot);
      gtk_snapshot_rotate (snapshot, a1 * 180 / G_PI);
      g_autoptr (GskPathBuilder) tb = gsk_path_builder_new ();
      gsk_path_builder_move_to (tb, RING_RADIUS - aw / 2, RING_STROKE / 2);
      gsk_path_builder_line_to (tb, RING_RADIUS + aw / 2, RING_STROKE / 2);
      gsk_path_builder_line_to (tb, RING_RADIUS, RING_STROKE / 2 + ah);
      gsk_path_builder_close (tb);
      g_autoptr (GskPath) tri = gsk_path_builder_to_path (tb);
      gtk_snapshot_append_fill (snapshot, tri, GSK_FILL_RULE_WINDING, &color);
      gtk_snapshot_restore (snapshot);
    }
  }
  gtk_snapshot_restore (snapshot);
}

static void
nb_refresh_dispose (GObject *object)
{
  NbRefresh *self = (NbRefresh *) object;
  if (self->tick) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
    self->tick = 0;
  }
  G_OBJECT_CLASS (nb_refresh_parent_class)->dispose (object);
}

static void
nb_refresh_class_init (NbRefreshClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = nb_refresh_dispose;
  GTK_WIDGET_CLASS (klass)->snapshot = nb_refresh_snapshot;
}

static void
nb_refresh_init (NbRefresh *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  gtk_widget_set_can_focus (GTK_WIDGET (self), FALSE);
  reset (self);
}

GtkWidget *
nb_refresh_new (NbTab *t)
{
  NbRefresh *self = g_object_new (nb_refresh_get_type (), NULL);
  self->tab = t;
  return GTK_WIDGET (self);
}

/* the view's loading changed: a refresh in progress ends with its load (not before that load has begun: right after
 * the reload WebKit may still report the old page as not loading) */
void
nb_refresh_progress (NbTab *t)
{
  NbRefresh *self = t->refresh ? (NbRefresh *) t->refresh : NULL;
  if (!self || (self->state != SPIN && self->state != TO_TRIGGER))
    return;
  if (t->view && webkit_web_view_is_loading (t->view))
    self->saw_loading = TRUE;
  else if (self->saw_loading)
    finish (self);
}

/* ---- the page's side ---- */

static const char script[] =
  "(() => {\n"
  "  const post = m => { try { window.webkit.messageHandlers.nbPull.postMessage(m); } catch (_) {} };\n"
  "  const root = () => document.scrollingElement || document.documentElement;\n"
  "  const atTop = () => window.scrollY <= 0 && (!root() || root().scrollTop <= 0) &&\n"
  "                      (!window.visualViewport || window.visualViewport.offsetTop <= 0.5);\n"
  "  const panY = v => !v || v === 'auto' || v === 'manipulation' || /pan-y|pan-up|pan-down/.test(v);\n"
  "  const refused = target => {\n"
  "    for (const n of [document.documentElement, document.body])\n"
  "      if (n && /none|contain/.test(getComputedStyle(n).overscrollBehaviorY || '')) return true;\n"
  "    for (let n = target instanceof Element ? target : target && target.parentElement; n; n = n.parentElement) {\n"
  "      const s = getComputedStyle(n);\n"
  "      if (!panY(s.touchAction)) return true;\n"
  "      if (n !== document.documentElement && n !== document.body && n.scrollTop > 0 &&\n"
  "          /auto|scroll|overlay/.test(s.overflowY)) return true;\n"
  "    }\n"
  "    return false;\n"
  "  };\n"
  "  let id = null, y0 = 0, on = false;\n"
  "  const stop = how => { if (on) post({ t: how }); on = false; id = null; };\n"
  "  const ours = e => id !== null && Array.prototype.find.call(e.changedTouches, x => x.identifier === id);\n"
  "  addEventListener('touchstart', e => {\n"
  "    if (e.touches.length !== 1) { stop('cancel'); return; }\n"
  "    const t = e.changedTouches[0];\n"
  "    id = atTop() && !refused(e.target) ? t.identifier : null;\n"
  "    y0 = t.clientY;\n"
  "  }, { capture: true, passive: true });\n"
  "  addEventListener('touchmove', e => {\n"
  "    const t = ours(e);\n"
  "    if (!t) return;\n"
  "    if (e.touches.length !== 1 || e.defaultPrevented || !atTop()) { stop('cancel'); return; }\n"
  "    const d = (t.clientY - y0) * (window.visualViewport ? window.visualViewport.scale : 1);\n"
  "    if (!on && d <= 0) return;\n"
  "    on = true;\n"
  "    post({ t: 'move', d: d });\n"
  "  }, { passive: true });\n"
  "  addEventListener('touchend', e => { if (ours(e)) stop(e.defaultPrevented ? 'cancel' : 'end'); }, { passive: true });\n"
  "  addEventListener('touchcancel', e => { if (ours(e)) stop('cancel'); }, { passive: true });\n"
  "})();\n";

static NbTab *
touched_tab (void)
{
  /* the script's messages do not say which view sent them (one content manager serves every tab), but only the
   * tab shown takes touches */
  return nb_tab_is_shown (nb->last_active) ? nb->last_active : NULL;
}

static void
on_message (WebKitUserContentManager *ucm, JSCValue *value, gpointer data)
{
  NbTab *t = touched_tab ();
  if (!t || !t->refresh || !t->view || t->fullscreen || !jsc_value_is_object (value))
    return;
  NbRefresh *self = (NbRefresh *) t->refresh;

  g_autoptr (JSCValue) kind_v = jsc_value_object_get_property (value, "t");
  g_autofree char *kind = jsc_value_to_string (kind_v);

  if (g_strcmp0 (kind, "move") == 0) {
    if (self->state != IDLE && self->state != PULL)
      return;                               /* a refresh already runs, or the circle is still on its way */
    g_autoptr (JSCValue) d_v = jsc_value_object_get_property (value, "d");
    double d = jsc_value_to_double (d_v) * webkit_web_view_get_zoom_level (t->view);
    double pull = MAX (0, d - TOUCH_SLOP) * DRAG_RATE;
    if (pull <= 0 && self->state == IDLE)
      return;
    self->state = PULL;
    move_spinner (self, pull);
    if (!self->tick)
      self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
  } else if (self->state == PULL) {
    /* the finger lifts: past the mark it refreshes, short of it the circle goes back */
    gboolean go = g_strcmp0 (kind, "end") == 0 && self->pull > SPINNER_END;
    if (go) {
      self->saw_loading = FALSE;
      self->spin_t0 = 0;
      animate (self, TO_TRIGGER);
      webkit_web_view_reload (t->view);
    } else {
      self->start_trim = self->end_trim = 0;
      self->arrow = 0;
      animate (self, TO_START);
    }
  }
}

void
nb_refresh_setup (void)
{
  g_signal_connect (nb->ucm, "script-message-received::nbPull", G_CALLBACK (on_message), NULL);
  webkit_user_content_manager_register_script_message_handler (nb->ucm, "nbPull", WORLD);
  WebKitUserScript *s = webkit_user_script_new_for_world (script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                          WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                          WORLD, NULL, NULL);
  webkit_user_content_manager_add_script (nb->ucm, s);
  webkit_user_script_unref (s);
}
