/* CalCompatSwitch: androidx SwitchCompat, the switch Etar's settings screens draw (SwitchPreferenceCompat;
 * still:cal-settings-general), which is not the Material 3 switch of its editor (NeoSwitch).
 *
 * A libneo candidate: any app rebuilding an androidx preference screen needs it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_COMPAT_SWITCH (cal_compat_switch_get_type ())
G_DECLARE_FINAL_TYPE (CalCompatSwitch, cal_compat_switch, CAL, COMPAT_SWITCH, GtkWidget)

GtkWidget *cal_compat_switch_new        (void);
gboolean   cal_compat_switch_get_active (CalCompatSwitch *self);
void       cal_compat_switch_set_active (CalCompatSwitch *self, gboolean active);

G_END_DECLS
