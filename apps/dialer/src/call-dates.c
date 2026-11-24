/* When a call was: see call-dates.h. */
#include "call-dates.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define MINUTE_MS (60 * G_GINT64_CONSTANT (1000))
#define HOUR_MS   (60 * MINUTE_MS)
#define DAY_MS    (24 * HOUR_MS)
#define WEEK_MS   (7 * DAY_MS)

/* the number of local calendar days between the two */
static int
day_distance (gint64 date_ms, gint64 now_ms)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local_usec (date_ms * 1000);
  g_autoptr(GDateTime) now = g_date_time_new_from_unix_local_usec (now_ms * 1000);
  GDate a, b;

  g_date_clear (&a, 1);
  g_date_clear (&b, 1);
  g_date_set_dmy (&a, g_date_time_get_day_of_month (date), g_date_time_get_month (date),
                  g_date_time_get_year (date));
  g_date_set_dmy (&b, g_date_time_get_day_of_month (now), g_date_time_get_month (now), g_date_time_get_year (now));
  return ABS (g_date_days_between (&a, &b));
}

DayGroup
call_dates_day_group (gint64 date_ms, gint64 now_ms)
{
  int days = day_distance (date_ms, now_ms);

  return days == 0 ? DAY_TODAY : days == 1 ? DAY_YESTERDAY : DAY_OLDER;
}

const char *
call_dates_day_group_name (DayGroup group)
{
  switch (group)
    {
    case DAY_TODAY:
      return _("Today");
    case DAY_YESTERDAY:
      return _("Yesterday");
    default:
      return _("Older");
    }
}

char *
call_dates_relative (gint64 date_ms, gint64 now_ms)
{
  gint64 duration = ABS (now_ms - date_ms);
  g_autoptr(GDateTime) date = NULL, now = NULL;
  int count;

  /* to the minute: no seconds, so a call under a minute ago is "0 min. ago" */
  if (duration < HOUR_MS)
    {
      count = duration / MINUTE_MS;
      return g_strdup_printf (now_ms >= date_ms ? g_dngettext (NULL, "%d min. ago", "%d min. ago", count)
                                                : g_dngettext (NULL, "In %d min.", "In %d min.", count), count);
    }
  /* "even if it actually happened yesterday": under a day is hours, rounded down */
  if (duration < DAY_MS)
    {
      count = duration / HOUR_MS;
      return g_strdup_printf (now_ms >= date_ms ? g_dngettext (NULL, "%d hr. ago", "%d hr. ago", count)
                                                : g_dngettext (NULL, "In %d hr.", "In %d hr.", count), count);
    }
  if (duration < WEEK_MS)
    {
      count = day_distance (date_ms, now_ms);
      if (count == 0)
        return g_strdup (_("Today"));
      if (count == 1)
        return g_strdup (now_ms >= date_ms ? _("Yesterday") : _("Tomorrow"));
      return g_strdup_printf (now_ms >= date_ms ? g_dngettext (NULL, "%d day ago", "%d days ago", count)
                                                : g_dngettext (NULL, "In %d day", "In %d days", count), count);
    }
  /* a week or more: the date, with the year only when it is not this one, in the language's own form ("October 28",
   * "28. Oktober", "28 октября"; "October 28, 2025") */
  date = g_date_time_new_from_unix_local_usec (date_ms * 1000);
  now = g_date_time_new_from_unix_local_usec (now_ms * 1000);
  return vibeos_format_date (date, g_date_time_get_year (date) == g_date_time_get_year (now) ? "MMMMd" : "yMMMMd");
}

char *
call_dates_details (gint64 date_ms)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local_usec (date_ms * 1000);

  if (!date)
    return g_strdup ("");
  /* the date and time in the language's own form, the hours by the 12/24-hour setting: "Wednesday, November 4, 2026
   * at 04:36", "Mittwoch, 4. November 2026 um 04:36" */
  return vibeos_format_date (date, "yMMMMEEEEdjm");
}

char *
call_dates_duration (gint64 seconds)
{
  if (seconds >= 60)
    /* TRANSLATORS: a call's duration with the abbreviations: "2m 15s" */
    return g_strdup_printf (C_("call duration", "%dm %ds"), (int) (seconds / 60), (int) (seconds % 60));
  /* TRANSLATORS: a call's duration under a minute: "12s" */
  return g_strdup_printf (C_("call duration", "%ds"), (int) seconds);
}
