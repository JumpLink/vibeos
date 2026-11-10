/* AnalogClock: DeskClock's analog clock (src/AnalogClock.java, D/clock_analog_*.xml; spec/clock.md §6.1).
 *
 * A white ring with three hands. As on Android the hour hand points at the hour only (HOUR × 30°, no minute
 * fraction), so it jumps once an hour; the minute hand jumps once a minute; the second hand, in colorAccent, ticks
 * once a second, on the second, when seconds are on. The face is drawn in the drawables' 1176-unit square, scaled to
 * the largest square that fits the widget and centred in it (ImageView's fitCenter). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_ANALOG_CLOCK (analog_clock_get_type ())
G_DECLARE_FINAL_TYPE (AnalogClock, analog_clock, CLOCK, ANALOG_CLOCK, GtkWidget)

/* @size: the face's own size in dp (the drawables' width, analog_clock_size); the widget shrinks below it to fit
 * the width it is given, keeping square */
GtkWidget *analog_clock_new (int size);
/* the zone it shows; NULL for the phone's own */
void       analog_clock_set_time_zone (AnalogClock *self, GTimeZone *tz);
/* the second hand, and a tick every second (enableSeconds) */
void       analog_clock_set_seconds (AnalogClock *self, gboolean seconds);

G_END_DECLS
