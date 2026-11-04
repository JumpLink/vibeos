/* The Wayland side: one fullscreen xdg toplevel, its exact pixel size (fractional scale + viewporter, or an integer
 * buffer scale), touch and pointer input in pixels, and a frame clock on the compositor's vsync grid. Runs inside the
 * GLib main loop. */
#pragma once

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  WL_TOUCH_DOWN,
  WL_TOUCH_MOTION,
  WL_TOUCH_UP,
  WL_TOUCH_CANCEL,      /* every sequence; id is -1 */
} WlTouchType;

typedef struct {
  /* the surface's size changed (pixels) or its scale did; the next frame draws at the new size */
  void (*resize) (gpointer data, int width, int height, double scale);
  /* draw a frame for presentation at `time` (CLOCK_MONOTONIC, ns) */
  void (*frame)  (gpointer data, gint64 time);
  /* pointer events arrive as touch id 1000 */
  void (*touch)  (gpointer data, WlTouchType type, int id, double x, double y, guint32 time_ms);
  void (*key)    (gpointer data, guint32 evdev_code, bool pressed);
  void (*close)  (gpointer data);
} WlCallbacks;

typedef struct Wl Wl;

Wl                *wl_open             (const char *app_id, const WlCallbacks *cb, gpointer data);
struct wl_display *wl_get_display      (Wl *wl);
struct wl_surface *wl_get_surface      (Wl *wl);
void               wl_get_size         (Wl *wl, int *width, int *height, double *scale);

/* Ask for a frame callback. Call before presenting (the present commits it). Nothing to draw: don't, and no more
 * frames come until wl_schedule. */
void               wl_frame_requested  (Wl *wl);
void               wl_schedule         (Wl *wl);      /* a frame is wanted: one comes at the next vsync */
gint64             wl_refresh_ns       (Wl *wl);
void               wl_close            (Wl *wl);
