/* The calendar's dates and times as text (cal-format.h). */
#include "cal-format.h"
#include <time.h>
#include "cal-model.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <langinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vibeos-locale.h>


gboolean
cal_format_uses_24h (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                              "org.gnome.desktop.interface", TRUE);
  g_autoptr(GSettings) settings = NULL;
  g_autofree char *format = NULL;

  /* no desktop schema (a test): 24 hours, as the phone */
  if (!schema)
    return TRUE;
  g_settings_schema_unref (schema);
  settings = g_settings_new ("org.gnome.desktop.interface");
  format = g_settings_get_string (settings, "clock-format");
  return g_strcmp0 (format, "12h") != 0;
}

static GDateTime *
local_time (gint64 time)
{
  return g_date_time_new_from_unix_local (time);
}

static GDateTime *
local_day (int day)
{
  return g_date_time_new_from_unix_local (cal_day_start (day));
}

char *
cal_format_time (gint64 time)
{
  g_autoptr(GDateTime) local = local_time (time);

  /* in the user's language, by the clock setting: "14:05", "2:05 PM" */
  return vibeos_format_date (local, cal_format_uses_24h () ? "Hm" : "hm");
}

/* from @start to @end in the fields @skeleton names, as the language writes a range (vibeos-locale.h) */
static char *
format_range (gint64 start, gint64 end, const char *skeleton)
{
  g_autoptr(GDateTime) a = local_time (start), b = local_time (end);

  return vibeos_format_interval (a, b, skeleton);
}

char *
cal_format_time_range (gint64 start, gint64 end)
{
  if (end <= start)
    return cal_format_time (start);
  /* "10:00 – 11:30", "10:00–11:30 Uhr" */
  return format_range (start, end, cal_format_uses_24h () ? "Hm" : "hm");
}

/* the day in the fields @skeleton names (vibeos-locale.h): in the user's language, in the order of the Formats region */
static char *
format_day (int day, const char *skeleton)
{
  g_autoptr(GDateTime) local = local_day (day);

  return vibeos_format_date (local, skeleton);
}

char *
cal_format_month_year (int day)
{
  /* the toolbar of the Week and Month views, "November 2026" */
  return format_day (day, "yMMMM");
}

char *
cal_format_full_date (int day)
{
  /* "November 7, 2026" (the date with the year) */
  return format_day (day, "yMMMMd");
}

char *
cal_format_month_day (int day)
{
  /* "November 7" */
  return format_day (day, "MMMMd");
}

char *
cal_format_weekday (int day)
{
  return format_day (day, "EEEE");
}

char *
cal_format_weekday_short (int day)
{
  return format_day (day, "EEE");
}

char *
cal_format_editor_date (int day)
{
  /* "Sun, Nov 1, 2026" (weekday, month, day, year; abbreviated) */
  return format_day (day, "yMMMEd");
}

char *
cal_format_picker_date (int day)
{
  /* the date picker's header, "Nov 1, 2026" */
  return format_day (day, "yMMMd");
}

char *
cal_format_relative_weekday (int day, int today)
{
  g_autofree char *weekday = cal_format_weekday (day);

  /* today, yesterday or tomorrow, then the weekday */
  if (day == today)
    return g_strdup_printf (_("Today, %s"), weekday);
  if (day == today - 1)
    return g_strdup_printf (_("Yesterday, %s"), weekday);
  if (day == today + 1)
    return g_strdup_printf (_("Tomorrow, %s"), weekday);
  return g_steal_pointer (&weekday);
}

/* "Thursday, April 12" (the year only when it is not this one) */
static char *
weekday_date (int day, int today)
{
  g_autoptr(GDateTime) a = local_day (day), b = local_day (today);

  if (g_date_time_get_year (a) == g_date_time_get_year (b))
    return vibeos_format_date (a, "MMMMEEEEd");
  return vibeos_format_date (a, "yMMMMEEEEd");
}

