/* vibeos-restart: a program of VibeOS's own starting over in the session's new language.
 *
 * Settings changes the session's language at once (vibeos-locale.h, vibeos_locale_set_session), and tells the running
 * programs by vibeos.Locale.Changed. A program that cannot make what it shows or says again in the new language (a
 * service whose notifications and spoken words are written as they come) starts over in it instead, as Android
 * recreates what a configuration change touches: once it is idle, it is itself again, in the new environment. Only
 * the programs that ask for it: nothing else is restarted.
 *
 * How: under systemd, as the main process of a service of Type=dbus (the Phone's call service, the Messages' SMS
 * service, the Calendar's reminders, the Clock's alarms), systemd restarts the service, in the environment Settings
 * gave systemd: started over in place, it would drop its bus name for a moment, and systemd stops a Type=dbus service
 * whose name goes. Any other way (Agent in the scope of its autostart, a test's service): the same executable again
 * in the same process (execve), with the arguments @prepare gives or its own, its well-known names on the session bus
 * given up first so that it finds them free. Its settings are written out first (g_settings_sync). Nothing of the
 * process's own is changed before it goes: no setenv, no setlocale.
 *
 * A program started over this way, or one systemd started, also looks at its start at the language systemd's user
 * manager now starts programs in: one that changed while it was starting (the gap of a restart) is followed then.
 *
 * Only GLib and GIO: the services build this file in, as they do vibeos-locale.c. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* TRUE while the program must not go: a call, an alarm ringing, a message being sent, Agent listening or speaking */
typedef gboolean (*VibeosRestartBusyFunc) (gpointer user_data);

/* called with @going TRUE just before the program goes: what it must do first (its notifications closed, to be posted
 * again in the new language by the program that comes: the shell keeps a notification whose sender left), and the
 * arguments it starts over with after its own name, newly allocated: NULL for the ones it was started with. Under
 * systemd the service's own command is used. Called again with @going FALSE if it did not go after all (systemd did
 * not restart the service, its executable could not be started again): what it took down it puts back, and the
 * value returned is not used. */
typedef char **(*VibeosRestartPrepareFunc) (gboolean going, gpointer user_data);

/* the program starts over in the session's new language when Settings changes it, once @busy (NULL: never busy) says
 * it is idle: asked a moment after the change, and every few seconds while it is busy. @prepare may be NULL. Call it
 * once, from the main thread, with a main loop to follow. */
void     vibeos_restart_follow_language (VibeosRestartBusyFunc busy, VibeosRestartPrepareFunc prepare,
                                         gpointer user_data);

/* the program may have become idle (a call ended, an alarm was dismissed): a restart waiting starts as soon as the
 * main loop is back, so it may be called from any callback */
void     vibeos_restart_check (void);

/* a new language waits for the program to be idle */
gboolean vibeos_restart_pending (void);

G_END_DECLS
