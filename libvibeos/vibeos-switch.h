/* VibeosSwitch: Material 3's switch with icons in its thumb, as the alarm cards and the settings rows show it.
 *
 *   off  a dark, outlined track; the thumb at the start in the outline's colour, with a close mark in the track's
 *   on   the track in the primary colour; the thumb at the end, with a check
 *
 * A tap anywhere on its target toggles it; the thumb slides and every colour blends over 250 ms. The state is the
 * "active" property: connect to notify::active. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_SWITCH (vibeos_switch_get_type ())
G_DECLARE_FINAL_TYPE (VibeosSwitch, vibeos_switch, VIBEOS, SWITCH, GtkWidget)

GtkWidget *vibeos_switch_new        (void);
gboolean   vibeos_switch_get_active (VibeosSwitch *self);
/* animates when the switch is on screen */
void       vibeos_switch_set_active (VibeosSwitch *self, gboolean active);
/* without the icons (a preference row's switch): the thumb is smaller off and grows to full size on, in the same
 * colours. On by default */
void       vibeos_switch_set_thumb_icons (VibeosSwitch *self, gboolean icons);

G_END_DECLS
