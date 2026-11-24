/* vibeos-locale: see vibeos-locale.h. ICU's date formats are slow to make (the pattern generator reads the
 * locale's data), so each one is made once, for a locale, a skeleton and a time zone offset, and kept; ICU's formats
 * are not to be used by two threads at once, so a lock is held around each. */
#include "vibeos-locale.h"

#include <gio/gio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unicode/ucasemap.h>
#include <unicode/udat.h>
#include <unicode/udateintervalformat.h>
#include <unicode/udatpg.h>
#include <unicode/uloc.h>
#include <unicode/unumberformatter.h>

static GMutex lock;
static GHashTable *formats;     /* "locale\nskeleton\noffset\nclock" -> UDateFormat */
static GHashTable *intervals;   /* "locale\nskeleton\noffset" -> UDateIntervalFormat */
static GHashTable *numbers;     /* "locale\nskeleton" -> UNumberFormatter */

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

/* the user's locale, as vibeos_locale_id has it; NULL when the interface is in no language (C) */
static char *
user_locale (void)
{
  /* the interface's language: the first of the user's languages that is one */
  g_autofree char *ui = NULL;
  for (const char *const *l = g_get_language_names (); *l && !ui; l++)
    ui = icu_of (*l);
  if (!ui)
    return NULL;
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

char *
vibeos_locale_id (void)
{
  char *id = user_locale ();
  return id ? id : g_strdup ("en_US");
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

/* ---- measures ---- */

/* the locale whose region picks the units: the user's, or the world's (metric) when the interface is in no language,
 * where vibeos_locale_id's "en_US" would turn every temperature into Fahrenheit */
static char *
units_locale (void)
{
  char *id = user_locale ();
  return id ? id : g_strdup ("en_001");
}

/* @value by ICU's number skeleton @skeleton in @locale; NULL when ICU cannot. The formatters are made once and kept,
 * as the date formats are, and used without the lock: ICU's number formatters are immutable, safe in any thread. */
static char *
number (const char *locale, const char *skeleton, double value)
{
  g_autofree char *key = g_strdup_printf ("%s\n%s", locale, skeleton);
  UErrorCode status = U_ZERO_ERROR;
  g_mutex_lock (&lock);
  if (!numbers)
    numbers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) unumf_close);
  UNumberFormatter *f = g_hash_table_lookup (numbers, key);
  if (!f) {
    g_autofree UChar *s = utf16 (skeleton);
    f = unumf_openForSkeletonAndLocale (s, -1, locale, &status);
    if (U_SUCCESS (status))
      g_hash_table_insert (numbers, g_steal_pointer (&key), f);
    else
      f = NULL;
  }
  g_mutex_unlock (&lock);
  if (!f)
    return NULL;
  UFormattedNumber *result = unumf_openResult (&status);
  unumf_formatDouble (f, value, result, &status);
  UChar out[128];
  int32_t length = unumf_resultToString (result, out, G_N_ELEMENTS (out), &status);
  unumf_closeResult (result);
  if (U_FAILURE (status) || status == U_STRING_NOT_TERMINATED_WARNING)
    return NULL;
  return utf8 (out, length);
}

gboolean
vibeos_locale_uses_fahrenheit (void)
{
  g_autofree char *locale = units_locale ();
  /* water's boiling point in the unit the region's weather is told in, in plain digits: "212°F" or "100°C" */
  g_autofree char *boiling = number (locale, "unit/celsius usage/weather precision-integer numbering-system/latn "
                                             "group-off", 100);
  return boiling && atoi (boiling) == 212;
}

char *
vibeos_format_wind_speed (double kilometres_per_hour)
{
  g_autofree char *locale = units_locale ();
  char *s = number (locale, "unit/kilometer-per-hour usage/wind unit-width-short precision-integer",
                    kilometres_per_hour);
  return s ? s : g_strdup_printf ("%.0f km/h", kilometres_per_hour);
}

char *
vibeos_format_air_pressure (double hectopascals)
{
  g_autofree char *locale = units_locale ();
  /* whole hectopascals, millibars and millimetres of mercury; inches of mercury, whose readings are around 30 (the
   * only barometer unit CLDR's preferences have that low), to two decimals, as a barometer gives them */
  g_autofree char *whole = number (locale, "unit/hectopascal usage/baromtrc precision-integer numbering-system/latn "
                                           "group-off", hectopascals);
  gboolean inches = whole && atoi (whole) < 100;
  char *s = number (locale, inches ? "unit/hectopascal usage/baromtrc unit-width-short precision-increment/0.01"
                                   : "unit/hectopascal usage/baromtrc unit-width-short precision-integer",
                    hectopascals);
  return s ? s : g_strdup_printf ("%.0f hPa", hectopascals);
}

/* ---- names ---- */

