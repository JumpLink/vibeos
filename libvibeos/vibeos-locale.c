/* vibeos-locale: see vibeos-locale.h. ICU's date formats are slow to make (the pattern generator reads the
 * locale's data), so each one is made once, for a locale, a skeleton and a time zone offset, and kept; ICU's formats
 * are not to be used by two threads at once, so a lock is held around each. */
#include "vibeos-locale.h"

#include <gio/gio.h>
#include <string.h>
#include <unicode/udat.h>
#include <unicode/udateintervalformat.h>
#include <unicode/udatpg.h>

static GMutex lock;
static GHashTable *formats;     /* "locale\nskeleton\noffset\nclock" -> UDateFormat */
static GHashTable *intervals;   /* "locale\nskeleton\noffset" -> UDateIntervalFormat */

/* "de_DE.UTF-8@euro" -> "de_DE"; NULL for C and POSIX */
static char *
icu_of (const char *posix)
{
  if (!posix || !*posix || !strcmp (posix, "C") || g_str_has_prefix (posix, "C.") || !strcmp (posix, "POSIX"))
    return NULL;
  return g_strndup (posix, strcspn (posix, ".@"));
}

static char *
language_of (const char *icu)
{
  return g_strndup (icu, strcspn (icu, "_"));
}

static const char *
country_of (const char *icu)
{
  const char *u = icu ? strchr (icu, '_') : NULL;
  return u && u[1] ? u + 1 : NULL;
}

char *
vibeos_locale_id (void)
{
  /* the interface's language: the first of the user's languages that is one */
  g_autofree char *ui = NULL;
  for (const char *const *l = g_get_language_names (); *l && !ui; l++)
    ui = icu_of (*l);
  if (!ui)
    return g_strdup ("en_US");
  /* the region: the Formats region's (LC_TIME, set only when it is not the language's), else the language's own, else
   * LANG's (the language may have come from LANGUAGE without a country) */
  const char *all = g_getenv ("LC_ALL");
  g_autofree char *time = icu_of (all && *all ? all : g_getenv ("LC_TIME"));
  g_autofree char *lang = icu_of (g_getenv ("LANG"));
  const char *country = country_of (time);
  if (!country)
    country = country_of (ui);
  if (!country)
    country = country_of (lang);
  g_autofree char *language = language_of (ui);
  return country ? g_strdup_printf ("%s_%s", language, country) : g_steal_pointer (&language);
}

gboolean
vibeos_clock_uses_24h (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                              "org.gnome.desktop.interface", TRUE);
  if (!schema)
    return TRUE;
  g_settings_schema_unref (schema);
  GSettings *settings = g_settings_new ("org.gnome.desktop.interface");
  char *format = g_settings_get_string (settings, "clock-format");
  gboolean is_24h = g_strcmp0 (format, "12h") != 0;
  g_free (format);
  g_object_unref (settings);
  return is_24h;
}

static UChar *
utf16 (const char *s)
{
  return (UChar *) g_utf8_to_utf16 (s, -1, NULL, NULL, NULL);
}

static char *
utf8 (const UChar *s, int32_t length)
{
  return g_utf16_to_utf8 ((const gunichar2 *) s, length, NULL, NULL, NULL);
}

/* the time zone @offset seconds east of UTC, as ICU names it ("GMT+01:00"); newly allocated */
static UChar *
zone_of (int offset)
{
  g_autofree char *zone = offset ? g_strdup_printf ("GMT%c%02d:%02d", offset < 0 ? '-' : '+', ABS (offset) / 3600,
                                                    ABS (offset) / 60 % 60)
                                 : g_strdup ("GMT");
  return utf16 (zone);
}

/* @skeleton with its 'j' (the locale's own hour) the hour of the clock the user set; newly allocated */
static char *
user_skeleton (const char *skeleton)
{
  char *skel = g_strdup (skeleton);

  if (strchr (skel, 'j'))
    g_strdelimit (skel, "j", vibeos_clock_uses_24h () ? 'H' : 'h');
  return skel;
}

static UDate
udate_of (GDateTime *time)
{
  return g_date_time_to_unix (time) * 1000.0 + g_date_time_get_microsecond (time) / 1000;
}

/* @pattern without its day period ("h:mm a" -> "h:mm", "a h:mm" -> "h:mm"): its letters a, b and B outside quotes,
 * and the spaces next to them */
static void
drop_day_period (UChar *pattern, int32_t *length)
{
  int32_t out = 0;
  gboolean quoted = FALSE;

  for (int32_t i = 0; i < *length; i++)
    {
      UChar c = pattern[i];
      if (c == '\'')
        quoted = !quoted;
      if (!quoted && (c == 'a' || c == 'b' || c == 'B'))
        {
          /* the space before it, or else the one after it */
          if (out > 0 && (pattern[out - 1] == ' ' || pattern[out - 1] == 0x00a0 || pattern[out - 1] == 0x202f))
            out--;
          else if (i + 1 < *length && (pattern[i + 1] == ' ' || pattern[i + 1] == 0x00a0 || pattern[i + 1] == 0x202f))
            i++;
          continue;
        }
      pattern[out++] = c;
    }
  *length = out;
}

/* the format for @locale, @skeleton and a time zone @offset seconds east of UTC, without a day period if @clock;
 * called with the lock held */
