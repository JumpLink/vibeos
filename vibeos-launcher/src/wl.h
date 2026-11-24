/* The Wayland side: one fullscreen xdg toplevel, its exact pixel size (fractional scale + viewporter, or an integer
 * buffer scale), touch and pointer input in pixels, keys through the compositor's keymap (xkbcommon), text from the
 * input method (text-input-v3: the shell's on-screen keyboard), and a frame clock on the compositor's vsync grid.
 * Runs inside the GLib main loop. */
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
  /* a key: its evdev code, its keysym and the text it types (NULL on release, "" for none) in the keymap's state */
  void (*key)    (gpointer data, guint32 evdev_code, guint32 keysym, const char *utf8, bool pressed);
  void (*close)  (gpointer data);
  /* frame `frame` (see wl_frames) is on screen: its frame callback came */
  void (*shown)  (gpointer data, guint64 frame);
  /* the compositor suspended the window (xdg_toplevel's suspended state: nothing of it shows, an app covers it) or
   * let it be again */
  void (*suspended) (gpointer data, bool suspended);
  /* while the text input is on (wl_text_input): the input method's text to insert at the cursor, the bytes before
   * and after it to delete first (a done applies both, in that order), and its submit action (Enter) */
  void (*text)   (gpointer data, const char *utf8);
  void (*erase)  (gpointer data, int before, int after);
  void (*submit) (gpointer data);
} WlCallbacks;

typedef struct Wl Wl;

Wl                *wl_open             (const char *app_id, const WlCallbacks *cb, gpointer data);
struct wl_display *wl_get_display      (Wl *wl);
struct wl_surface *wl_get_surface      (Wl *wl);
void               wl_get_size         (Wl *wl, int *width, int *height, double *scale);
/* how the screen the window is on is turned: a wl_output transform (1: 90° counter-clockwise, the device turned
 * clockwise), 0 until the compositor says (wl_surface 6) */
int                wl_get_transform    (Wl *wl);

/* Ask for a frame callback. Call before presenting (the present commits it). Nothing to draw: don't, and no more
 * frames come until wl_schedule. */
void               wl_frame_requested  (Wl *wl);
guint64            wl_frames           (Wl *wl);      /* frames committed so far; the next one is this + 1 */
void               wl_schedule         (Wl *wl);      /* a frame is wanted: one comes at the next vsync */
gint64             wl_refresh_ns       (Wl *wl);
/* A text field has the typing (on) or none does: the input method's view of it, its text with the cursor (a byte
 * offset) and its box (pixels), sent again on every change. On is kept while the window lacks the keyboard focus,
 * and takes effect when it gets it. */
void               wl_text_input       (Wl *wl, bool on, const char *text, int cursor, float x, float y, float w,
                                        float h);
/* the on-screen keyboard, asked for (a tap on the field; again after it was put away) */
void               wl_text_input_show  (Wl *wl);
void               wl_close            (Wl *wl);
