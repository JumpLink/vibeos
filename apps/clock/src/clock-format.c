/* How the Clock writes times, days and durations: see clock-format.h. */
#include "clock-format.h"

#include <glib/gi18n.h>
#include <langinfo.h>
#include <string.h>

GDateWeekday
clock_week_start (GSettings *settings)
{
  /* the setting's values are java.util.Calendar's: SUNDAY 1, MONDAY 2, SATURDAY 7; 0 is "the locale's" */
  int day = g_settings_get_enum (settings, "week-start");

  if (day == 1)
    return G_DATE_SUNDAY;
  if (day == 2)
    return G_DATE_MONDAY;
  if (day == 7)
    return G_DATE_SATURDAY;

#ifdef _NL_TIME_FIRST_WEEKDAY
  /* glibc's locale data, read as GtkCalendar reads it: the first weekday counted from the week's origin
   * (19971130 is a Sunday, 19971201 a Monday) */
  {
    union { unsigned int word; char *string; } info;
    int origin_is_monday, first;

    info.string = nl_langinfo (_NL_TIME_FIRST_WEEKDAY);
    first = info.string[0];
    info.string = nl_langinfo (_NL_TIME_WEEK_1STDAY);
    origin_is_monday = info.word == 19971201;
    /* 0 Sunday .. 6 Saturday */
    day = (origin_is_monday + first - 1) % 7;
    return day == 0 ? G_DATE_SUNDAY : (GDateWeekday) day;
  }
#else
  return G_DATE_MONDAY;
#endif
}

char *
clock_time_text (int hour, int minute, gboolean use_24h)
{
  if (use_24h)
    return g_strdup_printf ("%02d:%02d", hour, minute);
  return g_strdup_printf ("%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
}

char *
clock_time_markup (int hour, int minute, gboolean use_24h, double ampm_ratio)
{
  if (use_24h)
    return g_strdup_printf ("%02d:%02d", hour, minute);
  /* the space before AM/PM is a hair space (U+200A), as Utils.get12ModeFormat makes it */
  return g_strdup_printf ("%d:%02d <span size='%d%%' weight='900'>%s</span>", hour % 12 ? hour % 12 : 12,
                          minute, (int) (ampm_ratio * 100), hour < 12 ? "AM" : "PM");
}

char *
clock_alarm_time_text (GDateTime *time, gboolean use_24h)
{
  return g_date_time_format (time, use_24h ? "%a %H:%M" : "%a %-l:%M %p");
}

char *
clock_alarm_set_text (gint64 seconds_until)
{
  g_autofree char *d = NULL, *h = NULL, *m = NULL;
  gint64 minutes, hours, days;

  if (seconds_until < 60)
    return g_strdup (_("Alarm set for less than 1 minute from now."));
  /* rounded up to whole minutes, so that "less than 1 minute" never shows as "0 minutes" */
  minutes = (seconds_until + 59) / 60;
  hours = minutes / 60;
  minutes %= 60;
  days = hours / 24;
  hours %= 24;

  d = g_strdup_printf (ngettext ("1 day", "%" G_GINT64_FORMAT " days", days), days);
  h = g_strdup_printf (ngettext ("1 hour", "%" G_GINT64_FORMAT " hours", hours), hours);
  m = g_strdup_printf (ngettext ("1 minute", "%" G_GINT64_FORMAT " minutes", minutes), minutes);
  /* the alarm_set sentences of strings.xml, picked by which of days, hours and minutes are there */
  switch ((days > 0) | (hours > 0) << 1 | (minutes > 0) << 2)
    {
    case 1: return g_strdup_printf (_("Alarm set for %s from now."), d);
    case 2: return g_strdup_printf (_("Alarm set for %s from now."), h);
    case 3: return g_strdup_printf (_("Alarm set for %s and %s from now."), d, h);
    case 4: return g_strdup_printf (_("Alarm set for %s from now."), m);
    case 5: return g_strdup_printf (_("Alarm set for %s and %s from now."), d, m);
    case 6: return g_strdup_printf (_("Alarm set for %s and %s from now."), h, m);
    default: return g_strdup_printf (_("Alarm set for %s, %s, and %s from now."), d, h, m);
    }
}

char *
clock_weekday_letter (int index)
{
  /* 2024-01-01 was a Monday; the first character of the short name stands for Android's narrow one */
  g_autoptr(GDateTime) day = g_date_time_new_utc (2024, 1, 1 + index, 12, 0, 0);
  g_autofree char *name = g_date_time_format (day, "%a");

  return g_utf8_substring (name, 0, 1);
}

/* a sound file's name without its folder and extension, as the media scanner titles it */
static char *
sound_name (const char *uri)
{
  g_autoptr(GFile) file = g_file_new_for_uri (uri);
  g_autofree char *base = g_file_get_basename (file);
  char *dot = base ? strrchr (base, '.') : NULL;

  if (dot)
    *dot = '\0';
  return g_strdup (base ? base : uri);
}

char *
clock_ringtone_title (const char *ringtone, GSettings *settings)
{
  g_autofree char *default_sound = NULL, *name = NULL;

  if (ringtone && !*ringtone)
    return g_strdup (_("Silent"));
  if (ringtone)
    return sound_name (ringtone);
  /* the default: Android titles content://settings/system/alarm_alert "Default (Hassium)" */
  default_sound = g_settings_get_string (settings, "default-alarm-ringtone");
  name = *default_sound ? sound_name (default_sound) : g_strdup ("Hassium");
  return g_strdup_printf (_("Default (%s)"), name);
}
