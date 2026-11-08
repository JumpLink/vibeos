/* The Phone's notifications (InCallUI's StatusBarNotifier and the Dialer's MissedCallNotifier, spec §8.16):
 *
 *  - the call in front, one at a time: "Incoming call" with Decline and Answer, which peeks and stays while it
 *    rings; "Dialing", "Ongoing call" counting on, "On hold", each with Hang up and the speaker;
 *  - one per missed call: "Missed call" and who, with Call back. Swiped away or tapped, it is seen.
 *
 * The texts come from the caller: what a call is called (a contact's name, the number, "Private number") is the
 * service's to work out. */
#pragma once

#include <glib.h>

typedef struct _CallNotifications CallNotifications;

typedef enum {
  CALL_NOTIFICATION_INCOMING,   /* ringing, the answer screen not in front: it peeks */
  CALL_NOTIFICATION_INCOMING_QUIET,     /* ringing, the answer screen shows it already */
  CALL_NOTIFICATION_DIALING,
  CALL_NOTIFICATION_ONGOING,
  CALL_NOTIFICATION_HELD,
} CallNotificationKind;

/* the user acted on a notification. For the call's: "answer", "decline", "hangup", "speaker-on", "speaker-off",
 * "open" (tapped). For a missed call's, with the call log id: "call-back", "open" (tapped), "closed" (swiped
 * away) */
typedef void (*CallNotificationAction) (gboolean missed, gint64 id, const char *action, gpointer data);

CallNotifications *call_notifications_new (CallNotificationAction callback, gpointer data);
void               call_notifications_free (CallNotifications *self);

/* the notification of call @call_id, in place of any for another call. @connected: when an ongoing call was
 * connected (Unix ms), which its chronometer counts from. @speaker: the speaker is on, so the action turns it off */
void call_notifications_show_call (CallNotifications *self, guint call_id, CallNotificationKind kind,
                                   const char *title, gint64 connected, gboolean speaker);
void call_notifications_clear_call (CallNotifications *self);

/* a missed call's notification: @log_id is its row in the call log, @who what it is called, @number for calling
 * back ("" or NULL for a withheld one: no Call back) */
void call_notifications_show_missed (CallNotifications *self, gint64 log_id, const char *who, const char *number);
void call_notifications_clear_missed (CallNotifications *self, gint64 log_id);
/* every missed call is seen */
void call_notifications_clear_all_missed (CallNotifications *self);
