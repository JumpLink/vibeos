/* spongy-gtk (see spongy-gtk.h).
 *
 * The child is laid out at its full height (its full width, sideways) and moved by its allocation's transform,
 * rounded to whole device pixels; a GtkScrollable child is given the view's size and scrolled through its adjustment
 * instead. One SpongyList does the scrolling either way: it only knows positions along the axis. The touch is
 * the view's only once it has moved past the slop (or at once, when it stops a fling or catches a stretch): until
 * then a row under it can still take the tap, and claiming it cancels the row's press.
 *
 * GTK's renderers have no custom shaders (GskGLShader only ran on the old GL renderer), so the stretch is drawn by
 * placing the child's render nodes again (see stretch_node). */
#include "spongy-gtk.h"
#include "spongy.h"

#include <math.h>

/* A touch in a scrolling list shows its press only after TAP_TIMEOUT without scrolling, so a finger about to scroll
 * does not flash the row it began on; a tap quicker than that flashes it for PRESSED_STATE_DURATION */
#define TAP_TIMEOUT_MS            100
#define PRESSED_STATE_DURATION_MS 64

struct _SpongyScrollView {
  GtkWidget      parent_instance;
  GtkWidget     *child;
  GtkAdjustment *adj;          /* a GtkScrollable child's, along the axis */
  GtkAdjustment *own_adj;      /* any other child's: follows the list (get_vadjustment, get_hadjustment) */
  bool           allocating;
  SpongyList     list;         /* top and bottom: the left and right edges, sideways */
  float          stretch;      /* as drawn: the top edge's amount (positive) plus the bottom's (negative) */
  double         offset;       /* as laid out and notified */
  guint          tick;
  double         clock_offset; /* monotonic ms − event ms, from the touch's first event */
  double         start_pos;    /* where the touch went down, along the axis */
  bool           setting_adj;
  GtkWidget     *pressed;      /* what the touch went down on (weak) */
  guint          press_timer;  /* to show the press, or to end a tap's flash of it */
  float          top_at_down, bottom_at_down;   /* the edges' stretch when the touch went down */
  bool           overshot_top, overshot_bottom; /* edge-overshot sent for this touch */
  bool           dragged;      /* this touch scrolled the list: it snaps when let go */
  GdkSurface    *surface;      /* realized on: its scale is followed */
  GtkOrientation orientation;
  GtkEventController *scroll;  /* wheels and touchpads, along the axis */
  double         snap_interval;
  bool           snap_pages;
};

enum { PROP_0, PROP_CHILD, PROP_OFFSET, N_PROPS, PROP_ORIENTATION = N_PROPS };
static GParamSpec *props[N_PROPS];
enum { EDGE_OVERSHOT, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE_WITH_CODE (SpongyScrollView, spongy_scroll_view, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_ORIENTABLE, NULL))

static inline bool
horizontal (SpongyScrollView *self)
{
  return self->orientation == GTK_ORIENTATION_HORIZONTAL;
}

/* a point's coordinate along the axis the view scrolls, and across it */
static inline double
along (SpongyScrollView *self, double x, double y)
{
  return horizontal (self) ? x : y;
}

static inline double
across (SpongyScrollView *self, double x, double y)
{
  return horizontal (self) ? y : x;
}

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