static UDateFormat *
format_for (const char *locale, const char *skeleton, int offset, gboolean clock)
{
  g_autofree char *key = g_strdup_printf ("%s\n%s\n%d\n%d", locale, skeleton, offset, clock);
  if (!formats)
    formats = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) udat_close);
  UDateFormat *format = g_hash_table_lookup (formats, key);
  if (format)
    return format;

  UErrorCode status = U_ZERO_ERROR;
  UDateTimePatternGenerator *generator = udatpg_open (locale, &status);
  g_autofree UChar *skel = utf16 (skeleton);
  UChar pattern[160];
  int32_t length = U_SUCCESS (status) ? udatpg_getBestPattern (generator, skel, -1, pattern,
                                                               G_N_ELEMENTS (pattern), &status) : 0;
  if (generator)
    udatpg_close (generator);
  if (U_FAILURE (status))
    return NULL;
  if (clock)
    drop_day_period (pattern, &length);
  /* the time zone as an offset: the time is shown as @time has it */
  g_autofree UChar *zone16 = zone_of (offset);
  format = udat_open (UDAT_PATTERN, UDAT_PATTERN, locale, zone16, -1, pattern, length, &status);
  if (U_FAILURE (status))
    return NULL;
  g_hash_table_insert (formats, g_steal_pointer (&key), format);
  return format;
}

static char *
format (GDateTime *time, const char *skeleton, gboolean clock)
{
  g_autofree char *locale = vibeos_locale_id ();
  g_autofree char *skel = user_skeleton (skeleton);
  int offset = (int) (g_date_time_get_utc_offset (time) / G_TIME_SPAN_SECOND);
  UDate when = udate_of (time);

  g_mutex_lock (&lock);
  UDateFormat *date_format = format_for (locale, skel, offset, clock);
  UChar out[256];
  UErrorCode status = U_ZERO_ERROR;
  int32_t length = date_format ? udat_format (date_format, when, out, G_N_ELEMENTS (out), NULL, &status) : -1;
  g_mutex_unlock (&lock);
  if (!date_format || U_FAILURE (status))
    return g_date_time_format (time, "%x");
  return utf8 (out, length);
}

char *
vibeos_format_date (GDateTime *time, const char *skeleton)
{
  g_return_val_if_fail (time != NULL && skeleton != NULL, NULL);
  return format (time, skeleton, FALSE);
}

char *
vibeos_format_time (GDateTime *time)
{
  g_return_val_if_fail (time != NULL, NULL);
  return format (time, "jm", FALSE);
}

char *
vibeos_format_clock (GDateTime *time)
{
  g_return_val_if_fail (time != NULL, NULL);
  return format (time, "jm", TRUE);
}

void
vibeos_locale_am_pm (char **am, char **pm)
{
  g_autofree char *locale = vibeos_locale_id ();
  UErrorCode status = U_ZERO_ERROR;
  UChar text[32];

  *am = *pm = NULL;
  g_mutex_lock (&lock);
  UDateFormat *date_format = format_for (locale, "hma", 0, FALSE);
  for (int i = 0; date_format && i < 2; i++)
    {
      status = U_ZERO_ERROR;
      int32_t length = udat_getSymbols (date_format, UDAT_AM_PMS, i, text, G_N_ELEMENTS (text), &status);
      if (U_SUCCESS (status))
        *(i ? pm : am) = utf8 (text, length);
    }
  g_mutex_unlock (&lock);
  if (!*am)
    *am = g_strdup ("AM");
  if (!*pm)
    *pm = g_strdup ("PM");
}

char *
vibeos_format_interval (GDateTime *from, GDateTime *to, const char *skeleton)
{
  g_return_val_if_fail (from != NULL && to != NULL && skeleton != NULL, NULL);
  g_autofree char *locale = vibeos_locale_id ();
  g_autofree char *skel = user_skeleton (skeleton);
  int offset = (int) (g_date_time_get_utc_offset (from) / G_TIME_SPAN_SECOND);
  g_autofree char *key = g_strdup_printf ("%s\n%s\n%d", locale, skel, offset);
  UChar out[256];
  UErrorCode status = U_ZERO_ERROR;
  int32_t length = -1;

  g_mutex_lock (&lock);
  if (!intervals)
    intervals = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) udtitvfmt_close);
  UDateIntervalFormat *interval = g_hash_table_lookup (intervals, key);
  if (!interval)
    {
      g_autofree UChar *skel16 = utf16 (skel), *zone16 = zone_of (offset);
      interval = udtitvfmt_open (locale, skel16, -1, zone16, -1, &status);
      if (U_SUCCESS (status))
        g_hash_table_insert (intervals, g_steal_pointer (&key), interval);
      else
        interval = NULL;
    }
  if (interval)
    {
      status = U_ZERO_ERROR;
      length = udtitvfmt_format (interval, udate_of (from), udate_of (to), out, G_N_ELEMENTS (out), NULL, &status);
    }
  g_mutex_unlock (&lock);
  if (interval && U_SUCCESS (status))
    return utf8 (out, length);
  /* both ends in full, between an en dash */
  g_autofree char *a = vibeos_format_date (from, skeleton), *b = vibeos_format_date (to, skeleton);
  return g_strconcat (a, " – ", b, NULL);
}