char *
cal_format_event_when (gint64 start, gint64 end, gboolean all_day, gint64 now)
{
  int today = cal_day_number (now), first = cal_day_number (start);
  int last = end > start ? cal_day_number (end - 1) : first;

  if (all_day)
    {
      /* single day: "Today" / "Tomorrow" when it is, else the date; several: the range of dates */
      if (first == last && first == today)
        return g_strdup (_("Today"));
      if (first == last && first == today + 1)
        return g_strdup (_("Tomorrow"));
      if (first == last)
        return weekday_date (first, today);
      {
        g_autoptr(GDateTime) a = local_day (first), b = local_day (last), now_day = local_day (today);

        /* the year only when the range does not lie in this one */
        return vibeos_format_interval (a, b, g_date_time_get_year (a) == g_date_time_get_year (now_day)
                                             && g_date_time_get_year (b) == g_date_time_get_year (now_day)
                                             ? "MMMMEEEEd" : "yMMMMEEEEd");
      }
    }
  /* a timed event within one day: "Today at 10:00 – 11:00", "Thursday, April 12, 10:00 – …" */
  if (first == last)
    {
      g_autofree char *times = cal_format_time_range (start, end);

      if (first == today)
        return g_strdup_printf (_("Today at %s"), times);
      if (first == today + 1)
        return g_strdup_printf (_("Tomorrow at %s"), times);
      {
        g_autofree char *date = weekday_date (first, today);

        /* the date, then the times */
        return g_strdup_printf (_("%s, %s"), date, times);
      }
    }
  /* over several days: "Fri, Apr 6, 17:00 – Sun, Apr 8, 18:00" */
  return format_range (start, end, cal_format_uses_24h () ? "MMMEdHm" : "MMMEdhm");
}

char *
cal_format_agenda_when (gint64 start, gint64 end, gboolean all_day)
{
  if (all_day)
    {
      int first = cal_day_number (start), last = end > start ? cal_day_number (end - 1) : first;

      if (first == last)
        return cal_format_month_day (first);
      {
        /* "November 7 – 9", "7.–9. November", "November 30 – December 2" */
        g_autoptr(GDateTime) x = local_day (first), y = local_day (last);

        return vibeos_format_interval (x, y, "MMMMd");
      }
    }
  if (cal_day_number (start) != cal_day_number (end > start ? end - 1 : start))
    {
      /* over midnight: the dates added, "Nov 9, 22:00 – Nov 10, 02:00" */
      return format_range (start, end, cal_format_uses_24h () ? "MMMdHm" : "MMMdhm");
    }
  return cal_format_time_range (start, end);
}

char *
cal_format_reminder_text (gint64 start, gboolean all_day, const char *location, gint64 now)
{
  int today = cal_day_number (now), day = cal_day_number (start);
  GString *text = g_string_new (NULL);
  g_autofree char *trimmed = location ? g_strstrip (g_strdup (location)) : NULL;

  /* abbreviated; the time unless all-day; the date unless today or tomorrow */
  if (day < today || day > today + 1)
    {
      g_autoptr(GDateTime) local = local_time (start);
      /* "Nov 9" */
      g_autofree char *date = vibeos_format_date (local, "MMMd");

      g_string_append (text, date);
      if (!all_day)
        g_string_append (text, ", ");
    }
  if (!all_day)
    {
      g_autofree char *time = cal_format_time (start);

      g_string_append (text, time);
    }
  if (day == today + 1)
    {
      if (text->len)
        g_string_append (text, ", ");
      g_string_append (text, _("Tomorrow"));
    }
  if (trimmed && *trimmed)
    {
      if (text->len)
        g_string_append (text, ", ");
      g_string_append (text, trimmed);
    }
  return g_string_free (text, FALSE);
}

