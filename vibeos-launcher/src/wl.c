/* The Wayland side (see wl.h). */
#include "wl.h"

#include <glib/gi18n.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

#define POINTER_ID 1000
/* the self-heal's wait after a configure before it acts: the shell fits the window first (home.js _fitWindow) */
#define HEAL_DELAY_MS 500

/* An output as the compositor last described it (its done): its current mode and transform. The launcher is not drawn
 * turned (the compositor turns it), but its size has to be the output's turned one. */
typedef struct {
  struct Wl        *wl;
  struct wl_output *output;
  guint32           name;                      /* the registry's */
  int32_t           mode_w, mode_h, transform;
  int32_t           next_w, next_h, next_transform;
  bool              done;
} WlOutput;

/* the self-heal (see heal_check) */
typedef enum {
  HEAL_IDLE,
  HEAL_ASKED,                /* asked for fullscreen again */
  HEAL_GAVE_UP,              /* asked once and still wrong: not again until the output or the size changes */
} HealState;

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
  struct zwp_text_input_manager_v3 *ti_manager;
  guint32            ti_version;
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
  bool               pending_fullscreen, fullscreen;
  bool               windowed;                 /* VIBEOS_WINDOWED: not fullscreen, nothing to heal */
  GPtrArray         *outputs;                  /* WlOutput */
  struct wl_output  *entered;                  /* the output the surface last entered */
  HealState          heal;
  bool               heal_unset;               /* asked unset + set: the next configure, out of fullscreen, is unused */
  guint              heal_id;                  /* the check HEAL_DELAY_MS after a configure or an output change */
  double             scale;                    /* fractional (n/120) or the integer buffer scale */
  int                buffer_scale;
  int                transform;                /* the output's, as the compositor would like buffers (wl_output) */
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

  /* keys: the compositor's keymap and its state (the modifiers) */
  struct xkb_context *xkb;
  struct xkb_keymap  *keymap;
  struct xkb_state   *xkb_state;

  /* the text input: wanted on or off by the app, on with the compositor (it needs the keyboard focus) */
  struct zwp_text_input_v3 *ti;
  bool               ti_focus, ti_want, ti_on;
  char              *ti_text;
  int                ti_cursor;
  int                ti_rect[4];               /* surface coordinates */
  /* what the next done applies */
  char              *ti_commit;
  guint32            ti_before, ti_after;
  bool               ti_submit;

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

static void
surface_enter (void *data, struct wl_surface *s, struct wl_output *o)
{
  Wl *wl = data;
  wl->entered = o;
}

static void
surface_leave (void *data, struct wl_surface *s, struct wl_output *o)
{
  Wl *wl = data;
  if (wl->entered == o)
    wl->entered = NULL;
}

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

/* The window is not drawn turned (the compositor turns it with the screen), but the parallax needs to know: its
 * accelerometer is on the panel's axes, the picture on the screen's (wl_surface 6) */
