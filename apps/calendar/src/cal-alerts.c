/* When reminders are due (cal-alerts.h). */
#include "cal-alerts.h"

static char *
digest (const char *text)
{
  /* 16 hex digits of SHA-1: unique enough among one phone's reminders, and any UID becomes a plain key */
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, text, -1);

  return g_strndup (sum, 16);
}

char *
cal_alert_occurrence_key (const CalEvent *event)
{
  g_autofree char *text = g_strdup_printf ("%s\n%s\n%s\n%" G_GINT64_FORMAT, event->calendar ? event->calendar : "",
                                           event->uid ? event->uid : "", event->rid ? event->rid : "", event->start);

  return digest (text);
}

char *
cal_alert_key (const CalEvent *event, int minutes)
{
  g_autofree char *occurrence = cal_alert_occurrence_key (event);
  g_autofree char *text = g_strdup_printf ("%s/%d", occurrence, minutes);

  return digest (text);
}

gboolean
cal_alert_notifies (const CalReminder *reminder)
{
  return reminder->method == CAL_METHOD_NOTIFICATION || reminder->method == CAL_METHOD_ALARM;
}

void
cal_alert_free (CalAlert *alert)
{
  cal_event_unref (alert->event);
  g_ptr_array_unref (alert->keys);
  g_free (alert);
}

void
cal_alerts_compute (GPtrArray *events, GHashTable *fired, gint64 now, GPtrArray *due, gint64 *next)
{
  g_autoptr(GHashTable) latest = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  *next = 0;
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];

      for (guint r = 0; event->reminders && r < event->reminders->len; r++)
        {
          const CalReminder *reminder = &g_array_index (event->reminders, CalReminder, r);
          gint64 time = event->start - (gint64) reminder->minutes * 60;
          g_autofree char *key = NULL;

          if (!cal_alert_notifies (reminder) || reminder->minutes < 0)
            continue;
          if (time > now)
            {
              if (!*next || time < *next)
                *next = time;
              continue;
            }
          key = cal_alert_key (event, reminder->minutes);
          /* notified already; or too late, the event is over (AlertService drops those) */
          if (g_hash_table_contains (fired, key) || MAX (event->end, event->start) <= now)
            continue;
          {
            char *occurrence = cal_alert_occurrence_key (event);
            CalAlert *before = g_hash_table_lookup (latest, occurrence);

            if (before)
              {
                /* the same occurrence: one notification, for its latest reminder */
                if (time > before->time)
                  {
                    before->minutes = reminder->minutes;
                    before->time = time;
                  }
                g_ptr_array_add (before->keys, g_steal_pointer (&key));
                g_free (occurrence);
                continue;
              }
            before = g_new0 (CalAlert, 1);
            before->event = cal_event_ref (event);
            before->minutes = reminder->minutes;
            before->time = time;
            before->keys = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (before->keys, g_steal_pointer (&key));
            g_hash_table_insert (latest, occurrence, before);
            g_ptr_array_add (due, before);
          }
        }
    }
}
