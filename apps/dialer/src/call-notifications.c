/* The Phone's notifications (call-notifications.h). */
#include "call-notifications.h"
#include "notifier.h"

#include <glib/gi18n.h>

typedef struct {
  guint32 id;                   /* the server's; 0 while it is not up */
  char *who, *number;           /* what it shows, to show it again if the server restarts */
} Missed;

struct _CallNotifications {
  Notifier *notifier;
  CallNotificationAction callback;
  gpointer data;

  /* the call's: one at a time */
  guint32 call_notification;
  guint call_id;
  CallNotificationKind kind;
  char *title;
  gint64 connected;
  gboolean speaker;

  GHashTable *missed;           /* call log id (gint64 *) -> Missed */
};

static void
missed_free (Missed *missed)
{
  g_free (missed->who);
  g_free (missed->number);
  g_free (missed);
}

/* --- the call's */

static void
post_call (CallNotifications *self)
{
  /* the call notification's text and actions */
  const char *incoming_actions[] = { "default", "", "decline", _("Decline"), "answer", _("Answer"), NULL };
  const char *speaker_on[] = { "default", "", "hangup", _("Hang up"), "speaker-on", _("Turn speaker on"), NULL };
  const char *speaker_off[] = { "default", "", "hangup", _("Hang up"), "speaker-off", _("Turn speaker off"), NULL };
  const char * const *actions = self->speaker ? speaker_off : speaker_on;
  NotifierUrgency urgency = NOTIFIER_URGENCY_NORMAL;
  GVariant *hints = NULL;
  const char *text;

  switch (self->kind)
    {
    case CALL_NOTIFICATION_INCOMING:
      /* the "Incoming calls" channel, high importance with a full-screen intent: it peeks, and stays while it rings */
      text = _("Incoming call");
      actions = incoming_actions;
      urgency = NOTIFIER_URGENCY_CRITICAL;
      break;
    case CALL_NOTIFICATION_INCOMING_QUIET:
      /* quiet: the answer screen is up, so on the ongoing channel, which does not peek */
      text = _("Incoming call");
      actions = incoming_actions;
      hints = notifier_header_hints (0, NULL, TRUE);
      break;
    case CALL_NOTIFICATION_DIALING:
      text = _("Dialing");
      hints = notifier_header_hints (0, NULL, TRUE);
      break;
    case CALL_NOTIFICATION_HELD:
      text = _("On hold");
      hints = notifier_header_hints (0, NULL, TRUE);
      break;
    case CALL_NOTIFICATION_ONGOING:
    default:
      text = _("Ongoing call");
      /* the chronometer counts from when it was connected */
      hints = notifier_header_hints (self->connected, NULL, TRUE);
      break;
    }
  self->call_notification = notifier_notify (self->notifier, self->call_notification, self->title, text, actions,
                                             urgency, hints);
}

void
call_notifications_show_call (CallNotifications *self, guint call_id, CallNotificationKind kind, const char *title,
                              gint64 connected, gboolean speaker)
{
  /* a notification is shown again only when what it says changed: a peeking one would peek again */
  if (self->call_notification && self->call_id == call_id && self->kind == kind && self->connected == connected
      && self->speaker == speaker && g_strcmp0 (self->title, title) == 0)
    return;
  /* another kind is another channel: it is posted anew, and so it may peek */
  if (self->call_notification && (self->call_id != call_id || self->kind != kind))
    {
      notifier_close (self->notifier, self->call_notification);
      self->call_notification = 0;
    }
  self->call_id = call_id;
  self->kind = kind;
  g_free (self->title);
  self->title = g_strdup (title);
  self->connected = connected;
  self->speaker = speaker;
  post_call (self);
}

void
call_notifications_clear_call (CallNotifications *self)
{
  notifier_close (self->notifier, self->call_notification);
  self->call_notification = 0;
  self->call_id = 0;
}

/* --- the missed calls' */

static void
post_missed (CallNotifications *self, Missed *missed)
{
  const char *with_call_back[] = { "default", "", "call-back", _("Call back"), NULL };
  const char *without[] = { "default", "", NULL };

  /* a missed call peeks (the "Missed calls" channel is high importance) */
  missed->id = notifier_notify (self->notifier, missed->id, _("Missed call"), missed->who,
                                missed->number && *missed->number ? with_call_back : without,
                                NOTIFIER_URGENCY_NORMAL, NULL);
}

void
call_notifications_show_missed (CallNotifications *self, gint64 log_id, const char *who, const char *number)
{
  Missed *missed = g_hash_table_lookup (self->missed, &log_id);

  if (!missed)
    {
      gint64 *key = g_new (gint64, 1);

      *key = log_id;
      missed = g_new0 (Missed, 1);
      g_hash_table_insert (self->missed, key, missed);
    }
  g_free (missed->who);
  g_free (missed->number);
  missed->who = g_strdup (who);
  missed->number = g_strdup (number);
  post_missed (self, missed);
}

void
call_notifications_clear_missed (CallNotifications *self, gint64 log_id)
{
  Missed *missed = g_hash_table_lookup (self->missed, &log_id);

  if (!missed)
    return;
  notifier_close (self->notifier, missed->id);
  g_hash_table_remove (self->missed, &log_id);
}

void
call_notifications_clear_all_missed (CallNotifications *self)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->missed);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    notifier_close (self->notifier, ((Missed *) value)->id);
  g_hash_table_remove_all (self->missed);
}

/* --- what the user did */

static void
on_action (guint32 id, const char *action, gpointer data)
{
  CallNotifications *self = data;
  GHashTableIter iter;
  gpointer key, value;

  if (id && id == self->call_notification)
    {
      if (g_str_equal (action, "closed") || g_str_equal (action, "gone"))
        {
          /* an ongoing call's notification should not be swiped away; here it went, so it is gone */
          self->call_notification = 0;
          return;
        }
      self->callback (FALSE, self->call_id, g_str_equal (action, "default") ? "open" : action, self->data);
      return;
    }
  g_hash_table_iter_init (&iter, self->missed);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      Missed *missed = value;
      gint64 log_id = *(gint64 *) key;

      if (missed->id != id)
        continue;
      if (g_str_equal (action, "gone"))
        {
          missed->id = 0;
          return;
        }
      /* tapped or swiped away, it is seen and goes; Call back leaves it as it is until the call log is opened */
      if (!g_str_equal (action, "call-back"))
        g_hash_table_iter_remove (&iter);
      if (g_str_equal (action, "default"))
        notifier_close (self->notifier, id);
      self->callback (TRUE, log_id, g_str_equal (action, "default") ? "open" : action, self->data);
      return;
    }
}

/* the notification server came back: what was shown is shown again */
static void
on_restart (gpointer data)
{
  CallNotifications *self = data;
  GHashTableIter iter;
  gpointer value;

  if (self->call_notification)
    {
      self->call_notification = 0;
      post_call (self);
    }
  g_hash_table_iter_init (&iter, self->missed);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      ((Missed *) value)->id = 0;
      post_missed (self, value);
    }
}

CallNotifications *
call_notifications_new (CallNotificationAction callback, gpointer data)
{
  CallNotifications *self = g_new0 (CallNotifications, 1);

  self->callback = callback;
  self->data = data;
  self->missed = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) missed_free);
  self->notifier = notifier_new (on_action, on_restart, self);
  return self;
}

void
call_notifications_free (CallNotifications *self)
{
  notifier_free (self->notifier);
  g_hash_table_unref (self->missed);
  g_free (self->title);
  g_free (self);
}
