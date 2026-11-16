/* CalMaterialSwitch: the editor's All day switch, without thumb icons, in the colours of the look in use
 * (cal-colors.h). libvibeos's VibeosSwitch draws the same switch in its own dark palette only; this is it over
 * the app's palette, so the light theme has a light switch.
 *
 *   off  an outlined track, a small thumb at the start
 *   on   a filled track, a bigger thumb at the end
 *
 * A tap on its touch target, taller than the track, toggles it, the thumb sliding and the colours blending over
 * 250 ms. The state is the "active" property. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_MATERIAL_SWITCH (cal_material_switch_get_type ())
G_DECLARE_FINAL_TYPE (CalMaterialSwitch, cal_material_switch, CAL, MATERIAL_SWITCH, GtkWidget)

GtkWidget *cal_material_switch_new        (void);
gboolean   cal_material_switch_get_active (CalMaterialSwitch *self);
void       cal_material_switch_set_active (CalMaterialSwitch *self, gboolean active);

/* CalCheckBox: a settings check box (Use home time zone): an outlined box off, filled with a tick on, in a
 * thumb-sized touch target. "active" as above. */
#define CAL_TYPE_CHECK_BOX (cal_check_box_get_type ())
G_DECLARE_FINAL_TYPE (CalCheckBox, cal_check_box, CAL, CHECK_BOX, GtkWidget)

GtkWidget *cal_check_box_new        (void);
gboolean   cal_check_box_get_active (CalCheckBox *self);
void       cal_check_box_set_active (CalCheckBox *self, gboolean active);

G_END_DECLS
