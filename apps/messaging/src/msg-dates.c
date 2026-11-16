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

/* the time of day, as the system clock is set: "14:05" or "2:05 PM" */
static char *
time_of_day (GDateTime *date)
{
  return g_date_time_format (date, vibeos_clock_uses_24h () ? C_("time", "%H:%M") : C_("time", "%-l:%M %p"));
}

static char *
timestamp (gint64 time, gint64 now, gboolean abbreviated)
{
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local (time / 1000);
  g_autoptr(GDateTime) today = g_date_time_new_from_unix_local (now / 1000);
  gint64 age = now - time;
  gboolean use_24h = vibeos_clock_uses_24h ();
  g_autofree char *clock = NULL;

  if (!date || !today)
    return g_strdup ("");
  if (age < MINUTE_MS)
    return g_strdup (abbreviated ? _("Just now") : _("Now"));
  if (age < HOUR_MS)
    {
      int minutes = age / MINUTE_MS;

      return g_strdup_printf (ngettext ("%d min", "%d mins", minutes), minutes);
    }
  if (same_day (date, today))
    return time_of_day (date);
  if (abbreviated)
    {
      if (age < WEEK_MS)
        return g_date_time_format (date, C_("conversation date", "%a"));
      if (age < YEAR_MS)
        return g_date_time_format (date, C_("conversation date", "%b %-d"));
      return g_date_time_format (date, C_("conversation date", "%x"));
    }
  /* a message's: the day, and when in it ("EEE h:mmaa", "MMM d, h:mmaa", "M/d/yy, h:mmaa") */
  clock = g_date_time_format (date, use_24h ? C_("time", "%H:%M") : C_("message time", "%-l:%M%p"));
  if (age < WEEK_MS)
    {
      g_autofree char *day = g_date_time_format (date, C_("message date", "%a"));

      return g_strdup_printf (C_("message date, time", "%s %s"), day, clock);
    }
  else
    {
      g_autofree char *day = g_date_time_format (date, age < YEAR_MS ? C_("message date", "%b %-d")
                                                                      : C_("message date", "%x"));

      return g_strdup_printf (C_("message date, time", "%s, %s"), day, clock);
    }
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
  g_autofree char *day = date ? g_date_time_format (date, C_("message details date", "%B %-d, %Y")) : NULL;
  g_autofree char *clock = date ? time_of_day (date) : NULL;

  return date ? g_strdup_printf (C_("message date, time", "%s, %s"), day, clock) : g_strdup ("");
}
