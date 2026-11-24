/* How a conversation's messages are laid out (msg-thread.h). */
#include "msg-thread.h"

#include <glib/gi18n.h>
#include <vibeos-locale.h>

#define GAP_IN_RUN      2
#define GAP_BETWEEN     16

static gboolean
same_run (const MsgThreadItem *a, const MsgThreadItem *b)
{
  if (!a->outgoing != !b->outgoing)
    return FALSE;
  /* in a group, another sender starts another run */
  return a->outgoing || g_strcmp0 (a->sender, b->sender) == 0;
}

void
msg_thread_layout (const MsgThreadItem *items, guint n, MsgThreadPlace *places)
{
  for (guint i = 0; i < n; i++)
    {
      places[i].separator = i == 0 || items[i].time - items[i - 1].time >= MSG_THREAD_SEPARATOR_GAP_MS;
      places[i].time_under = FALSE;
    }
  for (guint i = 0; i < n; i++)
    {
      gboolean joins_before = i > 0 && !places[i].separator && same_run (&items[i - 1], &items[i]);
      gboolean joins_after = i + 1 < n && !places[i + 1].separator && same_run (&items[i], &items[i + 1]);

      places[i].position = joins_before ? (joins_after ? MSG_RUN_MIDDLE : MSG_RUN_LAST)
                                        : (joins_after ? MSG_RUN_FIRST : MSG_RUN_SINGLE);
    }
  if (n > 0 && !items[n - 1].failed)
    places[n - 1].time_under = TRUE;
}

int
msg_thread_gap_before (const MsgThreadPlace *places, guint index)
{
  if (index == 0 || places[index].separator)
    return 0;
  return places[index].position == MSG_RUN_MIDDLE || places[index].position == MSG_RUN_LAST ? GAP_IN_RUN
                                                                                               : GAP_BETWEEN;
}

char *
msg_thread_time_text (gint64 time, gboolean use_24h)
{
  g_autoptr (GDateTime) date = g_date_time_new_from_unix_local (time / 1000);

  if (!date)
    return g_strdup ("");
  return vibeos_format_date (date, use_24h ? "Hm" : "hm");
}

char *
msg_thread_separator_text (gint64 time, gint64 now, gboolean use_24h)
{
  g_autoptr (GDateTime) date = g_date_time_new_from_unix_local (time / 1000);
  g_autoptr (GDateTime) today = g_date_time_new_from_unix_local (now / 1000);
  g_autofree char *clock = msg_thread_time_text (time, use_24h);
  g_autofree char *day = NULL;
  int days;

  if (!date || !today)
    return g_strdup ("");
  {
    g_autoptr (GDate) a = g_date_new_dmy (g_date_time_get_day_of_month (date), g_date_time_get_month (date),
                                          g_date_time_get_year (date));
    g_autoptr (GDate) b = g_date_new_dmy (g_date_time_get_day_of_month (today), g_date_time_get_month (today),
                                          g_date_time_get_year (today));

    days = g_date_days_between (a, b);
  }
  if (days == 0)
    day = g_strdup (_("Today"));
  else if (days == 1)
    day = g_strdup (_("Yesterday"));
  else
    /* "Thursday, Oct 29", with the year when it is not this one */
    day = vibeos_format_date (date, g_date_time_get_year (date) == g_date_time_get_year (today) ? "MMMEEEEd"
                                                                                                : "yMMMEEEEd");
  /* the day, a bullet, the time */
  return g_strdup_printf (C_("separator", "%s • %s"), day, clock);
}
