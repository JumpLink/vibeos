/* spongy-gtk (see spongy-gtk.h).
 *
 * The child is laid out at its full height and moved by its allocation's transform, rounded to whole device
 * pixels; a GtkScrollable child is given the view's size and scrolled through its vadjustment instead. The touch is
 * the view's only once it has moved past the slop (or at once, when it stops a fling or catches a stretch): until
 * then a row under it can still take the tap, and claiming it cancels the row's press.
 *
 * GTK's renderers have no custom shaders (GskGLShader only ran on the old GL renderer), so the stretch is drawn by
 * placing the child's render nodes again (see stretch_node). */
#include "spongy-gtk.h"
#include "spongy.h"

#include <math.h>

/* android.view.ViewConfiguration: a touch in a scrolling list shows its press only after TAP_TIMEOUT without
 * scrolling (View.isInScrollingContainer); a tap quicker than that flashes it for PRESSED_STATE_DURATION */
#define TAP_TIMEOUT_MS            100
#define PRESSED_STATE_DURATION_MS 64

struct _SpongyScrollView {
  GtkWidget      parent_instance;
  GtkWidget     *child;
  GtkAdjustment *vadj;         /* a GtkScrollable child's */
  SpongyList     list;
  float          stretch;      /* as drawn: the top edge's amount (positive) plus the bottom's (negative) */
  double         offset;       /* as laid out and notified */
  guint          tick;
  double         clock_offset; /* monotonic ms − event ms, from the touch's first event */
  double         start_y;
  bool           setting_adj;
  GtkWidget     *pressed;      /* what the touch went down on (weak) */
  guint          press_timer;  /* to show the press, or to end a tap's flash of it */
};

enum { PROP_0, PROP_CHILD, PROP_OFFSET, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE (SpongyScrollView, spongy_scroll_view, GTK_TYPE_WIDGET)

static double
now_ms (void)
{
  return g_get_monotonic_time () / 1000.0;
}

static double
event_ms (SpongyScrollView *self, GtkEventController *c)
{
  return gtk_event_controller_get_current_event_time (c) + self->clock_offset;
}

static double
device_scale (SpongyScrollView *self)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;
  return surface ? gdk_surface_get_scale (surface) : 1.0;
}

/* Android's dp in this surface's logical pixels: SPONGY_DENSITY (screen px per dp), or the monitor's dots per inch
 * over 160 */
static void
update_density (SpongyScrollView *self)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;
  if (!surface)
    return;
  double scale = gdk_surface_get_scale (surface);
  const char *env = g_getenv ("SPONGY_DENSITY");
  double px_per_dp = env ? g_ascii_strtod (env, NULL) : 0;
  if (px_per_dp <= 0) {
    GdkMonitor *m = gdk_display_get_monitor_at_surface (gdk_surface_get_display (surface), surface);
    int mm = m ? gdk_monitor_get_height_mm (m) : 0;
    if (mm > 0) {
      GdkRectangle geo;
      gdk_monitor_get_geometry (m, &geo);
      px_per_dp = geo.height * gdk_monitor_get_scale (m) / (mm / 25.4) / 160;
    } else {
      px_per_dp = scale;
    }
  }
  spongy_set_density (px_per_dp / scale);
  double density = spongy_density ();
  self->list.slop = (int) lround (8 * density);
  self->list.min_fling = (int) lround (50 * density);
  self->list.max_fling = (int) lround (8000 * density);
}

/* the list's offset to the child: its allocation, or its adjustment */
static void
sync_offset (SpongyScrollView *self)
{
  double offset = self->list.offset;
  if (self->vadj) {
    if (gtk_adjustment_get_value (self->vadj) != offset) {
      self->setting_adj = true;
      gtk_adjustment_set_value (self->vadj, offset);
      self->setting_adj = false;
    }
  } else if (offset != self->offset) {
    gtk_widget_queue_allocate (GTK_WIDGET (self));
  }
  if (offset != self->offset) {
    self->offset = offset;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFSET]);
  }
}