/* The physics' unit length (a dp) in this surface's logical pixels: one, since a logical pixel is a dp at any scale
 * the screen is set to, unless SPONGY_DENSITY gives a dp in screen pixels, which the surface's scale turns into
 * logical ones */
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
  if (px_per_dp <= 0)
    px_per_dp = scale;
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
  if (self->adj) {
    if (gtk_adjustment_get_value (self->adj) != offset) {
      self->setting_adj = true;
      gtk_adjustment_set_value (self->adj, offset);
      self->setting_adj = false;
    }
  } else {
    if (gtk_adjustment_get_value (self->own_adj) != offset) {
      self->setting_adj = true;
      gtk_adjustment_set_value (self->own_adj, offset);
      self->setting_adj = false;
    }
    if (offset != self->offset)
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

/* ---- snapping ---- */

/* the snap grid's interval, 0 for none: a pager without one pages by the view's size */
static float
snap_interval (SpongyScrollView *self)
{
  if (self->adj)
    return 0;
  if (self->snap_interval > 0)
    return (float) self->snap_interval;
  return self->snap_pages ? self->list.view_height : 0;
}

/* the list just let go (or flung by a touchpad), aimed at its snap point; where it will rest */
static float
snap (SpongyScrollView *self, double t)
{
  float interval = snap_interval (self);
  if (interval <= 0)
    return self->list.offset;
  return spongy_list_snap (&self->list, self->snap_pages ? SPONGY_SNAP_PAGES : SPONGY_SNAP_ITEMS, interval, t);
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
emit_overshot (SpongyScrollView *self, GtkPositionType pos)
{
  if (horizontal (self))
    pos = pos == GTK_POS_TOP ? GTK_POS_LEFT : GTK_POS_RIGHT;
  /* a handler may take the view out of its window (a refresh building the list again) */
  g_object_ref (self);
  g_signal_emit (self, signals[EDGE_OVERSHOT], 0, pos);
  g_object_unref (self);
}

/* edge-overshot: the touch pulled an edge further than it found it */
static void
overshoot (SpongyScrollView *self)
{
  if (!self->list.dragging)
    return;
  if (!self->overshot_top && self->list.top.state == SPONGY_EDGE_PULL &&
      self->list.top.distance > self->top_at_down) {
    self->overshot_top = true;
    emit_overshot (self, GTK_POS_TOP);
  }
  if (!self->overshot_bottom && self->list.bottom.state == SPONGY_EDGE_PULL &&
      self->list.bottom.distance > self->bottom_at_down) {
    self->overshot_bottom = true;
    emit_overshot (self, GTK_POS_BOTTOM);
  }
}

static void
drag_begin (GtkGestureDrag *drag, double x, double y, SpongyScrollView *self)
{
  GtkEventController *c = GTK_EVENT_CONTROLLER (drag);
  self->clock_offset = now_ms () - gtk_event_controller_get_current_event_time (c);
  self->start_pos = along (self, x, y);
  self->top_at_down = self->list.top.distance;
  self->bottom_at_down = self->list.bottom.distance;
  self->overshot_top = self->overshot_bottom = false;
  spongy_list_down (&self->list, self->start_pos, event_ms (self, c));
  self->dragged = self->list.dragging;
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
  double d = along (self, dx, dy), cross = across (self, dx, dy);
  if (!self->list.down)
    return;
  /* across past the slop first: not a scroll of this view (the navigation view's swipe back, a slider, a view that
   * scrolls the other way, in it or around it) */
  if (!self->list.dragging && fabs (cross) > self->list.slop && fabs (cross) > fabs (d)) {
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
    double to_widget = self->start_pos + d - along (self, ex, ey);
    GdkAxisUse axis = horizontal (self) ? GDK_AXIS_X : GDK_AXIS_Y;
    GdkAxisFlags flag = horizontal (self) ? GDK_AXIS_FLAG_X : GDK_AXIS_FLAG_Y;
    for (guint i = 0; i < n; i++) {
      if (history[i].flags & flag)
        spongy_list_move (&self->list, history[i].axes[axis] + to_widget, history[i].time + self->clock_offset);
    }
    g_free (history);
  }
  if (spongy_list_move (&self->list, self->start_pos + d, event_ms (self, c)) &&
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED)) {
    press_look (self, FALSE);
    press_forget (self);
  }
  if (self->list.dragging)
    self->dragged = true;
  sync_offset (self);
  start_ticking (self);
  overshoot (self);
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
  double t = event_ms (self, GTK_EVENT_CONTROLLER (drag));
  spongy_list_up (&self->list, t);   /* nothing after a cancel */
  if (self->dragged)
    snap (self, t);
  self->dragged = false;
  start_ticking (self);
}

static void
drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, SpongyScrollView *self)
{
  press_forget (self);
  spongy_list_cancel (&self->list, now_ms ());
  /* a drag taken away half way still comes to rest on its grid */
  if (self->dragged)
    snap (self, now_ms ());
  self->dragged = false;
  start_ticking (self);
}