/* ICU's id of a POSIX locale with what its modifier says of the language: "sr_RS.UTF-8@latin" -> "sr_Latn_RS",
 * "ca_ES.UTF-8@valencia" -> "ca_ES_VALENCIA"; a modifier that does not ("@euro": the currency) is left out. NULL for
 * C and POSIX */
static char *
icu_full (const char *posix)
{
  static const struct {
    const char *modifier, *script;
  } scripts[] = { { "latin", "Latn" }, { "cyrillic", "Cyrl" }, { "devanagari", "Deva" } };
  g_autofree char *base = icu_of (posix);
  const char *at = posix ? strchr (posix, '@') : NULL;
  if (!base || !at)
    return g_steal_pointer (&base);
  g_autofree char *language = language_of (base);
  const char *country = country_of (base);
  for (guint i = 0; i < G_N_ELEMENTS (scripts); i++)
    if (!strcmp (at + 1, scripts[i].modifier))
      return country ? g_strdup_printf ("%s_%s_%s", language, scripts[i].script, country)
                     : g_strdup_printf ("%s_%s", language, scripts[i].script);
  if (!strcmp (at + 1, "valencia"))
    return g_strdup_printf (country ? "%s_VALENCIA" : "%s__VALENCIA", base);
  return g_steal_pointer (&base);
}

char *
vibeos_locale_name (const char *posix, const char *in)
{
  g_autofree char *id = icu_full (posix);
  if (!id)
    return NULL;
  if (!in)
    in = id;
  UErrorCode status = U_ZERO_ERROR;
  UChar name[256];
  int32_t length = uloc_getDisplayName (id, in, name, G_N_ELEMENTS (name), &status);
  if (U_FAILURE (status) || status == U_STRING_NOT_TERMINATED_WARNING || length <= 0)
    return g_strdup (posix);
  g_autofree char *text = utf8 (name, length);
  /* its first letter a capital, by the language's own rules (Dutch's "IJslands"), the rest as ICU has it: CLDR writes
   * most languages' names in lower case ("français (France)"), as they are written inside a sentence */
  UCaseMap *map = ucasemap_open (in, U_TITLECASE_WHOLE_STRING | U_TITLECASE_NO_LOWERCASE, &status);
  char title[1024];
  int32_t n = U_SUCCESS (status) ? ucasemap_utf8ToTitle (map, title, sizeof title, text, -1, &status) : 0;
  ucasemap_close (map);
  if (U_FAILURE (status) || status == U_STRING_NOT_TERMINATED_WARNING || n <= 0)
    return g_steal_pointer (&text);
  return g_strndup (title, n);
}

/* ---- the session's language ---- */

#define LOCALE_PATH      "/vibeos/Locale"
#define LOCALE_INTERFACE "vibeos.Locale"

/* the Formats region's variables, the ones gnome-session sets */
static const char *const region_variables[] = { "LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER",
                                                NULL };

char **
vibeos_locale_session_environment (const char *language, const char *region)
{
  g_return_val_if_fail (language != NULL, NULL);
  GStrvBuilder *b = g_strv_builder_new ();
  g_strv_builder_take (b, g_strconcat ("LANG=", language, NULL));
  /* each of these would stand in front of LANG: gettext's list of languages, the variable over every category, and
   * the messages' own */
  g_strv_builder_add_many (b, "LANGUAGE=", "LC_ALL=", "LC_MESSAGES=", NULL);
  /* a region that is not the language's own: gnome-session leaves them unset for the language's */
  gboolean apart = region && *region && g_strcmp0 (region, language);
  for (guint i = 0; region_variables[i]; i++)
    g_strv_builder_take (b, g_strconcat (region_variables[i], "=", apart ? region : "", NULL));
  char **env = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return env;
}

/* a variable the session's language is in: the only ones a change sets, whoever sends it */
static gboolean
is_session_variable (const char *name)
{
  return !strcmp (name, "LANG") || !strcmp (name, "LANGUAGE") || !strcmp (name, "LC_ALL") ||
         !strcmp (name, "LC_MESSAGES") || g_strv_contains (region_variables, name);
}

/* this program's environment, C locale and ICU default locale to @env's (NAME=VALUE, an empty VALUE unsets);
 * whether anything changed */
static gboolean
apply (const char *const *env)
{
  gboolean changed = FALSE;
  for (guint i = 0; env && env[i]; i++) {
    const char *eq = strchr (env[i], '=');
    if (!eq)
      continue;
    g_autofree char *name = g_strndup (env[i], eq - env[i]);
    const char *value = eq + 1, *was = g_getenv (name);
    if (!is_session_variable (name) || (!*value && (!was || !*was)) || !g_strcmp0 (was, value))
      continue;
    if (*value)
      g_setenv (name, value, TRUE);
    else
      g_unsetenv (name);
    changed = TRUE;
  }
  if (changed) {
    UErrorCode status = U_ZERO_ERROR;
    g_autofree char *id = vibeos_locale_id ();
    setlocale (LC_ALL, "");
    uloc_setDefault (id, &status);
  }
  return changed;
}