static void
surface_preferred_transform (void *data, struct wl_surface *s, uint32_t t)
{
  Wl *wl = data;
  if (wl->transform == (int) t)
    return;
  g_message ("wl: preferred transform %d -> %u", wl->transform, t);
  wl->transform = t;
  wl_schedule (wl);
}

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
  wl->pending_fullscreen = false;
  uint32_t *state;
  wl_array_for_each (state, states) {
    if (*state == XDG_TOPLEVEL_STATE_SUSPENDED)
      wl->pending_suspended = true;
    else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
      wl->pending_fullscreen = true;
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

/* The output the window is on, as it lies now (its mode turned by its transform: 90 and 270, flipped or not, swap the
 * sides); false while that is not known */
static bool
output_size (Wl *wl, int *w, int *h, int *transform)
{
  WlOutput *found = NULL;
  for (guint i = 0; i < wl->outputs->len; i++) {
    WlOutput *o = g_ptr_array_index (wl->outputs, i);
    if (o->output == wl->entered)
      found = o;
  }
  if (!found && wl->outputs->len == 1)
    found = g_ptr_array_index (wl->outputs, 0);
  if (!found || !found->done || found->mode_w <= 0 || found->mode_h <= 0)
    return false;
  bool turned = found->transform & 1;
  *w = turned ? found->mode_h : found->mode_w;
  *h = turned ? found->mode_w : found->mode_h;
  *transform = found->transform;
  return true;
}

/* 1 landscape, -1 portrait, 0 square or unknown */
static int
shape (int w, int h)
{
  return w > h ? 1 : h > w ? -1 : 0;
}

static const char *
shape_name (int w, int h)
{
  int s = shape (w, h);
  return s > 0 ? "landscape" : s < 0 ? "portrait" : "square";
}

static void
use_size (Wl *wl, int w, int h)
{
  if (w > 0 && h > 0) {
    wl->logical_w = w;
    wl->logical_h = h;
  } else if (!wl->logical_w) {
    wl->logical_w = 360;       /* the compositor leaves it to us; a phone's width */
    wl->logical_h = 760;
  }
  apply_size (wl);
}

/* the window's size the wrong way round for its output (portrait on a landscape one, or the other way round): fills
 * in the output's size and transform */
static bool
mismatched (Wl *wl, int *ow, int *oh, int *ot)
{
  if (wl->windowed || !wl->fullscreen || !output_size (wl, ow, oh, ot))
    return false;
  int want = shape (wl->logical_w, wl->logical_h), have = shape (*ow, *oh);
  return want && have && want != have;
}

/* The self-heal, a fallback: a fullscreen window's size is its output's, turned as the output is. The shell fits the
 * window when it is seen again and after every change of the screen (home.js _fitWindow: mutter tells a window seen
 * again the size it last sent it, which can be from before a turn and back while it was covered; the Xiaomi Pad 5's
 * home was drawn portrait on its landscape panel, 2026-11-25). If the size is still the wrong way round
 * HEAL_DELAY_MS after a configure (or after a turn of the output that left it so), the launcher asks to be fullscreen
 * again; the size is used as it comes meanwhile. Asking only set_fullscreen changes nothing in mutter (the window is
 * fullscreen already: no new configure), so it asks unset and set in one go: mutter answers with the window's size out
 * of fullscreen, which is not used, then its fullscreen size, the output's own. Once per mismatch: if the size is
 * still wrong after that, nothing is asked again until a configure fits the output or the output changes. */
static gboolean
heal_check (gpointer data)
{
  Wl *wl = data;
  int ow = 0, oh = 0, ot = 0;
  wl->heal_id = 0;
  if (!mismatched (wl, &ow, &oh, &ot))
    return G_SOURCE_REMOVE;
  switch (wl->heal) {
  case HEAL_IDLE:
    g_message ("wl: %dx%d (%s) on a %s output (%dx%d, transform %d) still, %d ms on: asking for fullscreen again",
               wl->logical_w, wl->logical_h, shape_name (wl->logical_w, wl->logical_h), shape_name (ow, oh),
               ow, oh, ot, HEAL_DELAY_MS);
    xdg_toplevel_unset_fullscreen (wl->toplevel);
    xdg_toplevel_set_fullscreen (wl->toplevel, NULL);
    wl_display_flush (wl->display);
    wl->heal = HEAL_ASKED;
    wl->heal_unset = true;
    break;
  case HEAL_ASKED:
    g_message ("wl: still %dx%d (%s) after asking for fullscreen again: not asking again until the output or the size "
               "changes", wl->logical_w, wl->logical_h, shape_name (wl->logical_w, wl->logical_h));
    wl->heal = HEAL_GAVE_UP;
    break;
  case HEAL_GAVE_UP:
    break;
  }
  return G_SOURCE_REMOVE;
}

static void
xdg_configure (void *data, struct xdg_surface *s, uint32_t serial)
{
  Wl *wl = data;
  int ow = 0, oh = 0, ot = 0;
  xdg_surface_ack_configure (s, serial);
  char output[64] = "not known yet";
  if (output_size (wl, &ow, &oh, &ot))
    g_snprintf (output, sizeof output, "%dx%d (transform %d)", ow, oh, ot);
  g_message ("wl: configure %dx%d%s%s; output %s", wl->pending_w, wl->pending_h,
             wl->pending_fullscreen ? " fullscreen" : "", wl->pending_suspended ? " suspended" : "", output);
  /* the unset half of the self-heal's request: its fullscreen configure follows */
  bool skip = wl->heal_unset && !wl->pending_fullscreen;
  wl->heal_unset = false;
  wl->configured = true;
  if (!skip) {
    wl->fullscreen = wl->pending_fullscreen;
    use_size (wl, wl->pending_w, wl->pending_h);
    g_clear_handle_id (&wl->heal_id, g_source_remove);
    if (mismatched (wl, &ow, &oh, &ot)) {
      wl->heal_id = g_timeout_add (HEAL_DELAY_MS, heal_check, wl);
    } else if (wl->heal != HEAL_IDLE && wl->fullscreen) {
      g_message ("wl: the configured size fits the output again");
      wl->heal = HEAL_IDLE;
    }
  }
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
  Wl *wl = data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !wl->xkb) {
    close (fd);
    return;
  }
  char *map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close (fd);
  if (map == MAP_FAILED)
    return;
  struct xkb_keymap *keymap = xkb_keymap_new_from_string (wl->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap (map, size);
  if (!keymap)
    return;
  g_clear_pointer (&wl->xkb_state, xkb_state_unref);
  g_clear_pointer (&wl->keymap, xkb_keymap_unref);
  wl->keymap = keymap;
  wl->xkb_state = xkb_state_new (keymap);
}

static void keyboard_enter (void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                            struct wl_array *keys) {}
static void keyboard_leave (void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {}

static void
keyboard_key (void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  Wl *wl = data;
  bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
  guint32 sym = 0;
  char text[64] = "";
  if (wl->xkb_state) {
    sym = xkb_state_key_get_one_sym (wl->xkb_state, key + 8);   /* xkb's keycodes are evdev's + 8 */
    if (pressed)
      xkb_state_key_get_utf8 (wl->xkb_state, key + 8, text, sizeof text);
  }
  if (wl->cb->key)
    wl->cb->key (wl->data, key, sym, pressed ? text : NULL, pressed);
}

static void
keyboard_modifiers (void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock,
                    uint32_t group)
{
  Wl *wl = data;
  if (wl->xkb_state)
    xkb_state_update_mask (wl->xkb_state, dep, lat, lock, 0, 0, group);
}
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

static void
output_geometry (void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t subpixel,
                 const char *make, const char *model, int32_t transform)
{
  WlOutput *out = data;
  out->next_transform = transform;
}

static void
output_mode (void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
  WlOutput *out = data;
  if (!(flags & WL_OUTPUT_MODE_CURRENT))
    return;
  out->next_w = w;
  out->next_h = h;
}

/* a turned or changed output: the self-heal may ask again, and checks HEAL_DELAY_MS on if the window's size is the
 * wrong way round for it now (the configure the shell sends after a turn restarts the wait, so it goes first) */
static void
output_done (void *data, struct wl_output *o)
{
  WlOutput *out = data;
  Wl *wl = out->wl;
  bool changed = out->done && (out->mode_w != out->next_w || out->mode_h != out->next_h ||
                               out->transform != out->next_transform);
  out->mode_w = out->next_w;
  out->mode_h = out->next_h;
  out->transform = out->next_transform;
  out->done = true;
  if (!changed)
    return;
  g_message ("wl: an output now %dx%d (transform %d); the window %dx%d", out->mode_w, out->mode_h, out->transform,
             wl->logical_w, wl->logical_h);
  if (wl->heal == HEAL_GAVE_UP)
    wl->heal = HEAL_IDLE;
  int ow = 0, oh = 0, ot = 0;
  if (wl->configured && mismatched (wl, &ow, &oh, &ot)) {
    g_clear_handle_id (&wl->heal_id, g_source_remove);
    wl->heal_id = g_timeout_add (HEAL_DELAY_MS, heal_check, wl);
  }
}

static void
output_scale (void *data, struct wl_output *o, int32_t factor)
{
  WlOutput *out = data;
  out->wl->output_scale = MAX (out->wl->output_scale, factor);
}

static void output_name (void *data, struct wl_output *o, const char *name) {}
static void output_description (void *data, struct wl_output *o, const char *d) {}

static const struct wl_output_listener output_listener = {
  output_geometry, output_mode, output_done, output_scale, output_name, output_description,
};

/* ---- the text input ----------------------------------------------------------------------------------------- */

/* the field's state for the input method: what it holds, where the cursor and the field are */
static void
ti_state (Wl *wl)
{
  zwp_text_input_v3_set_surrounding_text (wl->ti, wl->ti_text ? wl->ti_text : "", wl->ti_cursor, wl->ti_cursor);
  zwp_text_input_v3_set_cursor_rectangle (wl->ti, wl->ti_rect[0], wl->ti_rect[1], wl->ti_rect[2], wl->ti_rect[3]);
}

/* on: enabled (which resets the input method's state, so the whole state follows), or the state alone while it is
 * on already; off: disabled. Only with the keyboard focus: the compositor ignores the requests without it. */
static void
ti_apply (Wl *wl)
{
  if (!wl->ti || !wl->ti_focus)
    return;
  if (wl->ti_want) {
    if (!wl->ti_on) {
      zwp_text_input_v3_enable (wl->ti);
      zwp_text_input_v3_set_content_type (wl->ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                          ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
#ifdef ZWP_TEXT_INPUT_V3_SET_AVAILABLE_ACTIONS_SINCE_VERSION
      if (wl->ti_version >= ZWP_TEXT_INPUT_V3_SET_AVAILABLE_ACTIONS_SINCE_VERSION) {
        struct wl_array actions;
        wl_array_init (&actions);
        uint32_t *a = wl_array_add (&actions, sizeof *a);
        *a = ZWP_TEXT_INPUT_V3_ACTION_SUBMIT;
        zwp_text_input_v3_set_available_actions (wl->ti, &actions);
        wl_array_release (&actions);
      }
#endif
    }
    ti_state (wl);
  } else if (wl->ti_on) {
    zwp_text_input_v3_disable (wl->ti);
  } else {
    return;
  }
  zwp_text_input_v3_commit (wl->ti);
  wl->ti_on = wl->ti_want;
}

void
wl_text_input (Wl *wl, bool on, const char *text, int cursor, float x, float y, float w, float h)
{
  g_free (wl->ti_text);
  wl->ti_text = g_strdup (text);
  wl->ti_cursor = cursor;
  double s = wl->scale > 0 ? wl->scale : 1;
  wl->ti_rect[0] = (int) floor (x / s);
  wl->ti_rect[1] = (int) floor (y / s);
  wl->ti_rect[2] = (int) ceil (w / s);
  wl->ti_rect[3] = (int) ceil (h / s);
  wl->ti_want = on;
  ti_apply (wl);
}

void
wl_text_input_show (Wl *wl)
{
  if (!wl->ti || !wl->ti_on)
    return;
#ifdef ZWP_TEXT_INPUT_V3_SHOW_INPUT_PANEL_SINCE_VERSION
  if (wl->ti_version >= ZWP_TEXT_INPUT_V3_SHOW_INPUT_PANEL_SINCE_VERSION) {
    zwp_text_input_v3_show_input_panel (wl->ti);
    zwp_text_input_v3_commit (wl->ti);
    return;
  }
#endif
  /* version 1: mutter shows the keyboard when a focused field is enabled again */
  zwp_text_input_v3_enable (wl->ti);
  ti_state (wl);
  zwp_text_input_v3_commit (wl->ti);
}

static void
ti_enter (void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s)
{
  Wl *wl = data;
  wl->ti_focus = true;
  wl->ti_on = false;
  ti_apply (wl);
}

static void
ti_leave (void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s)
{
  Wl *wl = data;
  wl->ti_focus = false;
  wl->ti_on = false;
}

static void ti_preedit (void *data, struct zwp_text_input_v3 *ti, const char *text, int32_t begin, int32_t end) {}

static void
ti_commit_string (void *data, struct zwp_text_input_v3 *ti, const char *text)
{
  Wl *wl = data;
  g_free (wl->ti_commit);
  wl->ti_commit = g_strdup (text);
}

static void
ti_delete (void *data, struct zwp_text_input_v3 *ti, uint32_t before, uint32_t after)
{
  Wl *wl = data;
  wl->ti_before = before;
  wl->ti_after = after;
}

static void
ti_done (void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
  Wl *wl = data;
  g_autofree char *commit = g_steal_pointer (&wl->ti_commit);
  guint32 before = wl->ti_before, after = wl->ti_after;
  bool submit = wl->ti_submit;
  wl->ti_before = wl->ti_after = 0;
  wl->ti_submit = false;
  if ((before || after) && wl->cb->erase)
    wl->cb->erase (wl->data, before, after);
  if (commit && wl->cb->text)
    wl->cb->text (wl->data, commit);
  if (submit && wl->cb->submit)
    wl->cb->submit (wl->data);
}

#ifdef ZWP_TEXT_INPUT_V3_ACTION_SINCE_VERSION
static void
ti_action (void *data, struct zwp_text_input_v3 *ti, uint32_t action, uint32_t serial)
{
  Wl *wl = data;
  wl->ti_submit = action == ZWP_TEXT_INPUT_V3_ACTION_SUBMIT;
}

static void ti_language (void *data, struct zwp_text_input_v3 *ti, const char *language) {}
static void ti_preedit_hint (void *data, struct zwp_text_input_v3 *ti, uint32_t start, uint32_t end,
                             uint32_t hint) {}
#endif

static const struct zwp_text_input_v3_listener ti_listener = {
  ti_enter, ti_leave, ti_preedit, ti_commit_string, ti_delete, ti_done,
#ifdef ZWP_TEXT_INPUT_V3_ACTION_SINCE_VERSION
  ti_action, ti_language, ti_preedit_hint,
#endif
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
    WlOutput *out = g_new0 (WlOutput, 1);
    out->wl = wl;
    out->name = name;
    out->output = wl_registry_bind (r, name, &wl_output_interface, MIN (version, 4));
    wl_output_add_listener (out->output, &output_listener, out);
    g_ptr_array_add (wl->outputs, out);
  } else if (!strcmp (iface, wp_presentation_interface.name)) {
    wl->presentation = wl_registry_bind (r, name, &wp_presentation_interface, 1);
  } else if (!strcmp (iface, zwp_text_input_manager_v3_interface.name)) {
    /* version 2 (show_input_panel, the submit action) where both sides have it */
    wl->ti_version = MIN (version, (guint32) zwp_text_input_v3_interface.version);
    wl->ti_manager = wl_registry_bind (r, name, &zwp_text_input_manager_v3_interface, wl->ti_version);
  }
}

static void
output_free (gpointer data)
{
  WlOutput *out = data;
  if (wl_output_get_version (out->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
    wl_output_release (out->output);
  else
    wl_output_destroy (out->output);
  g_free (out);
}

/* an output unplugged (a TV) */
static void
registry_remove (void *data, struct wl_registry *r, uint32_t name)
{
  Wl *wl = data;
  for (guint i = 0; i < wl->outputs->len; i++) {
    WlOutput *out = g_ptr_array_index (wl->outputs, i);
    if (out->name != name)
      continue;
    if (wl->entered == out->output)
      wl->entered = NULL;
    g_ptr_array_remove_index (wl->outputs, i);
    return;
  }
}

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
  wl->outputs = g_ptr_array_new_with_free_func (output_free);
  wl->windowed = g_getenv ("VIBEOS_WINDOWED") != NULL;
  wl->xkb = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  wl->display = wl_display_connect (NULL);
  if (!wl->display) {
    g_warning ("wayland: no compositor");
    g_ptr_array_unref (wl->outputs);
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
  if (wl->ti_manager && wl->seat) {
    wl->ti = zwp_text_input_manager_v3_get_text_input (wl->ti_manager, wl->seat);
    zwp_text_input_v3_add_listener (wl->ti, &ti_listener, wl);
  }

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
  xdg_toplevel_set_title (wl->toplevel, _("Home"));
  if (!wl->windowed)
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

int
wl_get_transform (Wl *wl)
{
  return wl->transform;
}

void
wl_close (Wl *wl)
{
  if (wl->idle_id)
    g_source_remove (wl->idle_id);
  g_clear_handle_id (&wl->heal_id, g_source_remove);
  if (wl->source) {
    g_source_destroy (wl->source);
    g_source_unref (wl->source);
  }
  if (wl->frame_cb)
    wl_callback_destroy (wl->frame_cb);
  if (wl->ti)
    zwp_text_input_v3_destroy (wl->ti);
  if (wl->ti_manager)
    zwp_text_input_manager_v3_destroy (wl->ti_manager);
  g_free (wl->ti_text);
  g_free (wl->ti_commit);
  g_clear_pointer (&wl->xkb_state, xkb_state_unref);
  g_clear_pointer (&wl->keymap, xkb_keymap_unref);
  g_clear_pointer (&wl->xkb, xkb_context_unref);
  if (wl->frac)
    wp_fractional_scale_v1_destroy (wl->frac);
  if (wl->viewport)
    wp_viewport_destroy (wl->viewport);
  g_ptr_array_unref (wl->outputs);
  xdg_toplevel_destroy (wl->toplevel);
  xdg_surface_destroy (wl->xdg_surface);
  wl_surface_destroy (wl->surface);
  wl_display_disconnect (wl->display);
  g_free (wl);
}
