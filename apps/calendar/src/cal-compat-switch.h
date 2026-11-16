/* CalCompatSwitch: the settings screens' switch, a thin track under a bigger thumb (cal-compat-switch.c); not the
 * editor's switch (CalMaterialSwitch).
 *
 * A libvibeos candidate: any settings screen of this style needs it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_COMPAT_SWITCH (cal_compat_switch_get_type ())
G_DECLARE_FINAL_TYPE (CalCompatSwitch, cal_compat_switch, CAL, COMPAT_SWITCH, GtkWidget)

GtkWidget *cal_compat_switch_new        (void);
gboolean   cal_compat_switch_get_active (CalCompatSwitch *self);
void       cal_compat_switch_set_active (CalCompatSwitch *self, gboolean active);

G_END_DECLS
