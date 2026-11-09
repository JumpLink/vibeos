/* Settings (spec §4.3-4.4): with one SIM, SettingsActivity opens ApplicationSettingsActivity straight away, titled
 * "Settings". Its rows that this app acts on: "Outgoing message sounds" and "Swipe to delete" (GSettings
 * de.yesman.Messaging).
 *
 * Left out, as each would claim what the phone does not do: "Default SMS app" (no default to change here; the
 * session's sms: handler is set apart from the app), "Notifications" (the system's notification settings for the
 * app: this phone has no such page), and "Advanced" (the SIM's MMS settings and delivery reports: the service sends
 * no MMS and asks for no delivery reports yet). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_SETTINGS (msg_settings_get_type ())
G_DECLARE_FINAL_TYPE (MsgSettings, msg_settings, MSG, SETTINGS, GtkWidget)

GtkWidget *msg_settings_new (void);

/* Signals: "closed" () (the up arrow) */

G_END_DECLS
