/* test-locale: vibeos-locale's locale, from the environment as gnome-session leaves it, and its dates and times in
 * that locale's language and order. The expected texts are CLDR's (ICU's data); the time zone is the time's own. */
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/locale/id", test_id);
  g_test_add_func ("/locale/dates", test_dates);
  g_test_add_func ("/locale/intervals", test_intervals);
  g_test_add_func ("/locale/time", test_time);
  return g_test_run ();
}
