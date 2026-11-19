/* A parallax wallpaper: transparent layers over a background, each a texture a little larger than the screen, shifted
 * by its depth as the phone tilts. The tilt is the accelerometer's (IIO sysfs), measured against a slowly following
 * rest position so the picture sits centred however the phone is held. */
#pragma once

#include <stdbool.h>

typedef struct Parallax Parallax;

/* `dir` holds parallax.json ({"range": dp, "tilt": g, "layers": [{"file", "depth"}…]}, back to front) */
Parallax *parallax_new    (const char *dir, void (*wake) (void *data), void *data);
/* a still picture: one layer that never moves, and no sensor */
Parallax *parallax_new_image (const char *file);
void      parallax_free   (Parallax *p);
void      parallax_resize (Parallax *p, int w, int h, double scale);
/* who is called when the picture changes by itself (a turned screen's layers decoded): a frame is wanted */
void      parallax_set_wake (Parallax *p, void (*wake) (void *data), void *data);
void      parallax_run    (Parallax *p, bool run);     /* follow the tilt while shown */
/* one frame's sensor step; true while the picture moves (another frame is wanted). At rest the sensor is polled
 * slowly and `wake` is called when it moves again. */
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
