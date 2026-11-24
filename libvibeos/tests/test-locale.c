/* test-locale: vibeos-locale's locale, from the environment as gnome-session leaves it, its dates and times in that
 * locale's language and order, the weather's units by its region, the names of locales, and a change of the
 * session's language. The expected texts are CLDR's (ICU's data); the time zone is the time's own. */
#include "vibeos-locale.h"

#include <gio/gio.h>

static GDateTime *
tuesday (void)
{
  g_autoptr (GTimeZone) zone = g_time_zone_new_identifier ("Europe/Berlin");
  return g_date_time_new (zone, 2026, 11, 24, 14, 5, 0);
}

/* the environment as a session has it: @lang, @language, @time (NULL for unset) */
static void
set_env (const char *lang, const char *language, const char *time)
{
  g_unsetenv ("LC_ALL");
  g_unsetenv ("LC_MESSAGES");
  if (lang)
    g_setenv ("LANG", lang, TRUE);
  else
    g_unsetenv ("LANG");
  if (language)
    g_setenv ("LANGUAGE", language, TRUE);
  else
    g_unsetenv ("LANGUAGE");
  if (time)
    g_setenv ("LC_TIME", time, TRUE);
  else
    g_unsetenv ("LC_TIME");
}

/* @text with its no-break, narrow no-break and thin spaces as plain ones: which of them CLDR puts where changes from one
 * ICU release to the next (Spanish's "a. m." has a no-break space in ICU 76, a narrow one in 77) */
static char *
plain (const char *text)
{
  GString *out = g_string_new (NULL);
  for (const char *c = text; *c; c = g_utf8_next_char (c))
    {
      gunichar u = g_utf8_get_char (c);
      if (u == 0x00a0 || u == 0x202f || u == 0x2009)
        g_string_append_c (out, ' ');
      else
        g_string_append_unichar (out, u);
    }
  return g_string_free (out, FALSE);
}

static void
check_id (const char *lang, const char *language, const char *time, const char *expected)
{
  set_env (lang, language, time);
  g_autofree char *id = vibeos_locale_id ();
  g_assert_cmpstr (id, ==, expected);
}

static void
test_id (void)
{
  check_id ("de_DE.UTF-8", NULL, NULL, "de_DE");
  check_id ("en_GB.UTF-8", NULL, NULL, "en_GB");
  /* English with the Formats region Germany: English names, the German order */
  check_id ("en_GB.UTF-8", NULL, "de_DE.UTF-8", "en_DE");
  /* German chosen in LANGUAGE over an English LANG */
  check_id ("en_GB.UTF-8", "de", NULL, "de_GB");
  check_id ("nl_NL.UTF-8", "nl_NL:nl", NULL, "nl_NL");
  check_id ("C.UTF-8", NULL, NULL, "en_US");
  check_id (NULL, NULL, NULL, "en_US");
}

static void
check_date (const char *lang, const char *time, const char *skeleton, const char *expected)
{
  g_autoptr (GDateTime) t = tuesday ();
  set_env (lang, NULL, time);
  g_autofree char *text = vibeos_format_date (t, skeleton);
  g_autofree char *spaced = plain (text);
  g_assert_cmpstr (spaced, ==, expected);
}

static void
test_dates (void)
{
  check_date ("en_US.UTF-8", NULL, "EEEMMMd", "Tue, Nov 24");
  check_date ("en_GB.UTF-8", "de_DE.UTF-8", "EEEMMMd", "Tue, 24 Nov");
  check_date ("de_DE.UTF-8", NULL, "EEEMMMd", "Di., 24. Nov.");
  check_date ("de_DE.UTF-8", NULL, "EEEEMMMMd", "Dienstag, 24. November");
  check_date ("fr_FR.UTF-8", NULL, "EEEEMMMMd", "mardi 24 novembre");
  check_date ("it_IT.UTF-8", NULL, "EEEEMMMMd", "martedì 24 novembre");
  check_date ("es_ES.UTF-8", NULL, "EEEEMMMMd", "martes, 24 de noviembre");
  check_date ("nl_NL.UTF-8", NULL, "EEEMMMd", "di 24 nov");
  check_date ("ru_RU.UTF-8", NULL, "EEEMMMd", "вт, 24 нояб.");
}

static void
check_interval (const char *lang, const char *skeleton, const char *expected)
{
  g_autoptr (GTimeZone) zone = g_time_zone_new_identifier ("Europe/Berlin");
  g_autoptr (GDateTime) from = g_date_time_new (zone, 2026, 11, 24, 10, 0, 0);
  g_autoptr (GDateTime) to = g_date_time_new (zone, 2026, 11, 26, 11, 30, 0);
  set_env (lang, NULL, NULL);
  g_autofree char *text = vibeos_format_interval (from, to, skeleton);
  g_autofree char *spaced = plain (text);
  g_assert_cmpstr (spaced, ==, expected);
}