char *
cal_format_reminder_minutes (int minutes)
{
  /* weeks, days or hours when it divides evenly, else minutes */
  if (minutes != 0 && minutes % (7 * 24 * 60) == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d week", "%d weeks", minutes / (7 * 24 * 60)),
                            minutes / (7 * 24 * 60));
  if (minutes != 0 && minutes % (24 * 60) == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d day", "%d days", minutes / (24 * 60)), minutes / (24 * 60));
  if (minutes != 0 && minutes % 60 == 0)
    return g_strdup_printf (g_dngettext (NULL, "%d hour", "%d hours", minutes / 60), minutes / 60);
  return g_strdup_printf (g_dngettext (NULL, "%d minute", "%d minutes", minutes), minutes);
}


int
cal_week_start (int setting)
{
  /* the setting's values are Sunday 1 … Saturday 7 */
  if (setting >= 1 && setting <= 7)
    return setting - 1;
#ifdef HAVE__NL_TIME_FIRST_WEEKDAY
  {
    /* GtkCalendar's reading of glibc's locale: the week's origin (a Sunday or a Monday) and the first weekday
     * counted from it */
    union { unsigned int word; char *string; } info;
    int origin = 0, first;

    info.string = nl_langinfo (_NL_TIME_FIRST_WEEKDAY);
    first = info.string[0];
    info.string = nl_langinfo (_NL_TIME_WEEK_1STDAY);
    if (info.word == 19971201)
      origin = 1;
    return ((origin + first - 1) % 7 + 7) % 7;
  }
#else
  return 0;
#endif
}

/* --- repeat rules in words */

static const char *byday_names[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

/* "on every [first … fourth, last] [weekday]", one string each for the languages whose
 * words change with the weekday's gender: the repeat dialog's choice, and a monthly rule's words */
static const char *const repeat_by_nth[7][5] = {
  { N_("on every first Sunday"), N_("on every second Sunday"), N_("on every third Sunday"),
    N_("on every fourth Sunday"), N_("on every last Sunday") },
  { N_("on every first Monday"), N_("on every second Monday"), N_("on every third Monday"),
    N_("on every fourth Monday"), N_("on every last Monday") },
  { N_("on every first Tuesday"), N_("on every second Tuesday"), N_("on every third Tuesday"),
    N_("on every fourth Tuesday"), N_("on every last Tuesday") },
  { N_("on every first Wednesday"), N_("on every second Wednesday"), N_("on every third Wednesday"),
    N_("on every fourth Wednesday"), N_("on every last Wednesday") },
  { N_("on every first Thursday"), N_("on every second Thursday"), N_("on every third Thursday"),
    N_("on every fourth Thursday"), N_("on every last Thursday") },
  { N_("on every first Friday"), N_("on every second Friday"), N_("on every third Friday"),
    N_("on every fourth Friday"), N_("on every last Friday") },
  { N_("on every first Saturday"), N_("on every second Saturday"), N_("on every third Saturday"),
    N_("on every fourth Saturday"), N_("on every last Saturday") },
};

const char *
cal_format_nth_weekday (int weekday, int nth)
{
  return _(repeat_by_nth[CLAMP (weekday, 0, 6)][CLAMP (nth, 1, 5) - 1]);
}

static char *
weekday_name (int weekday, gboolean abbreviated)
{
  /* 2023-01-01 was a Sunday */
  g_autoptr(GDateTime) date = g_date_time_new_local (2023, 1, 1 + weekday, 12, 0, 0);

  return vibeos_format_date (date, abbreviated ? "EEE" : "EEEE");
}

char *
cal_format_rrule (const char *rrule, gint64 start)
{
  g_auto(GStrv) parts = NULL;
  const char *freq = NULL, *until = NULL;
  int interval = 1, count = 0, n_days = 0, days[7] = { 0 };
  gboolean nth_byday = FALSE;
  g_autoptr(GString) end = g_string_new (NULL);
  g_autoptr(GDateTime) first = g_date_time_new_from_unix_local (start);
  char *text = NULL;

  if (!rrule || !*rrule)
    return NULL;
  parts = g_strsplit (rrule, ";", -1);
  for (int i = 0; parts[i]; i++)
    {
      char *value = strchr (parts[i], '=');

      if (!value)
        continue;
      *value++ = '\0';
      if (g_str_equal (parts[i], "FREQ"))
        freq = value;
      else if (g_str_equal (parts[i], "INTERVAL"))
        interval = MAX (1, atoi (value));
      else if (g_str_equal (parts[i], "COUNT"))
        count = atoi (value);
      else if (g_str_equal (parts[i], "UNTIL"))
        until = value;
      else if (g_str_equal (parts[i], "BYDAY"))
        {
          g_auto(GStrv) list = g_strsplit (value, ",", -1);

          for (int j = 0; list[j] && n_days < 7; j++)
            {
              const char *code = list[j];

              /* "1MO", "-1FR": the nth weekday of a month */
              if (*code == '+' || *code == '-' || g_ascii_isdigit (*code))
                nth_byday = TRUE;
              while (*code == '+' || *code == '-' || g_ascii_isdigit (*code))
                code++;
              for (int k = 0; k < 7; k++)
                if (g_str_equal (code, byday_names[k]))
                  days[n_days++] = k;
            }
        }
    }
  if (!freq)
    return NULL;

  /* the end: "; until 12/31/2026" (a numeric date), "; for 5 times" */
  if (until)
    {
      int y = 0, m = 0, d = 0, h = 0, mi = 0, sec = 0;
      g_autoptr(GDateTime) date = NULL;

      /* parsed, then shown in the app's zone: a UTC end ("…T225959Z") is the local date it falls on */
      if (strlen (until) == 16 && until[15] == 'Z'
          && sscanf (until, "%4d%2d%2dT%2d%2d%2d", &y, &m, &d, &h, &mi, &sec) == 6)
        {
          g_autoptr(GDateTime) utc = g_date_time_new_utc (y, m, d, h, mi, sec);

          date = utc ? g_date_time_to_local (utc) : NULL;
        }
      else if (sscanf (until, "%4d%2d%2d", &y, &m, &d) == 3)
        date = g_date_time_new_local (y, m, d, 12, 0, 0);

      if (date)
        {
          /* a repeat rule's end date, numeric */
          g_autofree char *text_date = vibeos_format_date (date, "yMd");

          g_string_append_printf (end, _("; until %s"), text_date);
        }
    }
  if (count > 0)
    g_string_append_printf (end, g_dngettext (NULL, "; for one time", "; for %d times", count), count);

  if (g_str_equal (freq, "DAILY") && interval == 1)
    text = g_strdup (_("Daily"));
  else if (g_str_equal (freq, "DAILY"))
    text = g_strdup_printf (g_dngettext (NULL, "Every %d day", "Every %d days", interval), interval);
  else if (g_str_equal (freq, "WEEKLY"))
    {
      g_autoptr(GString) names = g_string_new (NULL);
      gboolean weekdays = n_days == 5 && interval == 1;

      for (int k = 0; k < n_days && weekdays; k++)
        weekdays = days[k] == k + 1;
      if (weekdays)
        return g_strconcat (_("Every weekday (Mon–Fri)"), end->str, NULL);
      if (n_days == 0)
        {
          /* no BYDAY: the first occurrence's weekday */
          days[0] = g_date_time_get_day_of_week (first) % 7;
          n_days = 1;
        }
      for (int k = 0; k < n_days; k++)
        {
          g_autofree char *name = weekday_name (days[k], n_days > 1);

          if (k)
            g_string_append (names, ", ");
          g_string_append (names, name);
        }
      if (interval == 1)
        text = g_strdup_printf (_("Weekly on %s"), names->str);
      else
        text = g_strdup_printf (g_dngettext (NULL, "Every %d week on %s", "Every %d weeks on %s", interval), interval,
                                names->str);
    }
  else if (g_str_equal (freq, "MONTHLY"))
    {
      if (nth_byday && n_days == 1)
        {
          int week = (g_date_time_get_day_of_month (first) - 1) / 7;
          const char *nth = cal_format_nth_weekday (g_date_time_get_day_of_week (first) % 7, MIN (week, 4) + 1);

          /* TRANSLATORS: a monthly repeat by the weekday, the %s the repeat dialog's words for it ("on every first
           * Monday"): "Monthly (on every first Monday)" */
          text = g_strdup_printf (_("Monthly (%s)"), nth);
        }
      else
        text = g_strdup (_("Monthly"));
    }
  else if (g_str_equal (freq, "YEARLY"))
    text = g_strdup (_("Yearly"));
  else
    return NULL;
  {
    char *full = g_strconcat (text, end->str, NULL);

    g_free (text);
    return full;
  }
}

/* --- the home time zone */

static char *phone_zone, *phone_tz_env;
static gboolean phone_read;

static void
read_phone_zone (void)
{
  g_autoptr(GTimeZone) local = NULL;

  if (phone_read)
    return;
  phone_read = TRUE;
  phone_tz_env = g_strdup (g_getenv ("TZ"));
  local = g_time_zone_new_local ();
  phone_zone = g_strdup (g_time_zone_get_identifier (local));
}

const char *
cal_phone_zone (void)
{
  read_phone_zone ();
  return phone_zone;
}

void
cal_set_home_zone (const char *tzid)
{
  read_phone_zone ();
  if (tzid && *tzid)
    g_setenv ("TZ", tzid, TRUE);
  else if (phone_tz_env)
    g_setenv ("TZ", phone_tz_env, TRUE);
  else
    g_unsetenv ("TZ");
  tzset ();
}

int
cal_week_number (int day, int week_start)
{
  int first = day - (cal_day_of_week (day) - week_start + 7) % 7;
  int thursday = first + (4 - week_start + 7) % 7;
  g_autoptr(GDateTime) time = g_date_time_new_from_unix_local (cal_day_start (thursday) + 12 * 3600);

  return g_date_time_get_week_of_year (time);
}

char *
cal_format_week_number (int week)
{
  return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "Week %d", "Week %d", week), week);
}
