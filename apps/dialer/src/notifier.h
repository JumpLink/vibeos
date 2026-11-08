/* The Phone service's notifications in the shade, over org.freedesktop.Notifications: the incoming call with Answer
 * and Decline, the ongoing call with Hang up, and the missed calls (InCallUI's StatusBarNotifier and the Dialer's
 * MissedCallNotifier).
 *
 * The same as the Clock's clock-notifier (apps/clock/src/clock-notifier.h), which says why it is not GNotification:
 * that one never says when the user swiped a notification away, and a missed-call notification answers that (the
 * missed calls are then seen). */
#pragma once

#include <gio/gio.h>

/* org.freedesktop.Notifications' urgencies. Low ones are filed under "Silent" and never peek; normal ones peek;
 * critical ones peek and stay until taken down (a ringing call). */
typedef enum {
  NOTIFIER_URGENCY_LOW = 0,
  NOTIFIER_URGENCY_NORMAL = 1,
  NOTIFIER_URGENCY_CRITICAL = 2,
} NotifierUrgency;

typedef struct _Notifier Notifier;

/* the user acted on notification @id: @action is one of its buttons' keys, "default" (tapped), or "closed" (swiped
 * away); "gone" when it went another way (closed by us, or the server lost it) */
typedef void (*NotifierAction) (guint32 id, const char *action, gpointer data);
/* the notification server came (back): nothing shown before is there now */
typedef void (*NotifierRestart) (gpointer data);

Notifier *notifier_new (NotifierAction action, NotifierRestart restart, gpointer data);
void      notifier_free (Notifier *self);

/* posts a notification, or with @replaces (an id of ours) updates it in place; its id, 0 if there is no server.
 * @actions: key, label pairs, NULL-terminated; "default" is a tap on the notification. @hints: more hints (a{sv},
 * floating), or NULL. Every one stays when a button is pressed (it is "resident": we take it down when what it says
 * is over) and shows in full on the lock screen. */
guint32 notifier_notify (Notifier *self, guint32 replaces, const char *title, const char *body,
                         const char * const *actions, NotifierUrgency urgency, GVariant *hints);
void    notifier_close (Notifier *self, guint32 id);

/* Android's extras in a notification's header, as hints for notifier_notify: a chronometer (setUsesChronometer),
 * which the shell shows as "Phone • 04:59" and counts on by itself, from @chronometer, the wall-clock time (Unix ms)
 * at which it reads zero, 0 for none, counting up from it; the sub-text (setSubText), or NULL; and @no_peek, for a
 * notification on a channel of default importance (the ongoing call): in the list, but no heads-up. The shell
 * renders them (extension/notifications.js); other notification servers leave them out. */
GVariant *notifier_header_hints (gint64 chronometer, const char *subtext, gboolean no_peek);