/* the parts both ends share written once, around CLDR's dash (with spaces around it in English) */
static void
test_intervals (void)
{
  check_interval ("en_US.UTF-8", "MMMd", "Nov 24 \u2013 26");
  check_interval ("de_DE.UTF-8", "MMMd", "24.\u201326. Nov.");
  check_interval ("ru_RU.UTF-8", "MMMMd", "24\u201326 ноября");
  check_interval ("de_DE.UTF-8", "MMMEdHm", "Di., 24. Nov., 10:00 \u2013 Do., 26. Nov., 11:30");
}

static void
test_time (void)
{
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.desktop.interface");
  g_autoptr (GDateTime) t = tuesday ();

  set_env ("en_US.UTF-8", NULL, NULL);
  g_settings_set_string (settings, "clock-format", "24h");
  g_assert_true (vibeos_clock_uses_24h ());
  g_autofree char *h24 = vibeos_format_time (t);
  g_assert_cmpstr (h24, ==, "14:05");
  g_autofree char *clock24 = vibeos_format_clock (t);
  g_assert_cmpstr (clock24, ==, "14:05");

  /* 12 hours, with the language's names for the afternoon */
  g_settings_set_string (settings, "clock-format", "12h");
  g_assert_false (vibeos_clock_uses_24h ());
  g_autofree char *h12 = vibeos_format_time (t);
  g_assert_true (g_str_has_prefix (h12, "2:05") && g_str_has_suffix (h12, "PM"));
  /* a large clock's, without them */
  g_autofree char *clock = vibeos_format_clock (t);
  g_assert_cmpstr (clock, ==, "2:05");
  g_autofree char *am = NULL, *pm = NULL;
  set_env ("es_ES.UTF-8", NULL, NULL);
  vibeos_locale_am_pm (&am, &pm);
  g_autofree char *am_spaced = plain (am), *pm_spaced = plain (pm);
  g_assert_cmpstr (am_spaced, ==, "a. m.");
  g_assert_cmpstr (pm_spaced, ==, "p. m.");
  g_settings_reset (settings, "clock-format");
}

static void
check_name (const char *posix, const char *in, const char *expected)
{
  g_autofree char *name = vibeos_locale_name (posix, in);
  g_assert_cmpstr (name, ==, expected);
}

/* a language list's names: each language in itself, or in another; the first letter a capital */
static void
test_names (void)
{
  check_name ("de_DE.UTF-8", NULL, "Deutsch (Deutschland)");
  check_name ("fr_FR.UTF-8", NULL, "Français (France)");
  check_name ("it_IT.UTF-8", NULL, "Italiano (Italia)");
  check_name ("es_ES.UTF-8", NULL, "Español (España)");
  check_name ("nl_NL.UTF-8", NULL, "Nederlands (Nederland)");
  check_name ("ru_RU.UTF-8", NULL, "Русский (Россия)");
  check_name ("en_GB.UTF-8", NULL, "English (United Kingdom)");
  /* a modifier that names a script or a variant, and one that does not */
  check_name ("sr_RS.UTF-8@latin", NULL, "Srpski (latinica, Srbija)");
  check_name ("de_DE.UTF-8@euro", NULL, "Deutsch (Deutschland)");
  check_name ("fr_FR.UTF-8", "en_US", "French (France)");
  check_name ("fr_FR.UTF-8", "de_DE", "Französisch (Frankreich)");
  check_name ("C.UTF-8", NULL, NULL);
  check_name ("POSIX", "en_US", NULL);
}

