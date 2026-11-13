/* VolumeSlider: the "Alarm volume" setting's slider (a discrete slider): a thick track, filled
 * in the accent up to the value and paler after it, a tall thin handle standing in a gap, and a dot at every step
 * (or, with many steps, only at the end: the screen saver's "Brightness").
 *
 * Signals: "changed" (int value): moved by a finger, step by step; "released" (): the finger let go. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_VOLUME_SLIDER (volume_slider_get_type ())
G_DECLARE_FINAL_TYPE (VolumeSlider, volume_slider, CLOCK, VOLUME_SLIDER, GtkWidget)

/* steps 0 … @max */
GtkWidget *volume_slider_new (int max);
int        volume_slider_get_value (VolumeSlider *self);
/* from outside (the volume changed elsewhere): no "changed" */
void       volume_slider_set_value (VolumeSlider *self, int value);

G_END_DECLS
