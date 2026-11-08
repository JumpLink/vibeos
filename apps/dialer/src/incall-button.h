/* One button of the in-call screen's grid: InCallUI's CheckableLabeledButton (spec §8.8).
 *
 * A 64 dp circle with a 36 dp icon in it and a 12 sp label under it. Checked (Mute on, Speaker on, the keypad
 * open, the call on hold) the circle fills light and the icon turns black; insensitive, the button shows at 30 %.
 * The ripple fills the circle wherever on the button the finger lands. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define INCALL_TYPE_BUTTON (incall_button_get_type ())
G_DECLARE_FINAL_TYPE (InCallButton, incall_button, INCALL, BUTTON, GtkWidget)

GtkWidget *incall_button_new (const char *icon_name, const char *label);
void       incall_button_set_checked (InCallButton *self, gboolean checked);
gboolean   incall_button_get_checked (InCallButton *self);
void       incall_button_set_icon (InCallButton *self, const char *icon_name);
void       incall_button_set_label (InCallButton *self, const char *label);

/* Signals: "clicked" () */

G_END_DECLS