/* ---- wheels and touchpads ---- */

static gboolean
scrolled (GtkEventControllerScroll *c, double dx, double dy, SpongyScrollView *self)
{
  /* a wheel's notch as GtkScrolledWindow takes it */
  double step = gtk_event_controller_scroll_get_unit (c) == GDK_SCROLL_UNIT_WHEEL ?
                pow (self->list.view_height, 2.0 / 3.0) : 1.0;
  double delta = along (self, dx, dy);
  if (delta == 0)
    return GDK_EVENT_PROPAGATE;
  bool past_top = delta < 0 && self->list.offset <= 0;
  bool past_bottom = delta > 0 && self->list.offset >= self->list.max_offset;
  spongy_list_scroll_to (&self->list, self->list.offset + delta * step);
  sync_offset (self);
  if (past_top || past_bottom)
    emit_overshot (self, past_top ? GTK_POS_TOP : GTK_POS_BOTTOM);
  return GDK_EVENT_STOP;
}

static void
decelerate (GtkEventControllerScroll *c, double vx, double vy, SpongyScrollView *self)
{
  double t = now_ms ();
  spongy_list_fling (&self->list, along (self, vx, vy), t);
  snap (self, t);
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

/* get_vadjustment's for a plain child: the app set its value */
static void
own_value_changed (GtkAdjustment *adj, SpongyScrollView *self)
{
  if (self->setting_adj || self->adj)
    return;
  double value = gtk_adjustment_get_value (adj);
  if (fabs (value - self->list.offset) < 0.001)
    return;     /* configured with the list's own offset */
  spongy_list_scroll_to (&self->list, (float) value);
  /* while allocating (from a handler of the adjustment's "changed"), the child is placed right after */
  if (!self->allocating)
    sync_offset (self);
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
  if (horizontal (self)) {
    /* the child at its minimum width (or the view's, when wider), scrolled along it, as a GtkScrolledWindow lays
     * out a child it scrolls sideways: a column of rows as wide as it asks, its labels ellipsized */
    int child_min, child_nat;
    gtk_widget_measure (self->child, GTK_ORIENTATION_HORIZONTAL, -1, &child_min, &child_nat, NULL, NULL);
    if (orientation == GTK_ORIENTATION_HORIZONTAL)
      *nat = child_nat;
    else
      gtk_widget_measure (self->child, orientation, MAX (child_min, for_size), min, nat, NULL, NULL);
  } else if (orientation == GTK_ORIENTATION_HORIZONTAL) {
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
  if (self->adj) {
    gtk_widget_allocate (self->child, width, height, -1, NULL);   /* configures the adjustment */
    return;
  }
  int min, nat, size = horizontal (self) ? width : height;
  gtk_widget_measure (self->child, self->orientation, horizontal (self) ? -1 : width, &min, &nat, NULL, NULL);
  int content = MAX (horizontal (self) ? min : nat, size);
  /* a pager keeps its page when its size changes (the screen turned) */
  float old_size = self->list.view_height;
  bool keep_page = self->snap_pages && self->snap_interval <= 0 && old_size > 0 && old_size != size &&
                   !self->list.down;
  float page = keep_page ? roundf (self->list.offset / old_size) : 0;
  spongy_list_set_size (&self->list, size, content);
  if (keep_page)
    spongy_list_scroll_to (&self->list, page * size);
  /* as GtkScrolledWindow configures its adjustment, before the child is placed: a handler of its "changed" may
   * move the list from here (to stay at the end of a chat) */
  self->allocating = true;
  gtk_adjustment_configure (self->own_adj, self->list.offset, 0, content, size * 0.1, size * 0.9, size);
  self->allocating = false;
  if (self->offset != self->list.offset) {
    self->offset = self->list.offset;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OFFSET]);
  }
  double scale = device_scale (self);
  float o = (float) (round (self->offset * scale) / scale);
  if (horizontal (self))
    gtk_widget_allocate (self->child, content, height, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (-o, 0)));
  else
    gtk_widget_allocate (self->child, width, content, -1, gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (0, -o)));
}

