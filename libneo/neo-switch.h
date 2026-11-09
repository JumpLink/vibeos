/* NeoSwitch: Material 3's switch with icons in its thumb (MaterialSwitch with a thumbIcon), as the alarm cards and
 * the settings rows show it.
 *
 *   off  a 52 x 32 dp track in surfaceContainerHighest with a 2 dp outline; a 24 dp outline-coloured thumb at the
 *        start with a close mark in the track colour
 *   on   the track in primary; the thumb onPrimary at the end with a check in onPrimaryContainer
 *
 * A tap anywhere on its 48 dp tall target toggles it; the thumb slides and every colour blends over 250 ms
 * (SwitchCompat's THUMB_ANIMATION_DURATION). The state is the "active" property: connect to notify::active. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_TYPE_SWITCH (neo_switch_get_type ())
G_DECLARE_FINAL_TYPE (NeoSwitch, neo_switch, NEO, SWITCH, GtkWidget)

GtkWidget *neo_switch_new        (void);
gboolean   neo_switch_get_active (NeoSwitch *self);
/* animates when the switch is on screen */
void       neo_switch_set_active (NeoSwitch *self, gboolean active);
/* without the icons (a preference row's MaterialSwitch): the thumb is 16 dp off and grows to 24 dp on, in the same
 * colours. On by default */
void       neo_switch_set_thumb_icons (NeoSwitch *self, gboolean icons);

G_END_DECLS
