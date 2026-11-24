/* When a message was (msg-dates.h). */
#include "msg-dates.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define MINUTE_MS (60 * 1000)
#define HOUR_MS (60 * MINUTE_MS)
#define WEEK_MS (7 * 24 * (gint64) HOUR_MS)
#define YEAR_MS (365 * 24 * (gint64) HOUR_MS)

/* days passed: calendar days, in the local time zone */
static gboolean
same_day (GDateTime *a, GDateTime *b)
{
  return g_date_time_get_year (a) == g_date_time_get_year (b)
         && g_date_time_get_day_of_year (a) == g_date_time_get_day_of_year (b);
}

static char *
timestamp (gint64 time, gint64 now, gboolean abbreviated)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (time / 1000);
  g_autoptr(GDateTime) today = g_date_time_new_from_unix_local (now / 1000);
  gint64 age = now - time;

  if (!date || !today)
    return g_strdup ("");
  if (age < MINUTE_MS)
    return g_strdup (abbreviated ? _("Just now") : _("Now"));
  if (age < HOUR_MS)
    {
      int minutes = age / MINUTE_MS;

      return g_strdup_printf (ngettext ("%d min", "%d mins", minutes), minutes);
    }
  /* the time of day, as the clock is set: "14:05" or "2:05 PM" */
  if (same_day (date, today))
    return vibeos_format_time (date);
  /* a conversation's: the day ("Tue", "Nov 24", "11/24/25") */
  if (abbreviated)
    return vibeos_format_date (date, age < WEEK_MS ? "EEE" : age < YEAR_MS ? "MMMd" : "yMd");
  /* a message's: the day, and when in it ("Tue 2:05 PM", "Nov 24, 2:05 PM", "11/24/25, 2:05 PM") */
  return vibeos_format_date (date, age < WEEK_MS ? "EEEjm" : age < YEAR_MS ? "MMMdjm" : "yMdjm");
}

char *
msg_dates_conversation (gint64 time, gint64 now)
{
  return timestamp (time, now, TRUE);
}

char *
msg_dates_message (gint64 time, gint64 now)
{
  return timestamp (time, now, FALSE);
}

char *
msg_dates_details (gint64 time)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (time / 1000);

  /* "November 24, 2026 at 2:05 PM" */
  return date ? vibeos_format_date (date, "yMMMMdjm") : g_strdup ("");
}