/* ---- drawing ---- */

/* ---- the stretch: the child's render nodes placed again where the stretch shader would sample them ----
 *
 * The child's node tree is rebuilt in this widget's coordinates with every leaf (a background, an icon, a line)
 * moved and scaled so its top and bottom edges (left and right, sideways) land exactly where the stretch puts them,
 * clips the same, and text only moved, by whole device pixels: a glyph scaled by a factor that changes every frame
 * is drawn again for each one (half a second a frame here), and a 3 % stretch of a line of text does not show.
 * Every node is still drawn once. */

typedef struct {
  float  amount, H;      /* H: the view's size along the axis */
  double scale;          /* device pixels per logical one */
  bool   horizontal;
} Stretch;

typedef struct {
  float sx, sy, dx, dy;  /* node coordinates to this widget's: x' = sx x + dx */
} Affine;

/* where the stretch shows what is at p (along the axis) unstretched */
static float
stretched (const Stretch *s, float p)
{
  return spongy_stretch_unmap (CLAMP (p, -s->H, 2 * s->H), s->amount, s->H);
}

/* a rectangle in node coordinates: where it spans along the axis, in this widget's */
static void
span (const Stretch *s, const graphene_rect_t *r, const Affine *t, float *p0, float *p1)
{
  if (s->horizontal) {
    *p0 = t->sx * r->origin.x + t->dx;
    *p1 = *p0 + t->sx * r->size.width;
  } else {
    *p0 = t->sy * r->origin.y + t->dy;
    *p1 = *p0 + t->sy * r->size.height;
  }
}

/* the node placed with t, then scaled by k along the axis and moved by d */
static GskRenderNode *
moved (const Stretch *s, GskRenderNode *node, const Affine *t, float k, float d)
{
  float sx = t->sx, sy = t->sy, dx = t->dx, dy = t->dy;
  if (s->horizontal) {
    dx = k * dx + d;
    sx = k * sx;
  } else {
    dy = k * dy + d;
    sy = k * sy;
  }
  if (sx == 1 && sy == 1 && dx == 0 && dy == 0)
    return gsk_render_node_ref (node);
  GskTransform *tr = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (dx, dy));
  tr = gsk_transform_scale (tr, sx, sy);
  GskRenderNode *out = gsk_transform_node_new (node, tr);
  gsk_transform_unref (tr);
  return out;
}

static GskRenderNode *
stretch_leaf (const Stretch *s, GskRenderNode *node, const Affine *t)
{
  graphene_rect_t b;
  float p0, p1;
  gsk_render_node_get_bounds (node, &b);
  span (s, &b, t, &p0, &p1);
  float margin = fabsf (s->amount) * s->H + 1;
  if (p1 < -margin || p0 > s->H + margin)
    return NULL;
  if (gsk_render_node_get_node_type (node) == GSK_TEXT_NODE || p1 <= p0) {
    float c = (p0 + p1) / 2;
    return moved (s, node, t, 1, (float) (round ((stretched (s, c) - c) * s->scale) / s->scale));
  }
  /* what of it shows: its edges outside the view would be stretched by the curve's extrapolation */
  float a = CLAMP (p0, 0, s->H), z = CLAMP (p1, 0, s->H);
  if (z <= a)
    return NULL;
  float u0 = stretched (s, a), u1 = stretched (s, z), k = (u1 - u0) / (z - a);
  return moved (s, node, t, k, u0 - a * k);
}

