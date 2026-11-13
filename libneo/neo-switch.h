/* NeoSwitch: Material 3's switch with icons in its thumb, as the alarm cards and the settings rows show it.
 *
 *   off  a dark, outlined track; the thumb at the start in the outline's colour, with a close mark in the track's
 *   on   the track in the primary colour; the thumb at the end, with a check
 *
 * A tap anywhere on its target toggles it; the thumb slides and every colour blends over 250 ms. The state is the
 * "active" property: connect to notify::active. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_TYPE_SWITCH (neo_switch_get_type ())
G_DECLARE_FINAL_TYPE (NeoSwitch, neo_switch, NEO, SWITCH, GtkWidget)

GtkWidget *neo_switch_new        (void);
gboolean   neo_switch_get_active (NeoSwitch *self);
/* animates when the switch is on screen */
void       neo_switch_set_active (NeoSwitch *self, gboolean active);
/* without the icons (a preference row's switch): the thumb is smaller off and grows to full size on, in the same
 * colours. On by default */
void       neo_switch_set_thumb_icons (NeoSwitch *self, gboolean icons);

G_END_DECLS
