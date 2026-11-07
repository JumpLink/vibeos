/* The alarm notifications (alarm-notifications.h). */
#include "alarm-notifications.h"
#include "clock-notifier.h"

#include <glib/gi18n.h>

typedef struct {
  guint32 id;                   /* the server's id for it; 0 while it is not up */
  AlarmInstance *instance;      /* what it shows: shown again if the server restarts */
} Shown;

struct _AlarmNotifications {
  ClockNotifier *notifier;
  AlarmNotificationCallback callback;
  gpointer data;
  GHashTable *shown;            /* instance id -> Shown */
};

static void
shown_free (Shown *shown)
{
  alarm_instance_free (shown->instance);
  g_free (shown);
}

char *
alarm_format_time (GDateTime *time)
{
  g_autoptr(GSettings) interface = g_settings_new ("org.gnome.desktop.interface");

  /* the skeletons "EHm" and "Ehma" */
  if (g_settings_get_enum (interface, "clock-format") == 0 /* 24h */)
    return g_date_time_format (time, "%a %H:%M");
  return g_date_time_format (time, "%a %-l:%M %p");
}

/* AlarmInstance.getLabelOrDefault */
static const char *
label_or_default (const AlarmInstance *instance)
{
  return instance->label && *instance->label ? instance->label : _("Alarm");
}

/* AlarmUtils.getAlarmText: the time, and " - label" if it has one */
static char *
alarm_text (const AlarmInstance *instance, gboolean include_label)
{
  g_autoptr(GDateTime) time = alarm_instance_time (instance);
  g_autofree char *text = alarm_format_time (time);

  if (!include_label || !instance->label || !*instance->label)
    return g_steal_pointer (&text);
  return g_strdup_printf ("%s - %s", text, instance->label);
}

void
alarm_notifications_show (AlarmNotifications *self, const AlarmInstance *instance)
{
  /* actions: key, label pairs; "default" is a tap on the notification */
  const char *upcoming_actions[] = { "default", "", "dismiss", _("Dismiss"), NULL };
  const char *ringing_actions[] = { "default", "", "snooze", _("Snooze"), "dismiss", _("Dismiss"), NULL };
  const char *missed_actions[] = { "default", "", NULL };
  Shown *shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance->id));
  g_autofree char *title = NULL, *body = NULL;
  const char * const *actions;
  ClockUrgency urgency;

  /* Android's channels are "Upcoming" and "Snoozing" at default importance: in the list, no heads-up. Here a
   * normal notification peeks, so these are low ones (under "Silent" in the shade) */
  switch (instance->state)
    {
    case ALARM_STATE_LOW_NOTIFICATION:
    case ALARM_STATE_HIGH_NOTIFICATION:
      title = g_strdup (_("Upcoming alarm"));
      body = alarm_text (instance, TRUE);
      actions = upcoming_actions;
      urgency = CLOCK_URGENCY_LOW;
      break;
    case ALARM_STATE_SNOOZE:
      {
        g_autoptr(GDateTime) time = alarm_instance_time (instance);
        g_autofree char *until = alarm_format_time (time);

        title = g_strdup (label_or_default (instance));
        body = g_strdup_printf (_("Snoozing until %s"), until);
        actions = upcoming_actions;
        urgency = CLOCK_URGENCY_LOW;
        break;
      }
    case ALARM_STATE_FIRED:
      title = g_strdup (label_or_default (instance));
      body = alarm_text (instance, FALSE);
      actions = ringing_actions;
      urgency = CLOCK_URGENCY_CRITICAL;       /* the firing channel: it peeks, and stays while it rings */
      break;
    case ALARM_STATE_MISSED:
      title = g_strdup (_("Missed alarm"));
      body = alarm_text (instance, TRUE);
      actions = missed_actions;
      urgency = CLOCK_URGENCY_NORMAL;         /* the missed channel is high importance: it peeks */
      break;
    default:
      /* silent, hidden, dismissed: none */
      alarm_notifications_clear (self, instance->id);
      return;
    }

  if (!shown)
    {
      shown = g_new0 (Shown, 1);
      g_hash_table_insert (self->shown, GUINT_TO_POINTER (instance->id), shown);
    }
  else
    alarm_instance_free (shown->instance);
  shown->instance = alarm_instance_copy (instance);
  shown->id = clock_notifier_notify (self->notifier, shown->id, title, body, actions, urgency, NULL);
}

void
alarm_notifications_clear (AlarmNotifications *self, guint instance_id)
{
  Shown *shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance_id));

  if (!shown)
    return;
  clock_notifier_close (self->notifier, shown->id);
  g_hash_table_remove (self->shown, GUINT_TO_POINTER (instance_id));
}

/* the instance a notification of ours shows, or 0 */
static guint
instance_of (AlarmNotifications *self, guint32 id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (((Shown *) value)->id == id)
      return GPOINTER_TO_UINT (key);
  return 0;
}

static void
on_action (guint32 id, const char *action, gpointer data)
{
  AlarmNotifications *self = data;
  guint instance_id = instance_of (self, id);

  if (!instance_id)
    return;
  if (g_str_equal (action, "closed") || g_str_equal (action, "gone"))
    {
      Shown *shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance_id));

      /* it is gone either way: a new one for the instance is a new notification */
      shown->id = 0;
      if (g_str_equal (action, "gone"))
        return;
    }
  self->callback (instance_id, g_str_equal (action, "default") ? "open" : action, self->data);
}

/* the notification server came back: what was shown is shown again */
static void
on_restart (gpointer data)
{
  AlarmNotifications *self = data;
  g_autoptr(GList) shown = g_hash_table_get_values (self->shown);
  g_autoptr(GPtrArray) copies = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_instance_free);

  for (GList *l = shown; l; l = l->next)
    {
      ((Shown *) l->data)->id = 0;
      g_ptr_array_add (copies, alarm_instance_copy (((Shown *) l->data)->instance));
    }
  for (guint i = 0; i < copies->len; i++)
    alarm_notifications_show (self, copies->pdata[i]);
}

AlarmNotifications *
alarm_notifications_new (AlarmNotificationCallback callback, gpointer data)
{
  AlarmNotifications *self = g_new0 (AlarmNotifications, 1);

  self->callback = callback;
  self->data = data;
  self->shown = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) shown_free);
  self->notifier = clock_notifier_new (on_action, on_restart, self);
  return self;
}

void
alarm_notifications_free (AlarmNotifications *self)
{
  clock_notifier_free (self->notifier);
  g_hash_table_unref (self->shown);
  g_free (self);
}