/* a rectangle in node coordinates, stretched, in this widget's */
static graphene_rect_t
stretch_rect (const Stretch *s, const graphene_rect_t *r, const Affine *t)
{
  float p0, p1;
  span (s, r, t, &p0, &p1);
  float u0 = stretched (s, p0), u1 = stretched (s, p1);
  if (s->horizontal)
    return GRAPHENE_RECT_INIT (u0, t->sy * r->origin.y + t->dy, u1 - u0, t->sy * r->size.height);
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
    float kx = t->sx, ky = t->sy;
    if (s->horizontal)
      kx = from->bounds.size.width > 0 ? clip.bounds.size.width / from->bounds.size.width : t->sx;
    else
      ky = from->bounds.size.height > 0 ? clip.bounds.size.height / from->bounds.size.height : t->sy;
    for (int i = 0; i < 4; i++) {
      clip.corner[i].width *= kx;
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
  float W = gtk_widget_get_width (widget), H = gtk_widget_get_height (widget), size = horizontal (self) ? W : H;
  if (!self->child)
    return;

  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, W, H));
  if (self->stretch == 0 || size <= 0) {
    gtk_widget_snapshot_child (widget, self->child, snapshot);
    gtk_snapshot_pop (snapshot);
    return;
  }
  GtkSnapshot *sub = gtk_snapshot_new ();
  gtk_widget_snapshot_child (widget, self->child, sub);
  GskRenderNode *node = gtk_snapshot_free_to_node (sub);
  if (node) {
    Stretch s = { self->stretch, size, device_scale (self), horizontal (self) };
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
on_scale (GdkSurface *surface, GParamSpec *pspec, SpongyScrollView *self)
{
  update_density (self);
}

static void
realize (GtkWidget *widget)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  GTK_WIDGET_CLASS (spongy_scroll_view_parent_class)->realize (widget);
  update_density (self);
  /* a new screen scale (another display size) can change it */
  GtkNative *native = gtk_widget_get_native (widget);
  self->surface = native ? gtk_native_get_surface (native) : NULL;
  if (self->surface) {
    g_object_ref (self->surface);
    g_signal_connect (self->surface, "notify::scale", G_CALLBACK (on_scale), self);
  }
}

static void
unrealize (GtkWidget *widget)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (widget);
  if (self->surface) {
    g_signal_handlers_disconnect_by_func (self->surface, on_scale, self);
    g_clear_object (&self->surface);
  }
  GTK_WIDGET_CLASS (spongy_scroll_view_parent_class)->unrealize (widget);
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
  /* nothing is left half stretched or flinging for the next time it shows, nor between two snap points */
  spongy_list_cancel (&self->list, now_ms ());
  spongy_list_scroll_to (&self->list, self->list.offset);
  spongy_edge_finish (&self->list.top);
  spongy_edge_finish (&self->list.bottom);
  if (snap_interval (self) > 0)
    spongy_list_scroll_to (&self->list, snap (self, now_ms ()));
  self->dragged = false;
  self->stretch = 0;
  GTK_WIDGET_CLASS (spongy_scroll_view_parent_class)->unmap (widget);
}

static void
set_adjustment (SpongyScrollView *self, GtkAdjustment *adj)
{
  if (self->adj) {
    g_signal_handlers_disconnect_by_data (self->adj, self);
    g_clear_object (&self->adj);
  }
  if (adj) {
    self->adj = g_object_ref_sink (adj);
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
  self->setting_adj = true;
  gtk_adjustment_set_value (self->own_adj, 0);
  self->setting_adj = false;
  if (child) {
    gtk_widget_set_parent (child, GTK_WIDGET (self));
    if (GTK_IS_SCROLLABLE (child)) {
      set_adjustment (self, gtk_adjustment_new (0, 0, 0, 0, 0, 0));
      g_object_set (child, horizontal (self) ? "hadjustment" : "vadjustment", self->adj,
                    horizontal (self) ? "vadjustment" : "hadjustment", gtk_adjustment_new (0, 0, 0, 0, 0, 0), NULL);
    }
  }
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CHILD]);
}

