/* What the timer and stopwatch tabs draw: the two shapes their rings are made of, and text that shrinks to fit. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* An arc: from `start` degrees (0 is 3 o'clock, 270 is 12 o'clock) for `sweep` degrees clockwise; a sweep of 360 or
 * more is the whole circle, a negative one goes the other way. */
typedef struct
{
  double start, sweep;
  GdkRGBA color;
} ClockArc;

/* Stroked arcs (butt ends), one for each of `arcs`, one over the other: the circle at (cx, cy), radius `radius`,
 * `width` wide. `scale`: device pixels per unit (neo_device_scale).
 *
 * The rings change every frame while a timer or the stopwatch runs, and GTK has cairo rasterise a stroke again in
 * every frame that has a new one: a ring 288 px across cost the stopwatch half a core. So the ring is rasterised
 * once per size, kept as a texture, and used as the mask for its colours, a conic gradient the GPU draws. */
void clock_snapshot_arcs (GtkSnapshot *snapshot, int scale, double cx, double cy, double radius, double width,
                          const ClockArc *arcs, guint n_arcs);

/* A filled circle, as a rounded clip the GPU draws (a filled path is cairo's, every frame) */
void clock_snapshot_dot (GtkSnapshot *snapshot, double cx, double cy, double radius, const GdkRGBA *color);

/* the label's text at the largest size up to `max_size`, in whole device pixels, at which it is no wider than `width`;
 * at `max_size` when it fits. Not from inside a size_allocate: the label then needs a new size itself. */
void clock_label_fit (GtkWidget *label, double max_size, double width);

G_END_DECLS