static gboolean
tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  float top, bottom;
  bool more = spongy_list_frame (&self->list, gdk_frame_clock_get_frame_time (clock) / 1000.0, &top, &bottom);

  sync_offset (self);
  if (top + bottom != self->stretch) {
    self->stretch = top + bottom;
    gtk_widget_queue_draw (widget);
  }
  if (more)
    return G_SOURCE_CONTINUE;
  self->tick = 0;
  return G_SOURCE_REMOVE;
}

static void
start_ticking (SpongyScrollView *self)
{
  if (!self->tick && gtk_widget_get_mapped (GTK_WIDGET (self)))
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), tick_cb, NULL, NULL);
}

/* ---- the press: GTK marks what a touch went down on :active (up to the window) at once and until it lifts, even
 * when a scroll takes the touch; here it is hidden until TAP_TIMEOUT, and again once the list scrolls. GTK's own
 * count of it is left alone: its release clears the flags whatever is set here. ---- */

static void
press_look (SpongyScrollView *self, gboolean on)
{
  for (GtkWidget *w = self->pressed; w && w != GTK_WIDGET (self); w = gtk_widget_get_parent (w)) {
    if (on)
      gtk_widget_set_state_flags (w, GTK_STATE_FLAG_ACTIVE, FALSE);
    else
      gtk_widget_unset_state_flags (w, GTK_STATE_FLAG_ACTIVE);
  }
}

static void
press_forget (SpongyScrollView *self)
{
  g_clear_handle_id (&self->press_timer, g_source_remove);
  g_clear_weak_pointer (&self->pressed);
}

static gboolean
press_show (gpointer data)
{
  SpongyScrollView *self = data;
  self->press_timer = 0;
  press_look (self, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean
press_flash_end (gpointer data)
{
  SpongyScrollView *self = data;
  self->press_timer = 0;
  press_look (self, FALSE);
  g_clear_weak_pointer (&self->pressed);
  return G_SOURCE_REMOVE;
}

/* ---- touch ---- */

static void
drag_begin (GtkGestureDrag *drag, double x, double y, SpongyScrollView *self)
{
  GtkEventController *c = GTK_EVENT_CONTROLLER (drag);
  self->clock_offset = now_ms () - gtk_event_controller_get_current_event_time (c);
  self->start_y = y;
  spongy_list_down (&self->list, y, event_ms (self, c));
  press_forget (self);
  g_set_weak_pointer (&self->pressed, gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT));
  press_look (self, FALSE);
  /* a fling stopped or a stretch caught: the touch is the list's from the start, it taps nothing */
  if (self->list.dragging)
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  else
    self->press_timer = g_timeout_add (TAP_TIMEOUT_MS, press_show, self);
  start_ticking (self);
}

static void
drag_update (GtkGestureDrag *drag, double dx, double dy, SpongyScrollView *self)
{
  GtkEventController *c = GTK_EVENT_CONTROLLER (drag);
  if (!self->list.down)
    return;
  /* sideways past the slop first: not a scroll (the navigation view's swipe back, a slider) */
  if (!self->list.dragging && fabs (dx) > self->list.slop && fabs (dx) > fabs (dy)) {
    spongy_list_cancel (&self->list, event_ms (self, c));
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
    start_ticking (self);
    return;
  }
  /* the moves GDK folded into this event, for the velocity (only a pointer's are: touches come one by one) */
  GdkEvent *event = gtk_event_controller_get_current_event (c);
  double ex, ey;
  if (event && gdk_event_get_event_type (event) == GDK_MOTION_NOTIFY && gdk_event_get_position (event, &ex, &ey)) {
    guint n = 0;
    GdkTimeCoord *history = gdk_event_get_history (event, &n);
    double to_widget = self->start_y + dy - ey;
    for (guint i = 0; i < n; i++) {
      if (history[i].flags & GDK_AXIS_FLAG_Y)
        spongy_list_move (&self->list, history[i].axes[GDK_AXIS_Y] + to_widget, history[i].time + self->clock_offset);
    }
    g_free (history);
  }
  if (spongy_list_move (&self->list, self->start_y + dy, event_ms (self, c)) &&
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED)) {
    press_look (self, FALSE);
    press_forget (self);
  }
  sync_offset (self);
  start_ticking (self);
}