static void
set_orientation (SpongyScrollView *self, GtkOrientation orientation)
{
  if (self->orientation == orientation)
    return;
  self->orientation = orientation;
  gtk_event_controller_scroll_set_flags (GTK_EVENT_CONTROLLER_SCROLL (self->scroll),
                                         (horizontal (self) ? GTK_EVENT_CONTROLLER_SCROLL_HORIZONTAL
                                                            : GTK_EVENT_CONTROLLER_SCROLL_VERTICAL) |
                                         GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
  /* the child again, scrolled from the start along the other axis */
  GtkWidget *child = self->child;
  if (child) {
    g_object_ref (child);
    spongy_scroll_view_set_child (self, NULL);
    spongy_scroll_view_set_child (self, child);
    g_object_unref (child);
  }
  gtk_widget_queue_resize (GTK_WIDGET (self));
  g_object_notify (G_OBJECT (self), "orientation");
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
 * spongy_scroll_view_get_vadjustment:
 *
 * Returns: (transfer none) (nullable): the vertical adjustment, as GtkScrolledWindow's; NULL when horizontal
 */
GtkAdjustment *
spongy_scroll_view_get_vadjustment (SpongyScrollView *self)
{
  g_return_val_if_fail (SPONGY_IS_SCROLL_VIEW (self), NULL);
  if (horizontal (self))
    return NULL;
  return self->adj ? self->adj : self->own_adj;
}

/**
 * spongy_scroll_view_get_hadjustment:
 *
 * Returns: (transfer none) (nullable): the horizontal adjustment of a horizontal view; NULL when vertical
 */
GtkAdjustment *
spongy_scroll_view_get_hadjustment (SpongyScrollView *self)
{
  g_return_val_if_fail (SPONGY_IS_SCROLL_VIEW (self), NULL);
  if (!horizontal (self))
    return NULL;
  return self->adj ? self->adj : self->own_adj;
}

void
spongy_scroll_view_set_snap (SpongyScrollView *self, double interval, gboolean pages)
{
  g_return_if_fail (SPONGY_IS_SCROLL_VIEW (self));
  self->snap_interval = MAX (interval, 0);
  self->snap_pages = pages;
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
  case PROP_ORIENTATION: g_value_set_enum (value, self->orientation); break;
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
  case PROP_ORIENTATION: set_orientation (self, g_value_get_enum (value)); break;
  default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
  }
}

static void
dispose (GObject *object)
{
  SpongyScrollView *self = SPONGY_SCROLL_VIEW (object);
  press_forget (self);
  spongy_scroll_view_set_child (self, NULL);
  if (self->own_adj)
    g_signal_handlers_disconnect_by_data (self->own_adj, self);
  g_clear_object (&self->own_adj);
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
  widget_class->unrealize = unrealize;
  widget_class->unmap = unmap;

  props[PROP_CHILD] = g_param_spec_object ("child", NULL, NULL, GTK_TYPE_WIDGET,
                                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_OFFSET] = g_param_spec_double ("offset", NULL, NULL, 0, G_MAXDOUBLE, 0,
                                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, props);
  g_object_class_override_property (object_class, PROP_ORIENTATION, "orientation");

  /**
   * SpongyScrollView::edge-overshot:
   * @self: the view
   * @pos: the edge
   *
   * As GtkScrolledWindow's: a touch pulled the edge into a stretch (once per touch), or a wheel scrolled past it.
   * GTK_POS_LEFT and GTK_POS_RIGHT for a horizontal view.
   */
  signals[EDGE_OVERSHOT] = g_signal_new ("edge-overshot", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 1, GTK_TYPE_POSITION_TYPE);
  gtk_widget_class_set_css_name (widget_class, "spongyscrollview");
}

static void
spongy_scroll_view_init (SpongyScrollView *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  self->orientation = GTK_ORIENTATION_VERTICAL;
  spongy_list_init (&self->list, 0, 0);
  self->own_adj = g_object_ref_sink (gtk_adjustment_new (0, 0, 0, 0, 0, 0));
  g_signal_connect (self->own_adj, "value-changed", G_CALLBACK (own_value_changed), self);
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

  self->scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                                 GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
  g_signal_connect (self->scroll, "scroll", G_CALLBACK (scrolled), self);
  g_signal_connect (self->scroll, "decelerate", G_CALLBACK (decelerate), self);
  gtk_widget_add_controller (widget, self->scroll);
}
