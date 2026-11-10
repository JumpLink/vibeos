/* Settings (spec §4.3-4.5): with one SIM, SettingsActivity opens ApplicationSettingsActivity straight away, titled
 * "Settings". Its rows that this app acts on: "Outgoing message sounds", "Swipe to delete" and "Advanced", which
 * opens the SIM's settings (PerSubscriptionSettingsActivity, "Advanced settings") with its "Advanced" category's
 * "SMS delivery reports". All are GSettings de.yesman.Messaging; the service reads "delivery-reports".
 *
 * Left out, as each would claim what the phone does not do: "Default SMS app" (no default to change here; the
 * session's sms: handler is set apart from the app), "Notifications" (the system's notification settings for the
 * app: this phone has no such page), and the SIM settings' "MMS" category (Group messaging, Your phone number,
 * Auto-retrieve, Roaming auto-retrieve: the service sends and fetches no MMS). The overflow's "Open source
 * licenses" (LicenseActivity's licenses.html) too: the project ships no licence text to show. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_SETTINGS (msg_settings_get_type ())
G_DECLARE_FINAL_TYPE (MsgSettings, msg_settings, MSG, SETTINGS, GtkWidget)

GtkWidget *msg_settings_new (void);
/* the SIM's settings, "Advanced settings": what "Advanced" opens */
GtkWidget *msg_settings_new_advanced (void);

/* Signals: "closed" () (the up arrow); "show-advanced" () (the "Advanced" row) */

G_END_DECLS
