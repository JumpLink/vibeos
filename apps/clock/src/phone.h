/* What the Clock's service asks of the phone for a ringing alarm or timer: to stay awake while it rings, to show
 * the ringing screen over the lock screen, and to wake from sleep at the time something is due; and to open the
 * Clock where a notification points. */
#pragma once

#include <gio/gio.h>

/* logind's sleep lock (a "block" inhibitor), as a file descriptor to close when it may sleep again; -1 if logind
 * said no. neo-wakeup holds the phone up only 30 s after it wakes it: something ringing needs longer. */
int  phone_inhibit_sleep (GDBusConnection *system, const char *why);
void phone_allow_sleep (int *inhibitor);

/* full screen: the shell opens @app_id over the lock screen and lights the screen if the phone is
 * asleep or locked; in use, it declines and the notification's heads-up stands for it. Without a shell to ask the
 * app is started anyway. */
void phone_show_full_screen (GDBusConnection *session, const char *app_id);

/* neo-wakeup's RTC wake-up named @name at @time (Unix seconds; 0 clears it). @alarm_clock: it is also the next
 * alarm the status bar shows */
void phone_wake_at (GDBusConnection *system, const char *name, gint64 time, gboolean alarm_clock);

/* starts @app_id from its desktop file (a tap on a ringing notification opens the ringing screen, locked or not) */
void phone_start_app (const char *app_id);
/* activates @app_id's GAction @action, with @parameter (floating, or NULL), D-Bus activating the app if it does not
 * run: "show this timer", say */
void phone_open_app (GDBusConnection *session, const char *app_id, const char *action, GVariant *parameter);
