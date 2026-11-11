/* The Wayland side (see wl.h). */
#include "wl.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

#define POINTER_ID 1000

struct Wl {
  const WlCallbacks *cb;
  gpointer           data;

  struct wl_display  *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  guint32            compositor_version;
  struct xdg_wm_base *wm_base;
  struct wl_seat     *seat;
  struct wl_touch    *touch;
  struct wl_pointer  *pointer;
  struct wl_keyboard *keyboard;
  struct wp_viewporter *viewporter;
  struct wp_fractional_scale_manager_v1 *frac_mgr;
  struct wp_presentation *presentation;
  int                output_scale;             /* the outputs' integer scale: a guess until the surface is shown */
  bool               frac_known;

  struct wl_surface  *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *frac;

  int                logical_w, logical_h;     /* from the last configure */
  int                pending_w, pending_h;
  bool               pending_suspended, suspended;
  double             scale;                    /* fractional (n/120) or the integer buffer scale */
  int                buffer_scale;
  int                width, height;            /* pixels */
  bool               configured;

  /* the frame clock */
  struct wl_callback *frame_cb;
  guint64            frames;                   /* committed so far */
  guint64            cb_frame;                 /* the one frame_cb is for */
  bool               wanted;
  guint              idle_id;
  gint64             last_presented;           /* ns, CLOCK_MONOTONIC */
  gint64             refresh;                  /* ns */

  /* the pointer, as a touch */
  double             px, py;
  bool               pressed;

  GSource           *source;
};

static gint64
now_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (gint64) ts.tv_sec * G_GINT64_CONSTANT (1000000000) + ts.tv_nsec;
}

/* When a frame drawn now reaches the screen: the next vsync on the grid the compositor reported */
static gint64
next_vsync (Wl *wl)
{
  gint64 now = now_ns ();
  if (!wl->last_presented || wl->refresh <= 0)
    return now + wl->refresh;
  gint64 k = (now - wl->last_presented + wl->refresh - 1) / wl->refresh;
  if (k < 1)
    k = 1;
  return wl->last_presented + k * wl->refresh;
}

static void draw (Wl *wl);

/* ---- size and scale ------------------------------------------------------------------------------------------- */

static void
apply_size (Wl *wl)
{
  int w, h;
  if (wl->frac && !wl->frac_known && wl->output_scale > 1)
    wl->scale = wl->output_scale;   /* the fractional scale comes once the surface is on an output */
  if (wl->frac) {
    w = (int) lround (wl->logical_w * wl->scale);
    h = (int) lround (wl->logical_h * wl->scale);
    wp_viewport_set_destination (wl->viewport, wl->logical_w, wl->logical_h);
  } else {
    wl_surface_set_buffer_scale (wl->surface, wl->buffer_scale);
    w = wl->logical_w * wl->buffer_scale;
    h = wl->logical_h * wl->buffer_scale;
  }
  if (w == wl->width && h == wl->height)
    return;
  wl->width = w;
  wl->height = h;
  wl->cb->resize (wl->data, w, h, wl->scale);
  wl_schedule (wl);
}

static void
frac_preferred (void *data, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
  Wl *wl = data;
  wl->frac_known = true;
  if (wl->scale == scale / 120.0)
    return;
  wl->scale = scale / 120.0;
  if (wl->configured) {
    wl->width = 0;             /* the scale alone changed: tell the app all the same */
    apply_size (wl);
  }
}

static const struct wp_fractional_scale_v1_listener frac_listener = { frac_preferred };

static void surface_enter (void *data, struct wl_surface *s, struct wl_output *o) {}
static void surface_leave (void *data, struct wl_surface *s, struct wl_output *o) {}

static void
surface_preferred_scale (void *data, struct wl_surface *s, int32_t factor)
{
  Wl *wl = data;
  if (wl->frac || wl->buffer_scale == factor)
    return;
  wl->buffer_scale = factor;
  wl->scale = factor;
  if (wl->configured) {
    wl->width = 0;
    apply_size (wl);
  }
}

static void surface_preferred_transform (void *data, struct wl_surface *s, uint32_t t) {}

static const struct wl_surface_listener surface_listener = {
  surface_enter, surface_leave, surface_preferred_scale, surface_preferred_transform,
};

/* ---- xdg ------------------------------------------------------------------------------------------------------ */

