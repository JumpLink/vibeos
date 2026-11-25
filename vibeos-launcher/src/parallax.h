/* A parallax wallpaper: transparent layers over a background, each a texture a little larger than the screen, shifted
 * by its depth as the phone tilts. The tilt is the accelerometer's (IIO sysfs), measured against a slowly following
 * rest position so the picture sits centred however the phone is held. */
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdbool.h>

typedef struct Parallax Parallax;

/* `dir` holds parallax.json ({"range": dp, "tilt": g, "layers": [{"file", "depth"}…]}, back to front) */
Parallax *parallax_new    (const char *dir, void (*wake) (void *data), void *data);
/* a still picture: one layer that never moves, and no sensor */
Parallax *parallax_new_image (const char *file);
/* the parallax set a wallpaper `file` is the picture of: its folder when that holds a parallax.json (the set at rest,
 * for the panel and the lock screen), else NULL (a still picture) */
char     *parallax_set_of (const char *file);
/* The wallpaper `file` as the home shows it at rest on a w × h screen of `scale` px per dp, zoomed by `zoom` about
 * the screen's centre (the home's WALLPAPER_ZOOM): its set's layers when it is a set's picture, with the tilt
 * centred, else the picture covering the screen. Composed on the CPU, for the lock screen (vibeos-launcher
 * --rest-picture), so that unlocking into the home moves nothing. NULL when nothing in it could be read. */
GdkPixbuf *parallax_rest_picture (const char *file, int w, int h, double scale, float zoom);
void      parallax_free   (Parallax *p);
void      parallax_resize (Parallax *p, int w, int h, double scale);
/* who is called when the picture changes by itself (the tilt moved a layer a px, a turned screen's layers decoded):
 * a frame is wanted */
void      parallax_set_wake (Parallax *p, void (*wake) (void *data), void *data);
/* Follow the tilt while shown: the filter steps 60 times a second on a timer of its own, and the layers are drawn at
 * offsets in whole px of the fastest one, so `wake` is called only when a step moves that a px; at rest the sensor is
 * polled slowly. */
void      parallax_run    (Parallax *p, bool run);
/* how the screen is turned (a wl_output transform): the tilt is taken on the screen's axes as it shows */
void      parallax_set_transform (Parallax *p, int transform);
/* the frame's step of the way back from a hold; true while it lasts (another frame is wanted) */
bool      parallax_tick   (Parallax *p);
/* Held, the offsets keep still whatever the tilt does (through a transition, so the layers are drawn as one picture
 * the whole way); let go, the picture eases to where the tilt has it in 150 ms. Before the frame's tick. */
void      parallax_hold   (Parallax *p, bool hold);
/* Before the frame's passes. While the layers keep still against each other (the drawer and the home only zoom
 * them), they are drawn into one picture once and that picture is drawn for them, a single opaque quad instead of
 * one blended quad a layer; while the tilt moves them, they are drawn one by one. `rest_zoom` is the zoom the picture
 * rests at (the home's), where the one picture is the layers' own pixels; it covers the screen from a zoom of 1 up. */
void      parallax_prepare (Parallax *p, float rest_zoom);
/* the whole screen, zoomed by `zoom` about its centre (1: none); the first thing drawn in its pass (cleared to black) */
void      parallax_draw   (Parallax *p, float zoom);