typedef struct {
  guint            id;
  VibeosLocaleFunc func;
  gpointer         data;
  GDestroyNotify   destroy;
} Watch;

/* the watches (the main thread's), and the subscription to the change while there are any */
static GList *watches;
static guint last_watch;
static GDBusConnection *watch_bus;
static guint subscription;

static void
notify (void)
{
  /* by id, since a function may unwatch itself or another */
  g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (guint));
  for (GList *l = watches; l; l = l->next)
    g_array_append_val (ids, ((Watch *) l->data)->id);
  for (guint i = 0; i < ids->len; i++)
    for (GList *l = watches; l; l = l->next)
      if (((Watch *) l->data)->id == g_array_index (ids, guint, i)) {
        ((Watch *) l->data)->func (((Watch *) l->data)->data);
        break;
      }
}

static void
changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
         GVariant *params, gpointer data)
{
  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(as)")))
    return;
  g_autofree const char **env = NULL;
  g_variant_get (params, "(^a&s)", &env);
  if (apply (env))
    notify ();
}

guint
vibeos_locale_watch (VibeosLocaleFunc func, gpointer user_data, GDestroyNotify destroy)
{
  g_return_val_if_fail (func != NULL, 0);
  if (!subscription) {
    g_autoptr (GError) error = NULL;
    watch_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    if (watch_bus)
      subscription = g_dbus_connection_signal_subscribe (watch_bus, NULL, LOCALE_INTERFACE, "Changed", LOCALE_PATH,
                                                         NULL, G_DBUS_SIGNAL_FLAGS_NONE, changed, NULL, NULL);
    else
      g_warning ("no session bus to hear of a new language on: %s", error->message);
  }
  Watch *w = g_new0 (Watch, 1);
  w->id = ++last_watch;
  w->func = func;
  w->data = user_data;
  w->destroy = destroy;
  watches = g_list_append (watches, w);
  return w->id;
}

void
vibeos_locale_unwatch (guint id)
{
  for (GList *l = watches; l; l = l->next) {
    Watch *w = l->data;
    if (w->id != id)
      continue;
    watches = g_list_delete_link (watches, l);
    if (w->destroy)
      w->destroy (w->data);
    g_free (w);
    break;
  }
  if (!watches && subscription) {
    g_dbus_connection_signal_unsubscribe (watch_bus, subscription);
    subscription = 0;
    g_clear_object (&watch_bus);
  }
}

/* @method of @name, waited for; its error kept in @first when it is the first */
static void
tell (GDBusConnection *bus, const char *name, const char *path, const char *iface, const char *method,
      GVariant *args, GError **first)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, name, path, iface, method, args, NULL,
                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  if (!reply && !*first) {
    g_prefix_error (&error, "%s: ", method);
    *first = g_steal_pointer (&error);
  }
}

gboolean
vibeos_locale_set_session (const char *language, const char *region, GError **error)
{
  g_return_val_if_fail (language && *language, FALSE);
  g_auto (GStrv) env = vibeos_locale_session_environment (language, region);
  g_autoptr (GError) failed = NULL;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &failed);
  if (bus) {
    GVariantBuilder unset, set, activation;
    g_variant_builder_init (&unset, G_VARIANT_TYPE_STRING_ARRAY);
    g_variant_builder_init (&set, G_VARIANT_TYPE_STRING_ARRAY);
    g_variant_builder_init (&activation, G_VARIANT_TYPE ("a{ss}"));
    for (guint i = 0; env[i]; i++) {
      const char *eq = strchr (env[i], '=');
      g_autofree char *name = g_strndup (env[i], eq - env[i]);
      if (eq[1])
        g_variant_builder_add (&set, "s", env[i]);
      else
        g_variant_builder_add (&unset, "s", name);
      g_variant_builder_add (&activation, "{ss}", name, eq + 1);
    }
    /* systemd's user manager: the services it starts from now on */
    tell (bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
          "UnsetAndSetEnvironment", g_variant_new ("(asas)", &unset, &set), &failed);
    /* the bus: the apps it starts for a D-Bus activation (an empty value can only be set, which the C library takes
     * for unset) */
    tell (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment",
          g_variant_new ("(a{ss})", &activation), &failed);
    /* the programs running that follow it */
    g_autoptr (GError) emit_error = NULL;
    if (!g_dbus_connection_emit_signal (bus, NULL, LOCALE_PATH, LOCALE_INTERFACE, "Changed",
                                        g_variant_new ("(^as)", env), &emit_error) ||
        !g_dbus_connection_flush_sync (bus, NULL, &emit_error)) {
      if (!failed)
        failed = g_steal_pointer (&emit_error);
    }
  }
  if (apply ((const char *const *) env))
    notify ();
  if (failed) {
    g_propagate_error (error, g_steal_pointer (&failed));
    return FALSE;
  }
  return TRUE;
}
