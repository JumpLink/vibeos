/* The Clock service's notifications in the shade, over org.freedesktop.Notifications.
 *
 * Not GNotification: that one never says when the user swiped a notification away, which some of the clock's
 * notifications answer: an upcoming alarm then stays hidden, a missed one is over. Each kind of notification (alarms,
 * timers, the stopwatch) has a notifier of its own, which hears only of the notifications it posted. */
#pragma once

#include <gio/gio.h>

/* org.freedesktop.Notifications' urgencies. Low ones are filed under "Silent" and never peek; critical ones peek
 * and stay while something rings. */
typedef enum {
  CLOCK_URGENCY_LOW = 0,
  CLOCK_URGENCY_NORMAL = 1,
  CLOCK_URGENCY_CRITICAL = 2,
} ClockUrgency;

typedef struct _ClockNotifier ClockNotifier;

/* the user acted on notification @id: @action is one of its buttons' keys, "default" (tapped), or "closed" (swiped
 * away); "gone" when it went another way (closed by us, or the server lost it) */
typedef void (*ClockNotifierAction) (guint32 id, const char *action, gpointer data);
/* the notification server came (back): nothing shown before is there now */
typedef void (*ClockNotifierRestart) (gpointer data);

ClockNotifier *clock_notifier_new (ClockNotifierAction action, ClockNotifierRestart restart, gpointer data);
void           clock_notifier_free (ClockNotifier *self);

/* posts a notification, or with @replaces (an id of ours) updates it in place; its id, 0 if there is no server.
 * @actions: key, label pairs, NULL-terminated; "default" is a tap on the notification. @hints: more hints (a{sv},
 * floating), or NULL. Every one stays when a button is pressed (it is "resident": we take it down when what it says
 * is over) and shows in full on the lock screen. */
guint32 clock_notifier_notify (ClockNotifier *self, guint32 replaces, const char *title, const char *body,
                               const char * const *actions, ClockUrgency urgency, GVariant *hints);
void    clock_notifier_close (ClockNotifier *self, guint32 id);

/* extras in a notification's header, as hints for clock_notifier_notify: a chronometer, which the shell shows as
 * "Clock • 04:59" and counts on by itself, from @chronometer, the wall-clock time (Unix ms) at which it reads zero,
 * 0 for none, counting down to it or up from it; and the sub-text ("Clock • Paused"), or NULL. The shell renders
 * them (extension/notifications.js); other notification servers leave them out. */
GVariant *clock_notifier_header_hints (gint64 chronometer, gboolean countdown, const char *subtext);

/* "04:59" or "1:04:59" */
char     *clock_notifier_format_elapsed (gint64 seconds);