static void
drag_end (GtkGestureDrag *drag, double dx, double dy, SpongyScrollView *self)
{
  /* a tap before the press showed: GTK has just cleared it, flash it */
  if (self->list.down && !self->list.dragging && self->press_timer && self->pressed) {
    g_source_remove (self->press_timer);
    press_look (self, TRUE);
    self->press_timer = g_timeout_add (PRESSED_STATE_DURATION_MS, press_flash_end, self);
  } else {
    press_forget (self);
  }
  spongy_list_up (&self->list, event_ms (self, GTK_EVENT_CONTROLLER (drag)));   /* nothing after a cancel */
  start_ticking (self);
}

static void
drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, SpongyScrollView *self)
{
  press_forget (self);
  spongy_list_cancel (&self->list, now_ms ());
  start_ticking (self);
}

/* ---- wheels and touchpads ---- */

static gboolean
scrolled (GtkEventControllerScroll *c, double dx, double dy, SpongyScrollView *self)
{
  /* a wheel's notch as GtkScrolledWindow takes it */
  double step = gtk_event_controller_scroll_get_unit (c) == GDK_SCROLL_UNIT_WHEEL ?
                pow (self->list.view_height, 2.0 / 3.0) : 1.0;
  if (dy == 0)
    return GDK_EVENT_PROPAGATE;
  spongy_list_scroll_to (&self->list, self->list.offset + dy * step);
  sync_offset (self);
  return GDK_EVENT_STOP;
}

static void
decelerate (GtkEventControllerScroll *c, double vx, double vy, SpongyScrollView *self)
{
  spongy_list_fling (&self->list, vy, now_ms ());
  start_ticking (self);
}

/* ---- a scrollable child's adjustment ---- */

static void
adj_value_changed (GtkAdjustment *adj, SpongyScrollView *self)
{
  if (self->setting_adj)
    return;
  /* the child moved itself: to a focused row, or a list view placing its rows again around the one it anchors
   * to (which must not stop a fling) */
  self->list.offset = fminf (fmaxf (0, gtk_adjustment_get_value (adj)), self->list.max_offset);
  self->offset = self->list.offset;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFSET]);
}

static void
adj_changed (GtkAdjustment *adj, SpongyScrollView *self)
{
  spongy_list_set_size (&self->list, gtk_adjustment_get_page_size (adj), gtk_adjustment_get_upper (adj));
  self->list.offset = fminf (gtk_adjustment_get_value (adj), self->list.max_offset);
  self->offset = self->list.offset;
}

/* ---- layout ---- */

static GtkSizeRequestMode
get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
         int *nat_base)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  *min = *nat = 0;
  if (!self->child)
    return;
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    gtk_widget_measure (self->child, orientation, -1, min, nat, NULL, NULL);
  } else {
    int child_min;
    gtk_widget_measure (self->child, orientation, for_size, &child_min, nat, NULL, NULL);
  }
}

static void
size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  if (!self->child)
    return;
  if (self->vadj) {
    gtk_widget_allocate (self->child, width, height, -1, NULL);   /* configures the adjustment */
    return;
  }
  int min, nat;
  gtk_widget_measure (self->child, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  int content = MAX (nat, height);
  spongy_list_set_size (&self->list, height, content);
  self->offset = self->list.offset;
  double scale = device_scale (self);
  float y = (float) (round (self->offset * scale) / scale);
  gtk_widget_allocate (self->child, width, content, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, -y)));
}

/* ---- drawing ---- */

/* ---- the stretch: the child's render nodes placed again where hwui's shader would sample them ----
 *
 * The child's node tree is rebuilt in this widget's coordinates with every leaf (a background, an icon, a line)
 * moved and scaled so its top and bottom edges land exactly where the stretch puts them, clips the same, and text
 * only moved, by whole device pixels: a glyph scaled by a factor that changes every frame is drawn again for each
 * one (half a second a frame here), and a 3 % stretch of a line of text does not show. Every node is still drawn
 * once. */

