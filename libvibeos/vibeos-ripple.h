/* vibeos-ripple: the touch ripple, a soft-edged disc that grows from the finger, for any widget to draw.
 *
 * A widget keeps a VibeosRipple, calls vibeos_ripple_press () where the finger lands and vibeos_ripple_release () when it
 * lifts or the touch is taken away, and draws it from its snapshot with vibeos_ripple_snapshot (), clipped to its own
 * shape. VibeosPressable does all of this for a widget that is just "a thing you tap".
 *
 * The motion:
 *
 *   progress p   0 -> 0.5 over 450 ms, fast-out-slow-in, from the press;
 *                0.5 -> 1 over 375 ms, linear, from the release, but never before the 450 ms are over
 *   alpha        min (p / 0.13, 1 - (p - 0.4) / 0.6), clamped to 0..1
 *   centre       slides from the touch point to the middle of the bounds by p = 0.5
 *   radius       2.3 x half the bounds' diagonal x p, with a soft edge: solid out to half the radius, gone at 1.5x
 *
 * so a quick tap still plays the full 825 ms. Several waves can be on screen at once: each press starts a new one. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_RIPPLE_WAVES 4

typedef struct {
  double  x, y;                 /* the touch point, in the bounds' coordinates */
  gint64  pressed_us;           /* 0: a free slot */
  gint64  released_us;          /* 0 while the finger is down */
} VibeosRippleWave;

typedef struct {
  GtkWidget     *widget;        /* the widget that draws it */
  guint          tick;
  VibeosRippleWave  waves[VIBEOS_RIPPLE_WAVES];
} VibeosRipple;

void     vibeos_ripple_init     (VibeosRipple *ripple, GtkWidget *widget);
/* stops and forgets every wave; call from the widget's dispose */
void     vibeos_ripple_clear    (VibeosRipple *ripple);
void     vibeos_ripple_press    (VibeosRipple *ripple, double x, double y);
void     vibeos_ripple_release  (VibeosRipple *ripple);
gboolean vibeos_ripple_is_active (VibeosRipple *ripple);

/* Draws the waves over `bounds` (their centre is where a wave ends up, their half diagonal its size), in `color`
 * at the alpha the colour has. The caller clips. */
void     vibeos_ripple_snapshot (VibeosRipple *ripple, GtkSnapshot *snapshot, const graphene_rect_t *bounds,
                              const GdkRGBA *color);

/* The default ripple colour: a translucent white, halved so that ripple and
 * background together reach the full alpha. */
GdkRGBA  vibeos_ripple_default_color (void);

G_END_DECLS
