/* What the Phone's service asks of the phone's shell: to show the in-call screen over the lock screen, and to open
 * an app where a notification points. (The Clock asks the same: apps/clock/src/phone.h.) */
#pragma once

#include <gio/gio.h>

/* @shown: the shell showed the app full screen; FALSE when the phone is in use, unlocked, and the shell left it
 * to the notification's heads-up */
typedef void (*PhoneFullScreenDone) (gboolean shown, gpointer data);

/* a full-screen request: the shell opens @app_id over the lock screen and lights the screen if the phone is
 * asleep or locked; in use, it declines. Without a shell to ask, the app is started anyway (and counts as shown) */
void phone_show_full_screen (GDBusConnection *session, const char *app_id, PhoneFullScreenDone done, gpointer data);

/* starts @app_id from its desktop file, or brings it to the front if it runs */
void phone_start_app (const char *app_id);
/* activates @app_id's GAction @action, with @parameter (floating, or NULL), D-Bus activating the app if it does not
 * run: "show the call log", say */
void phone_open_app (GDBusConnection *session, const char *app_id, const char *action, GVariant *parameter);