/* the weather's units by the region, by CLDR's unit preferences; the world's (metric) with no language */
static void
test_measures (void)
{
  struct {
    const char *lang, *wind, *pressure;
    gboolean fahrenheit;
  } cases[] = {
    { "en_US.UTF-8", "12 mph", "29.92 inHg", TRUE },
    { "en_GB.UTF-8", "12 mph", "1,013 mbar", FALSE },
    { "de_DE.UTF-8", "19 km/h", "1.013 hPa", FALSE },
    { "ru_RU.UTF-8", "5 м/с", "760 мм рт. ст.", FALSE },
    { "C.UTF-8", "19 km/h", "1,013 hPa", FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    set_env (cases[i].lang, NULL, NULL);
    g_assert_cmpint (vibeos_locale_uses_fahrenheit (), ==, cases[i].fahrenheit);
    g_autofree char *wind = vibeos_format_wind_speed (19.3), *pressure = vibeos_format_air_pressure (1013.2);
    g_autofree char *wind_spaced = plain (wind), *pressure_spaced = plain (pressure);
    g_assert_cmpstr (wind_spaced, ==, cases[i].wind);
    g_assert_cmpstr (pressure_spaced, ==, cases[i].pressure);
  }
  /* English with the region Germany: the region's units */
  set_env ("en_US.UTF-8", NULL, "de_DE.UTF-8");
  g_assert_false (vibeos_locale_uses_fahrenheit ());
}

static void
test_session_environment (void)
{
  g_auto (GStrv) same = vibeos_locale_session_environment ("de_DE.UTF-8", "");
  const char *const expected_same[] = { "LANG=de_DE.UTF-8", "LANGUAGE=", "LC_ALL=", "LC_MESSAGES=", "LC_TIME=",
                                        "LC_NUMERIC=", "LC_MONETARY=", "LC_MEASUREMENT=", "LC_PAPER=", NULL };
  g_assert_true (g_strv_equal ((const char *const *) same, expected_same));
  g_auto (GStrv) also_same = vibeos_locale_session_environment ("de_DE.UTF-8", "de_DE.UTF-8");
  g_assert_true (g_strv_equal ((const char *const *) also_same, expected_same));
  g_auto (GStrv) apart = vibeos_locale_session_environment ("en_US.UTF-8", "de_DE.UTF-8");
  const char *const expected_apart[] = { "LANG=en_US.UTF-8", "LANGUAGE=", "LC_ALL=", "LC_MESSAGES=",
                                         "LC_TIME=de_DE.UTF-8", "LC_NUMERIC=de_DE.UTF-8", "LC_MONETARY=de_DE.UTF-8",
                                         "LC_MEASUREMENT=de_DE.UTF-8", "LC_PAPER=de_DE.UTF-8", NULL };
  g_assert_true (g_strv_equal ((const char *const *) apart, expected_apart));
}

static int called;

static void
count (gpointer data)
{
  called++;
}

static void
heard (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
       GVariant *params, gpointer data)
{
  (*(int *) data)++;
}

/* runs the main context until @counter reaches @n, for 5 seconds at most */
static void
wait_for (int *counter, int n)
{
  gint64 end = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while (*counter < n && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, FALSE);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpint (*counter, >=, n);
}

/* a change on a bus of the test's own: this program's environment and its watches at once, its own signal coming
 * back changing nothing, another program's change then, and only the language's variables taken from it */
static void
test_session (void)
{
  g_autofree char *daemon = g_find_program_in_path ("dbus-daemon");
  if (!daemon) {
    g_test_skip ("no dbus-daemon");
    return;
  }
  g_autoptr (GTestDBus) test_bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (test_bus);
  set_env ("C.UTF-8", "en_GB:en", NULL);
  g_autoptr (GDBusConnection) other = g_dbus_connection_new_for_address_sync (
    g_test_dbus_get_bus_address (test_bus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
  g_assert_nonnull (other);
  int signals = 0;
  guint listen = g_dbus_connection_signal_subscribe (other, NULL, "vibeos.Locale", "Changed", "/vibeos/Locale", NULL,
                                                     G_DBUS_SIGNAL_FLAGS_NONE, heard, &signals, NULL);
  guint watch = vibeos_locale_watch (count, NULL, NULL);

  g_autoptr (GError) error = NULL;
  gboolean told = vibeos_locale_set_session ("de_DE.UTF-8", "en_GB.UTF-8", &error);
  /* a test bus has no systemd user manager: the one thing that could not be told */
  g_assert_false (told);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN);
  g_assert_cmpstr (g_getenv ("LANG"), ==, "de_DE.UTF-8");
  g_assert_null (g_getenv ("LANGUAGE"));
  g_assert_cmpstr (g_getenv ("LC_TIME"), ==, "en_GB.UTF-8");
  g_autofree char *id = vibeos_locale_id ();
  g_assert_cmpstr (id, ==, "de_GB");
  g_assert_cmpint (called, ==, 1);
  wait_for (&signals, 1);
  g_assert_cmpint (called, ==, 1);

  const char *const french[] = { "LANG=fr_FR.UTF-8", "LC_TIME=", "PATH=/nowhere", NULL };
  g_dbus_connection_emit_signal (other, NULL, "/vibeos/Locale", "vibeos.Locale", "Changed",
                                 g_variant_new ("(^as)", french), NULL);
  g_dbus_connection_flush_sync (other, NULL, NULL);
  wait_for (&called, 2);
  g_assert_cmpstr (g_getenv ("LANG"), ==, "fr_FR.UTF-8");
  g_assert_null (g_getenv ("LC_TIME"));
  g_assert_cmpstr (g_getenv ("PATH"), !=, "/nowhere");

  vibeos_locale_unwatch (watch);
  g_dbus_connection_signal_unsubscribe (other, listen);
  g_dbus_connection_close_sync (other, NULL, NULL);
  g_test_dbus_down (test_bus);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/locale/id", test_id);
  g_test_add_func ("/locale/dates", test_dates);
  g_test_add_func ("/locale/intervals", test_intervals);
  g_test_add_func ("/locale/time", test_time);
  g_test_add_func ("/locale/measures", test_measures);
  g_test_add_func ("/locale/names", test_names);
  g_test_add_func ("/locale/session-environment", test_session_environment);
  g_test_add_func ("/locale/session", test_session);
  return g_test_run ();
}