typedef struct {
  float  amount, H;
  double scale;          /* device pixels per logical one */
} Stretch;

typedef struct {
  float sx, sy, dx, dy;  /* node coordinates to this widget's: x' = sx x + dx */
} Affine;

/* where the stretch shows what is at y unstretched */
static float
stretched_y (const Stretch *s, float y)
{
  return spongy_stretch_unmap (CLAMP (y, -s->H, 2 * s->H), s->amount, s->H);
}

static GskRenderNode *
moved (GskRenderNode *node, const Affine *t, float ky, float ty)
{
  float dy = ky * t->dy + ty, sy = ky * t->sy;
  if (t->sx == 1 && sy == 1 && t->dx == 0 && dy == 0)
    return gsk_render_node_ref (node);
  GskTransform *tr = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (t->dx, dy));
  tr = gsk_transform_scale (tr, t->sx, sy);
  GskRenderNode *out = gsk_transform_node_new (node, tr);
  gsk_transform_unref (tr);
  return out;
}

static GskRenderNode *
stretch_leaf (const Stretch *s, GskRenderNode *node, const Affine *t)
{
  graphene_rect_t b;
  gsk_render_node_get_bounds (node, &b);
  float y0 = t->sy * b.origin.y + t->dy, y1 = y0 + t->sy * b.size.height;
  float margin = fabsf (s->amount) * s->H + 1;
  if (y1 < -margin || y0 > s->H + margin)
    return NULL;
  if (gsk_render_node_get_node_type (node) == GSK_TEXT_NODE || y1 <= y0) {
    float c = (y0 + y1) / 2;
    return moved (node, t, 1, (float) (round ((stretched_y (s, c) - c) * s->scale) / s->scale));
  }
  /* what of it shows: its edges outside the view would be stretched by the curve's extrapolation */
  float a = CLAMP (y0, 0, s->H), z = CLAMP (y1, 0, s->H);
  if (z <= a)
    return NULL;
  float u0 = stretched_y (s, a), u1 = stretched_y (s, z), ky = (u1 - u0) / (z - a);
  return moved (node, t, ky, u0 - a * ky);
}

/* a rectangle in node coordinates, stretched, in this widget's */
static graphene_rect_t
stretch_rect (const Stretch *s, const graphene_rect_t *r, const Affine *t)
{
  float y0 = t->sy * r->origin.y + t->dy, y1 = y0 + t->sy * r->size.height;
  float u0 = stretched_y (s, y0), u1 = stretched_y (s, y1);
  return GRAPHENE_RECT_INIT (t->sx * r->origin.x + t->dx, u0, t->sx * r->size.width, u1 - u0);
}

