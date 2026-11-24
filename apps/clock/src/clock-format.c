/* How the Clock writes times, days and durations: see clock-format.h. */
#include "clock-format.h"

#include <glib/gi18n.h>
#include <langinfo.h>
#include <vibeos-locale.h>
#include <stdio.h>
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

#ifdef HAVE__NL_TIME_FIRST_WEEKDAY
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

const char *
clock_day_period (int hour)
{
  static char *am, *pm;

  if (!am)
    vibeos_locale_am_pm (&am, &pm);
  return hour < 12 ? am : pm;
}

char *
clock_time_text (int hour, int minute, gboolean use_24h)
{
  if (use_24h)
    return g_strdup_printf ("%02d:%02d", hour, minute);
  return g_strdup_printf ("%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, clock_day_period (hour));
}

char *
clock_time_markup (int hour, int minute, gboolean use_24h, double ampm_ratio)
{
  if (use_24h)
    return g_strdup_printf ("%02d:%02d", hour, minute);
  /* the space before AM/PM is a hair space (U+200A), as Utils.get12ModeFormat makes it */
  return g_strdup_printf ("%d:%02d <span size='%d%%' weight='900'>%s</span>", hour % 12 ? hour % 12 : 12,
                          minute, (int) (ampm_ratio * 100), clock_day_period (hour));
}

char *
clock_alarm_time_text (GDateTime *time, gboolean use_24h)
{
  return vibeos_format_date (time, use_24h ? "EEEHm" : "EEEhm");
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

  d = g_strdup_printf (ngettext ("%d day", "%d days", days), (int) days);
  h = g_strdup_printf (ngettext ("%d hour", "%d hours", hours), (int) hours);
  m = g_strdup_printf (ngettext ("%d minute", "%d minutes", minutes), (int) minutes);
  /* the "Alarm set for …" sentences, picked by which of days, hours and minutes are there */
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
  /* 2024-01-01 was a Monday; the language's narrow name ("M", in Russian "П") */
  g_autoptr(GDateTime) day = g_date_time_new_utc (2024, 1, 1 + index, 12, 0, 0);

  return vibeos_format_date (day, "EEEEE");
}

/* a sound file's name without its folder and extension, as the media scanner titles it */
/* An Ogg file's TITLE, from its comment header (Vorbis "\x03vorbis", Opus "OpusTags"): a 32-bit little-endian
 * length and the vendor string, a count, then that many length-prefixed "KEY=value" comments. The header is the
 * second packet, on the page after the first, so the file's first 64 KiB hold it. NULL if there is none. */
static char *
ogg_title (const char *path)
{
  static const char *const magics[] = { "\x03vorbis", "OpusTags" };
  g_autofree guint8 *data = g_malloc (65536);
  FILE *stream = fopen (path, "rb");
  size_t length = stream ? fread (data, 1, 65536, stream) : 0;
  const guint8 *end = data + length, *p = NULL;
  guint32 count;

  if (stream)
    fclose (stream);
  for (guint i = 0; i < G_N_ELEMENTS (magics) && !p; i++)
    {
      size_t size = strlen (magics[i]);

      for (const guint8 *at = data; !p && at + size <= end; at++)
        if (memcmp (at, magics[i], size) == 0)
          p = at + size;
    }
#define READ_U32(v) (end - p < 4 ? FALSE : ((v) = p[0] | p[1] << 8 | p[2] << 16 | (guint32) p[3] << 24, p += 4, TRUE))
  if (!p || !READ_U32 (count) || (guint32) (end - p) < count)
    return NULL;
  p += count;                   /* the vendor string */
  if (!READ_U32 (count))
    return NULL;
  for (guint32 i = 0; i < count; i++)
    {
      guint32 size;

      if (!READ_U32 (size) || (guint32) (end - p) < size)
        return NULL;
      if (size > 6 && g_ascii_strncasecmp ((const char *) p, "TITLE=", 6) == 0
          && g_utf8_validate ((const char *) p + 6, size - 6, NULL))
        return g_strndup ((const char *) p + 6, size - 6);
      p += size;
    }
#undef READ_U32
  return NULL;
}

/* A sound's name: the file's own title, or its name without the extension. The
 * titles are kept, since the sound picker names every alarm sound each time it redraws. */
static char *
sound_name (const char *uri)
{
  static GHashTable *titles;
  g_autoptr(GFile) file = g_file_new_for_uri (uri);
  g_autofree char *base = g_file_get_basename (file);
  g_autofree char *path = g_file_get_path (file);
  char *title, *dot;

  if (!titles)
    titles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  title = g_hash_table_lookup (titles, uri);
  if (title)
    return g_strdup (title);
  title = path ? ogg_title (path) : NULL;
  if (!title)
    {
      dot = base ? strrchr (base, '.') : NULL;
      if (dot)
        *dot = '\0';
      title = g_strdup (base ? base : uri);
    }
  g_hash_table_insert (titles, g_strdup (uri), title);
  return g_strdup (title);
}

/* the sound the service rings when no alarm sound is chosen (alarm-klaxon.c): Hassium where the Android alarm sounds
 * are installed, else the Clock's own alarm_expire ("Beep-Beep-Beep Alarm"), so that a phone without them is not told
 * it rings Hassium */
static char *
default_alarm_sound (void)
{
  static const char *const sounds[] = { "sounds/android/alarms/Hassium.ogg", "vibeos-clock/sounds/alarm_expire.ogg" };
  const char *const *dirs = g_get_system_data_dirs ();

  for (guint s = 0; s < G_N_ELEMENTS (sounds); s++)
    for (guint i = 0; dirs[i]; i++)
      {
        g_autofree char *path = g_build_filename (dirs[i], sounds[s], NULL);

        if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
          return g_filename_to_uri (path, NULL, NULL);
      }
  return NULL;
}

char *
clock_ringtone_title (const char *ringtone, GSettings *settings)
{
  g_autofree char *default_sound = NULL, *name = NULL;

  if (ringtone && !*ringtone)
    return g_strdup (_("Silent"));
  if (ringtone)
    return sound_name (ringtone);
  /* the default: "Default (Hassium)", with the default sound's name */
  default_sound = g_settings_get_string (settings, "default-alarm-ringtone");
  if (g_str_equal (default_sound, CLOCK_SILENT))
    return g_strdup (_("Silent"));
  if (!*default_sound)
    {
      g_free (default_sound);
      default_sound = default_alarm_sound ();
    }
  name = default_sound ? sound_name (default_sound) : g_strdup ("Hassium");
  /* the Clock's own sound without a title of its own (the package links it to freedesktop's alarm-clock-elapsed):
   * the picker's name for the default, not the file's name */
  if (default_sound && g_str_has_suffix (default_sound, "/vibeos-clock/sounds/alarm_expire.ogg")
      && g_str_equal (name, "alarm_expire"))
    return g_strdup (_("Default alarm sound"));
  return g_strdup_printf (_("Default (%s)"), name);
}

char *
clock_timer_sound_title (GSettings *settings)
{
  g_autofree char *sound = g_settings_get_string (settings, "timer-ringtone");

  if (!*sound)
    return g_strdup (_("Timer Expired"));
  if (g_str_equal (sound, CLOCK_SILENT))
    return g_strdup (_("Silent"));
  return sound_name (sound);
}

char *
clock_sound_name (const char *uri)
{
  return sound_name (uri);
}

gboolean
clock_has_vibrator (void)
{
  static int found = -1;
  g_autoptr(GDir) dir = NULL;
  const char *name;

  /* whether there is a vibrator. feedbackd, which plays the vibration, has no way to say whether there is anything to
   * play it on (org.sigxcpu.Feedback.Haptic only vibrates), so this asks the kernel's input class: a vibrator is
   * an input device that can play force feedback, whatever the phone or its driver */
  if (found >= 0)
    return found;
  found = FALSE;
  dir = g_dir_open ("/sys/class/input", 0, NULL);
  while (dir && !found && (name = g_dir_read_name (dir)))
    {
      g_autofree char *path = g_build_filename ("/sys/class/input", name, "capabilities/ff", NULL);
      g_autofree char *ff = NULL;

      found = g_str_has_prefix (name, "input") && g_file_get_contents (path, &ff, NULL, NULL)
              && !g_str_equal (g_strstrip (ff), "0");
    }
  return found;
}
