/* CalMaterialSwitch: the editor's All day switch, Material 3's MaterialSwitch without thumb icons, in the colours
 * of the look in use (cal-colors.h). libneo's NeoSwitch draws the same switch in its own dark palette only; this is
 * it over the app's palette, so the light theme has a light switch.
 *
 *   off  a 52 x 32 dp track in surfaceContainerHighest with a 2 dp outline, a 16 dp thumb in outline at the start
 *   on   the track in primary, a 24 dp thumb in onPrimary at the end
 *
 * A tap on its 48 dp target toggles it, the thumb sliding and the colours blending over 250 ms. The state is the
 * "active" property. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_MATERIAL_SWITCH (cal_material_switch_get_type ())
G_DECLARE_FINAL_TYPE (CalMaterialSwitch, cal_material_switch, CAL, MATERIAL_SWITCH, GtkWidget)

GtkWidget *cal_material_switch_new        (void);
gboolean   cal_material_switch_get_active (CalMaterialSwitch *self);
void       cal_material_switch_set_active (CalMaterialSwitch *self, gboolean active);

/* CalCheckBox: a CheckBoxPreference's MaterialCheckBox (Use home time zone): an 18 dp box with 2 dp corners, a 2 dp
 * onSurfaceVariant outline off, filled in primary with an onPrimary check on, in a 48 dp target. "active" as above. */
#define CAL_TYPE_CHECK_BOX (cal_check_box_get_type ())
G_DECLARE_FINAL_TYPE (CalCheckBox, cal_check_box, CAL, CHECK_BOX, GtkWidget)

GtkWidget *cal_check_box_new        (void);
gboolean   cal_check_box_get_active (CalCheckBox *self);
void       cal_check_box_set_active (CalCheckBox *self, gboolean active);

G_END_DECLS