static GskRenderNode *
stretch_node (const Stretch *s, GskRenderNode *node, const Affine *t)
{
  switch (gsk_render_node_get_node_type (node)) {
  case GSK_CONTAINER_NODE: {
    guint n = gsk_container_node_get_n_children (node), k = 0;
    GskRenderNode **children = g_newa (GskRenderNode *, MAX (n, 1));
    for (guint i = 0; i < n; i++) {
      GskRenderNode *child = stretch_node (s, gsk_container_node_get_child (node, i), t);
      if (child)
        children[k++] = child;
    }
    if (k <= 1)
      return k ? children[0] : NULL;
    GskRenderNode *out = gsk_container_node_new (children, k);
    for (guint i = 0; i < k; i++)
      gsk_render_node_unref (children[i]);
    return out;
  }
  case GSK_TRANSFORM_NODE: {
    GskTransform *tr = gsk_transform_node_get_transform (node);
    if (gsk_transform_get_category (tr) < GSK_TRANSFORM_CATEGORY_2D_AFFINE)
      return stretch_leaf (s, node, t);
    float sx, sy, dx, dy;
    gsk_transform_to_affine (tr, &sx, &sy, &dx, &dy);
    Affine inner = { t->sx * sx, t->sy * sy, t->sx * dx + t->dx, t->sy * dy + t->dy };
    return stretch_node (s, gsk_transform_node_get_child (node), &inner);
  }
  case GSK_CLIP_NODE: {
    GskRenderNode *child = stretch_node (s, gsk_clip_node_get_child (node), t);
    if (!child)
      return NULL;
    graphene_rect_t clip = stretch_rect (s, gsk_clip_node_get_clip (node), t);
    GskRenderNode *out = gsk_clip_node_new (child, &clip);
    gsk_render_node_unref (child);
    return out;
  }
  case GSK_ROUNDED_CLIP_NODE: {
    GskRenderNode *child = stretch_node (s, gsk_rounded_clip_node_get_child (node), t);
    if (!child)
      return NULL;
    const GskRoundedRect *from = gsk_rounded_clip_node_get_clip (node);
    GskRoundedRect clip = *from;
    clip.bounds = stretch_rect (s, &from->bounds, t);
    float ky = from->bounds.size.height > 0 ? clip.bounds.size.height / from->bounds.size.height : t->sy;
    for (int i = 0; i < 4; i++) {
      clip.corner[i].width *= t->sx;
      clip.corner[i].height *= ky;
    }
    GskRenderNode *out = gsk_rounded_clip_node_new (child, &clip);
    gsk_render_node_unref (child);
    return out;
  }
  case GSK_OPACITY_NODE: {
    GskRenderNode *child = stretch_node (s, gsk_opacity_node_get_child (node), t);
    if (!child)
      return NULL;
    GskRenderNode *out = gsk_opacity_node_new (child, gsk_opacity_node_get_opacity (node));
    gsk_render_node_unref (child);
    return out;
  }
  case GSK_DEBUG_NODE:
    return stretch_node (s, gsk_debug_node_get_child (node), t);
  default:
    return stretch_leaf (s, node, t);
  }
}

static void
snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  float W = gtk_widget_get_width (widget), H = gtk_widget_get_height (widget);
  if (!self->child)
    return;

  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, W, H));
  if (self->stretch == 0 || H <= 0) {
    gtk_widget_snapshot_child (widget, self->child, snapshot);
    gtk_snapshot_pop (snapshot);
    return;
  }
  GtkSnapshot *sub = gtk_snapshot_new ();
  gtk_widget_snapshot_child (widget, self->child, sub);
  GskRenderNode *node = gtk_snapshot_free_to_node (sub);
  if (node) {
    Stretch s = { self->stretch, H, device_scale (self) };
    Affine identity = { 1, 1, 0, 0 };
    GskRenderNode *out = stretch_node (&s, node, &identity);
    if (out) {
      gtk_snapshot_append_node (snapshot, out);
      gsk_render_node_unref (out);
    }
    gsk_render_node_unref (node);
  }
  gtk_snapshot_pop (snapshot);
}

/* ---- the widget ---- */

static void
realize (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (spongy_scroll_view_parent_class)->realize (widget);
  update_density (SPONGY_SCROLL_VIEW (widget));
}

static void
unmap (GtkWidget *widget)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  if (self->tick) {
    gtk_widget_remove_tick_callback (widget, self->tick);
    self->tick = 0;
  }
  press_forget (self);
  /* nothing is left half stretched or flinging for the next time it shows */
  spongy_list_cancel (&self->list, now_ms ());
  spongy_list_scroll_to (&self->list, self->list.offset);
  spongy_edge_finish (&self->list.top);
  spongy_edge_finish (&self->list.bottom);
  self->stretch = 0;
  GTK_WIDGET_CLASS (spongy_scroll_view_parent_class)->unmap (widget);
}

static void
set_adjustment (SpongyScrollView *self, GtkAdjustment *adj)
{
  if (self->vadj) {
    g_signal_handlers_disconnect_by_data (self->vadj, self);
    g_clear_object (&self->vadj);
  }
  if (adj) {
    self->vadj = g_object_ref_sink (adj);
    g_signal_connect (adj, "value-changed", G_CALLBACK (adj_value_changed), self);
    g_signal_connect (adj, "changed", G_CALLBACK (adj_changed), self);
  }
}