static void
wm_ping (void *data, struct xdg_wm_base *b, uint32_t serial)
{
  xdg_wm_base_pong (b, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
toplevel_configure (void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
  Wl *wl = data;
  wl->pending_w = w;
  wl->pending_h = h;
  wl->pending_suspended = false;
  uint32_t *state;
  wl_array_for_each (state, states) {
    if (*state == XDG_TOPLEVEL_STATE_SUSPENDED)
      wl->pending_suspended = true;
  }
}

static void
toplevel_close (void *data, struct xdg_toplevel *t)
{
  Wl *wl = data;
  if (wl->cb->close)
    wl->cb->close (wl->data);
}

static void toplevel_bounds (void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void toplevel_caps (void *data, struct xdg_toplevel *t, struct wl_array *caps) {}

static const struct xdg_toplevel_listener toplevel_listener = {
  toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps,
};

static void
xdg_configure (void *data, struct xdg_surface *s, uint32_t serial)
{
  Wl *wl = data;
  xdg_surface_ack_configure (s, serial);
  if (wl->pending_w > 0 && wl->pending_h > 0) {
    wl->logical_w = wl->pending_w;
    wl->logical_h = wl->pending_h;
  } else if (!wl->logical_w) {
    wl->logical_w = 360;       /* the compositor leaves it to us; a phone's width */
    wl->logical_h = 760;
  }
  wl->configured = true;
  apply_size (wl);
  if (wl->pending_suspended != wl->suspended) {
    wl->suspended = wl->pending_suspended;
    if (wl->cb->suspended)
      wl->cb->suspended (wl->data, wl->suspended);
  }
}

static const struct xdg_surface_listener xdg_surface_listener = { xdg_configure };

/* ---- the frame clock ------------------------------------------------------------------------------------------ */

static void
feedback_sync_output (void *data, struct wp_presentation_feedback *f, struct wl_output *o) {}

static void
feedback_presented (void *data, struct wp_presentation_feedback *f, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                    uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
  Wl *wl = data;
  wl->last_presented = (gint64) (((guint64) sec_hi << 32) | sec_lo) * G_GINT64_CONSTANT (1000000000) + nsec;
  if (refresh)
    wl->refresh = refresh;
  wp_presentation_feedback_destroy (f);
}

static void
feedback_discarded (void *data, struct wp_presentation_feedback *f)
{
  wp_presentation_feedback_destroy (f);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
  feedback_sync_output, feedback_presented, feedback_discarded,
};

static void
frame_done (void *data, struct wl_callback *cb, uint32_t time)
{
  Wl *wl = data;
  wl_callback_destroy (cb);
  wl->frame_cb = NULL;
  if (wl->cb->shown)
    wl->cb->shown (wl->data, wl->cb_frame);
  if (wl->wanted)
    draw (wl);
}

static const struct wl_callback_listener frame_listener = { frame_done };

void
wl_frame_requested (Wl *wl)
{
  wl->frames++;
  if (!wl->frame_cb) {
    wl->frame_cb = wl_surface_frame (wl->surface);
    wl_callback_add_listener (wl->frame_cb, &frame_listener, wl);
    wl->cb_frame = wl->frames;
  }
  if (wl->presentation) {
    struct wp_presentation_feedback *f = wp_presentation_feedback (wl->presentation, wl->surface);
    wp_presentation_feedback_add_listener (f, &feedback_listener, wl);
  }
}

static void
draw (Wl *wl)
{
  wl->wanted = false;
  if (wl->width > 0 && wl->height > 0)
    wl->cb->frame (wl->data, next_vsync (wl));
}

static gboolean
draw_idle (gpointer data)
{
  Wl *wl = data;
  wl->idle_id = 0;
  if (wl->wanted && !wl->frame_cb)
    draw (wl);
  return G_SOURCE_REMOVE;
}

void
wl_schedule (Wl *wl)
{
  wl->wanted = true;
  /* a frame is on screen and its callback pending: that callback draws the next; otherwise the clock has stopped,
   * so start it now */
  if (!wl->frame_cb && !wl->idle_id && wl->configured)
    wl->idle_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE, draw_idle, wl, NULL);
}

guint64
wl_frames (Wl *wl)
{
  return wl->frames;
}

gint64
wl_refresh_ns (Wl *wl)
{
  return wl->refresh;
}

/* ---- input ---------------------------------------------------------------------------------------------------- */

static void
touch_down (void *data, struct wl_touch *t, uint32_t serial, uint32_t time, struct wl_surface *s, int32_t id,
            wl_fixed_t x, wl_fixed_t y)
{
  Wl *wl = data;
  wl->cb->touch (wl->data, WL_TOUCH_DOWN, id, wl_fixed_to_double (x) * wl->scale, wl_fixed_to_double (y) * wl->scale,
                 time);
}

static void
touch_up (void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id)
{
  Wl *wl = data;
  wl->cb->touch (wl->data, WL_TOUCH_UP, id, 0, 0, time);
}

static void
touch_motion (void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
  Wl *wl = data;
  wl->cb->touch (wl->data, WL_TOUCH_MOTION, id, wl_fixed_to_double (x) * wl->scale,
                 wl_fixed_to_double (y) * wl->scale, time);
}

static void touch_frame (void *data, struct wl_touch *t) {}

static void
touch_cancel (void *data, struct wl_touch *t)
{
  Wl *wl = data;
  wl->cb->touch (wl->data, WL_TOUCH_CANCEL, -1, 0, 0, 0);
}

static void touch_shape (void *data, struct wl_touch *t, int32_t id, wl_fixed_t major, wl_fixed_t minor) {}
static void touch_orientation (void *data, struct wl_touch *t, int32_t id, wl_fixed_t o) {}

static const struct wl_touch_listener touch_listener = {
  touch_down, touch_up, touch_motion, touch_frame, touch_cancel, touch_shape, touch_orientation,
};

static void
pointer_enter (void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
  Wl *wl = data;
  wl->px = wl_fixed_to_double (x) * wl->scale;
  wl->py = wl_fixed_to_double (y) * wl->scale;
  wl_pointer_set_cursor (p, serial, NULL, 0, 0);
}

static void
pointer_leave (void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
  Wl *wl = data;
  if (wl->pressed) {
    wl->pressed = false;
    wl->cb->touch (wl->data, WL_TOUCH_CANCEL, -1, 0, 0, 0);
  }
}

static void
pointer_motion (void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
  Wl *wl = data;
  wl->px = wl_fixed_to_double (x) * wl->scale;
  wl->py = wl_fixed_to_double (y) * wl->scale;
  if (wl->pressed)
    wl->cb->touch (wl->data, WL_TOUCH_MOTION, POINTER_ID, wl->px, wl->py, time);
}

static void
pointer_button (void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
  Wl *wl = data;
  if (button != BTN_LEFT)
    return;
  wl->pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
  wl->cb->touch (wl->data, wl->pressed ? WL_TOUCH_DOWN : WL_TOUCH_UP, POINTER_ID, wl->px, wl->py, time);
}

static void pointer_axis (void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {}
static void pointer_frame (void *data, struct wl_pointer *p) {}
static void pointer_axis_source (void *data, struct wl_pointer *p, uint32_t s) {}
static void pointer_axis_stop (void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete (void *data, struct wl_pointer *p, uint32_t axis, int32_t d) {}
static void pointer_axis_value120 (void *data, struct wl_pointer *p, uint32_t axis, int32_t v) {}
static void pointer_axis_relative_direction (void *data, struct wl_pointer *p, uint32_t axis, uint32_t d) {}

static const struct wl_pointer_listener pointer_listener = {
  pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis, pointer_frame, pointer_axis_source,
  pointer_axis_stop, pointer_axis_discrete, pointer_axis_value120, pointer_axis_relative_direction,
};

static void
keyboard_keymap (void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
  close (fd);
}

static void keyboard_enter (void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                            struct wl_array *keys) {}
static void keyboard_leave (void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {}

static void
keyboard_key (void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  Wl *wl = data;
  if (wl->cb->key)
    wl->cb->key (wl->data, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers (void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
                                uint32_t lock, uint32_t group) {}
static void keyboard_repeat (void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
  keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat,
};

static void
seat_caps (void *data, struct wl_seat *seat, uint32_t caps)
{
  Wl *wl = data;
  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !wl->touch) {
    wl->touch = wl_seat_get_touch (seat);
    wl_touch_add_listener (wl->touch, &touch_listener, wl);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && wl->touch) {
    wl_touch_release (wl->touch);
    wl->touch = NULL;
  }
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
    wl->pointer = wl_seat_get_pointer (seat);
    wl_pointer_add_listener (wl->pointer, &pointer_listener, wl);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->pointer) {
    wl_pointer_release (wl->pointer);
    wl->pointer = NULL;
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
    wl->keyboard = wl_seat_get_keyboard (seat);
    wl_keyboard_add_listener (wl->keyboard, &keyboard_listener, wl);
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->keyboard) {
    wl_keyboard_release (wl->keyboard);
    wl->keyboard = NULL;
  }
}

static void seat_name (void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void output_geometry (void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                             int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void output_mode (void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void output_done (void *data, struct wl_output *o) {}

static void
output_scale (void *data, struct wl_output *o, int32_t factor)
{
  Wl *wl = data;
  wl->output_scale = MAX (wl->output_scale, factor);
}

static void output_name (void *data, struct wl_output *o, const char *name) {}
static void output_description (void *data, struct wl_output *o, const char *d) {}

static const struct wl_output_listener output_listener = {
  output_geometry, output_mode, output_done, output_scale, output_name, output_description,
};

/* ---- the registry --------------------------------------------------------------------------------------------- */

static void
registry_global (void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
  Wl *wl = data;
  if (!strcmp (iface, wl_compositor_interface.name)) {
    wl->compositor_version = MIN (version, 6);
    wl->compositor = wl_registry_bind (r, name, &wl_compositor_interface, wl->compositor_version);
  } else if (!strcmp (iface, xdg_wm_base_interface.name)) {
    wl->wm_base = wl_registry_bind (r, name, &xdg_wm_base_interface, MIN (version, 6));   /* 6: suspended */
    xdg_wm_base_add_listener (wl->wm_base, &wm_listener, wl);
  } else if (!strcmp (iface, wl_seat_interface.name) && !wl->seat) {
    wl->seat = wl_registry_bind (r, name, &wl_seat_interface, MIN (version, 8));
    wl_seat_add_listener (wl->seat, &seat_listener, wl);
  } else if (!strcmp (iface, wp_viewporter_interface.name)) {
    wl->viewporter = wl_registry_bind (r, name, &wp_viewporter_interface, 1);
  } else if (!strcmp (iface, wp_fractional_scale_manager_v1_interface.name)) {
    wl->frac_mgr = wl_registry_bind (r, name, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (!strcmp (iface, wl_output_interface.name) && version >= 2) {
    struct wl_output *o = wl_registry_bind (r, name, &wl_output_interface, MIN (version, 4));
    wl_output_add_listener (o, &output_listener, wl);
  } else if (!strcmp (iface, wp_presentation_interface.name)) {
    wl->presentation = wl_registry_bind (r, name, &wp_presentation_interface, 1);
  }
}

static void registry_remove (void *data, struct wl_registry *r, uint32_t name) {}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

/* ---- the GLib source ------------------------------------------------------------------------------------------ */

typedef struct {
  GSource  source;
  Wl      *wl;
  gpointer tag;
  bool     reading;
} WlSource;

static gboolean
source_prepare (GSource *s, gint *timeout)
{
  WlSource *ws = (WlSource *) s;
  struct wl_display *d = ws->wl->display;
  *timeout = -1;
  if (ws->reading)
    return FALSE;
  if (wl_display_prepare_read (d) != 0)
    return TRUE;             /* events are queued: dispatch them first */
  ws->reading = true;
  wl_display_flush (d);
  return FALSE;
}

static gboolean
source_check (GSource *s)
{
  WlSource *ws = (WlSource *) s;
  struct wl_display *d = ws->wl->display;
  if (!ws->reading)
    return TRUE;
  ws->reading = false;
  GIOCondition rev = g_source_query_unix_fd (s, ws->tag);
  if (rev & G_IO_IN) {
    if (wl_display_read_events (d) < 0)
      g_error ("wayland: the connection is lost");
    return TRUE;
  }
  wl_display_cancel_read (d);
  if (rev & (G_IO_ERR | G_IO_HUP))
    g_error ("wayland: the compositor hung up");
  return FALSE;
}

static gboolean
source_dispatch (GSource *s, GSourceFunc cb, gpointer data)
{
  WlSource *ws = (WlSource *) s;
  if (wl_display_dispatch_pending (ws->wl->display) < 0)
    g_error ("wayland: dispatch failed");
  wl_display_flush (ws->wl->display);
  return G_SOURCE_CONTINUE;
}

static void
source_finalize (GSource *s)
{
  WlSource *ws = (WlSource *) s;
  if (ws->reading)
    wl_display_cancel_read (ws->wl->display);
}

static GSourceFuncs source_funcs = { source_prepare, source_check, source_dispatch, source_finalize };

/* ---- open/close ----------------------------------------------------------------------------------------------- */

Wl *
wl_open (const char *app_id, const WlCallbacks *cb, gpointer data)
{
  Wl *wl = g_new0 (Wl, 1);
  wl->cb = cb;
  wl->data = data;
  wl->scale = 1;
  wl->buffer_scale = 1;
  wl->refresh = 16666667;
  wl->display = wl_display_connect (NULL);
  if (!wl->display) {
    g_warning ("wayland: no compositor");
    g_free (wl);
    return NULL;
  }
  wl->registry = wl_display_get_registry (wl->display);
  wl_registry_add_listener (wl->registry, &registry_listener, wl);
  wl_display_roundtrip (wl->display);
  if (!wl->compositor || !wl->wm_base) {
    g_warning ("wayland: no xdg-shell");
    return NULL;
  }
  wl_display_roundtrip (wl->display);        /* the seat's capabilities, the outputs' scale */

  wl->surface = wl_compositor_create_surface (wl->compositor);
  wl_surface_add_listener (wl->surface, &surface_listener, wl);
  if (wl->frac_mgr && wl->viewporter) {
    wl->viewport = wp_viewporter_get_viewport (wl->viewporter, wl->surface);
    wl->frac = wp_fractional_scale_manager_v1_get_fractional_scale (wl->frac_mgr, wl->surface);
    wp_fractional_scale_v1_add_listener (wl->frac, &frac_listener, wl);
  }
  wl->xdg_surface = xdg_wm_base_get_xdg_surface (wl->wm_base, wl->surface);
  xdg_surface_add_listener (wl->xdg_surface, &xdg_surface_listener, wl);
  wl->toplevel = xdg_surface_get_toplevel (wl->xdg_surface);
  xdg_toplevel_add_listener (wl->toplevel, &toplevel_listener, wl);
  xdg_toplevel_set_app_id (wl->toplevel, app_id);
  xdg_toplevel_set_title (wl->toplevel, "Home");
  if (!g_getenv ("NEO_WINDOWED"))
    xdg_toplevel_set_fullscreen (wl->toplevel, NULL);
  wl_surface_commit (wl->surface);
  wl_display_roundtrip (wl->display);        /* the first configure, and the scale with it */

  WlSource *ws = (WlSource *) g_source_new (&source_funcs, sizeof (WlSource));
  ws->wl = wl;
  ws->tag = g_source_add_unix_fd (&ws->source, wl_display_get_fd (wl->display), G_IO_IN | G_IO_ERR | G_IO_HUP);
  g_source_set_priority (&ws->source, G_PRIORITY_HIGH);
  g_source_set_name (&ws->source, "wayland");
  g_source_attach (&ws->source, NULL);
  wl->source = &ws->source;
  return wl;
}

struct wl_display *
wl_get_display (Wl *wl)
{
  return wl->display;
}

struct wl_surface *
wl_get_surface (Wl *wl)
{
  return wl->surface;
}

void
wl_get_size (Wl *wl, int *width, int *height, double *scale)
{
  *width = wl->width;
  *height = wl->height;
  *scale = wl->scale;
}

void
wl_close (Wl *wl)
{
  if (wl->idle_id)
    g_source_remove (wl->idle_id);
  if (wl->source) {
    g_source_destroy (wl->source);
    g_source_unref (wl->source);
  }
  if (wl->frame_cb)
    wl_callback_destroy (wl->frame_cb);
  if (wl->frac)
    wp_fractional_scale_v1_destroy (wl->frac);
  if (wl->viewport)
    wp_viewport_destroy (wl->viewport);
  xdg_toplevel_destroy (wl->toplevel);
  xdg_surface_destroy (wl->xdg_surface);
  wl_surface_destroy (wl->surface);
  wl_display_disconnect (wl->display);
  g_free (wl);
}