void
spongy_scroll_view_set_child (SpongyScrollView *self, GtkWidget *child)
{
  g_return_if_fail (SPONGY_IS_SCROLL_VIEW (self));
  g_return_if_fail (child == NULL || GTK_IS_WIDGET (child));
  if (self->child == child)
    return;
  if (self->child) {
    if (GTK_IS_SCROLLABLE (self->child))
      g_object_set (self->child, "vadjustment", NULL, "hadjustment", NULL, NULL);
    gtk_widget_unparent (self->child);
  }
  set_adjustment (self, NULL);
  self->child = child;
  spongy_list_scroll_to (&self->list, 0);
  self->offset = 0;
  if (child) {
    gtk_widget_set_parent (child, GTK_WIDGET (self));
    if (GTK_IS_SCROLLABLE (child)) {
      set_adjustment (self, gtk_adjustment_new (0, 0, 0, 0, 0, 0));
      g_object_set (child, "vadjustment", self->vadj, "hadjustment", gtk_adjustment_new (0, 0, 0, 0, 0, 0), NULL);
    }
  }
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CHILD]);
}

/**
 * spongy_scroll_view_get_child:
 *
 * Returns: (transfer none) (nullable): the child
 */
GtkWidget *
spongy_scroll_view_get_child (SpongyScrollView *self)
{
  g_return_val_if_fail (SPONGY_IS_SCROLL_VIEW (self), NULL);
  return self->child;
}

double
spongy_scroll_view_get_offset (SpongyScrollView *self)
{
  g_return_val_if_fail (SPONGY_IS_SCROLL_VIEW (self), 0);
  return self->offset;
}

void
spongy_scroll_view_scroll_to (SpongyScrollView *self, double offset)
{
  g_return_if_fail (SPONGY_IS_SCROLL_VIEW (self));
  spongy_list_scroll_to (&self->list, (float) offset);
  sync_offset (self);
}

/**
 * spongy_scroll_view_new:
 *
 * Returns: (transfer floating): a new SpongyScrollView
 */
GtkWidget *
spongy_scroll_view_new (void)
{
  return g_object_new (SPONGY_TYPE_SCROLL_VIEW, NULL);
}

static void
get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (object);
  switch (id) {
  case PROP_CHILD: g_value_set_object (value, self->child); break;
  case PROP_OFFSET: g_value_set_double (value, self->offset); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (object);
  switch (id) {
  case PROP_CHILD: spongy_scroll_view_set_child (self, g_value_get_object (value)); break;
  case PROP_OFFSET: spongy_scroll_view_scroll_to (self, g_value_get_double (value)); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
dispose (GObject *object)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (object);
  press_forget (self);
  spongy_scroll_view_set_child (self, NULL);
  G_OBJECT_CLASS (spongy_scroll_view_parent_class)->dispose (object);
}

static void
spongy_scroll_view_class_init (SpongyScrollViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = get_property;
  object_class->set_property = set_property;
  object_class->dispose = dispose;
  widget_class->get_request_mode = get_request_mode;
  widget_class->measure = measure;
  widget_class->size_allocate = size_allocate;
  widget_class->snapshot = snapshot;
  widget_class->realize = realize;
  widget_class->unmap = unmap;

  props[PROP_CHILD] = g_param_spec_object ("child", NULL, NULL, GTK_TYPE_WIDGET,
                                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_OFFSET] = g_param_spec_double ("offset", NULL, NULL, 0, G_MAXDOUBLE, 0,
                                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, props);
  gtk_widget_class_set_css_name (widget_class, "spongyscrollview");
}

static void
spongy_scroll_view_init (SpongyScrollView *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  spongy_list_init (&self->list, 0, 0);
  gtk_widget_set_overflow (widget, GTK_OVERFLOW_HIDDEN);

  /* in the capture phase, so the drag is seen before the rows' presses take it */
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (drag), TRUE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (drag_cancel), self);
  gtk_widget_add_controller (widget, GTK_EVENT_CONTROLLER (drag));

  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                                                GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
  g_signal_connect (scroll, "scroll", G_CALLBACK (scrolled), self);
  g_signal_connect (scroll, "decelerate", G_CALLBACK (decelerate), self);
  gtk_widget_add_controller (widget, scroll);
}
