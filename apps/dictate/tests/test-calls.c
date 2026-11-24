/* test-calls: what Agent says when a call rings, and when it may (dictate-calls.c): the announcement in each
 * language from get_current_call's result, never a number's digits; the quiet hours, over midnight too; Announce
 * calls and the access switch. Settings in memory, the time of day from DICTATE_TEST_NOW. */
#include "dictate-calls.h"

#include <string.h>

static void
test_announcement (void)
{
  static const char *const languages[] = { "en", "de", "es", "fr", "it", "ru" };
  const char *anna = "{\"calls\":[{\"with\":\"Anna Becker\",\"in_contacts\":true,\"state\":\"ringing\"}]}";
  const char *unknown = "{\"calls\":[{\"with\":\"an unknown number (not in the contacts)\",\"in_contacts\":false,"
                        "\"state\":\"ringing\"}]}";
  const char *hidden = "{\"calls\":[{\"with\":\"a private number\",\"in_contacts\":false,\"private_number\":true,"
                       "\"state\":\"ringing\"}]}";
  const char *going = "{\"calls\":[{\"with\":\"Anna Becker\",\"in_contacts\":true,\"state\":\"going on\"}]}";

  for (guint i = 0; i < G_N_ELEMENTS (languages); i++)
    {
      g_autofree char *named = dictate_calls_announce_text (languages[i], anna);
      g_autofree char *other = dictate_calls_announce_text (languages[i], unknown);
      g_autofree char *withheld = dictate_calls_announce_text (languages[i], hidden);

      g_assert_nonnull (strstr (named, "Anna Becker"));
      /* an unknown caller: said so in the language, with no English words and no digits */
      g_assert_null (strstr (other, "unknown number"));
      g_assert_cmpstr (other, !=, withheld);
      for (const char *p = other; *p; p++)
        g_assert_false (g_ascii_isdigit (*p));
    }
  /* private mode on the speaker: the tool gave no name, nor does the announcement */
  {
    const char *withheld = "{\"calls\":[{\"with\":\"someone\",\"in_contacts\":true,\"state\":\"ringing\"}],"
                           "\"names_withheld\":\"…\"}";
    g_autofree char *quiet = dictate_calls_announce_text ("de", withheld);
    g_assert_cmpstr (quiet, ==, "Anruf von einem deiner Kontakte. Annehmen oder ablehnen?");
  }
  g_assert_null (dictate_calls_announce_text ("en", going));
  g_assert_null (dictate_calls_announce_text ("en", "{\"note\":\"No call is ringing or going on.\"}"));
  g_assert_null (dictate_calls_announce_text ("en", "{\"error\":\"No access\"}"));
}

static void
test_quiet (void)
{
  g_autoptr (GSettings) settings = g_settings_new ("vibeos.Dictate");

  g_settings_set_string (settings, "quiet-hours", "22:00-07:00");
  g_setenv ("DICTATE_TEST_NOW", "23:30", TRUE);
  g_assert_true (dictate_calls_quiet_now (settings));
  g_setenv ("DICTATE_TEST_NOW", "06:59", TRUE);
  g_assert_true (dictate_calls_quiet_now (settings));
  g_setenv ("DICTATE_TEST_NOW", "07:00", TRUE);
  g_assert_false (dictate_calls_quiet_now (settings));
  g_setenv ("DICTATE_TEST_NOW", "12:00", TRUE);
  g_assert_false (dictate_calls_quiet_now (settings));
  g_settings_set_string (settings, "quiet-hours", "13:00-14:30");
  g_setenv ("DICTATE_TEST_NOW", "14:00", TRUE);
  g_assert_true (dictate_calls_quiet_now (settings));
  g_setenv ("DICTATE_TEST_NOW", "22:00", TRUE);
  g_assert_false (dictate_calls_quiet_now (settings));
  g_settings_set_string (settings, "quiet-hours", "");
  g_assert_false (dictate_calls_quiet_now (settings));
  g_settings_set_string (settings, "quiet-hours", "nonsense");
  g_assert_false (dictate_calls_quiet_now (settings));
  g_unsetenv ("DICTATE_TEST_NOW");
}

static void
test_may_announce (void)
{
  g_autoptr (GSettings) settings = g_settings_new ("vibeos.Dictate");

  g_settings_set_string (settings, "quiet-hours", "22:00-07:00");
  g_setenv ("DICTATE_TEST_NOW", "12:00", TRUE);
  /* off by default: only with the window open */
  g_assert_false (g_settings_get_boolean (settings, "announce-calls"));
  g_assert_false (dictate_calls_may_announce (settings, FALSE));
  g_assert_true (dictate_calls_may_announce (settings, TRUE));
  g_settings_set_boolean (settings, "announce-calls", TRUE);
  g_assert_true (dictate_calls_may_announce (settings, FALSE));
  /* quiet hours: not even open, unless calls are let through */
  g_setenv ("DICTATE_TEST_NOW", "23:00", TRUE);
  g_assert_false (dictate_calls_may_announce (settings, TRUE));
  g_settings_set_boolean (settings, "quiet-calls", TRUE);
  g_assert_true (dictate_calls_may_announce (settings, FALSE));
  /* no access to calls: never */
  g_settings_set_boolean (settings, "access-calls", FALSE);
  g_assert_false (dictate_calls_may_announce (settings, TRUE));
  g_settings_reset (settings, "access-calls");
  g_settings_reset (settings, "quiet-calls");
  g_settings_reset (settings, "announce-calls");
  g_settings_reset (settings, "quiet-hours");
  g_unsetenv ("DICTATE_TEST_NOW");
}

static void
test_parse (void)
{
  g_autoptr (GVariant) calls = g_variant_ref_sink (g_variant_new_parsed (
    "[{'id': <uint32 1>, 'number': <'+44 7700 900101'>, 'presentation': <uint32 1>, 'direction': <uint32 1>, "
    "'state': <uint32 4>, 'connected': <int64 5>}, "
    "{'id': <uint32 2>, 'number': <''>, 'presentation': <uint32 3>, 'direction': <uint32 1>, 'state': <uint32 3>}, "
    "{'id': <uint32 3>, 'number': <'+44 7700 900201'>, 'state': <uint32 7>}]"));
  g_autoptr (GPtrArray) list = dictate_calls_parse (calls);

  /* the ended one left out */
  g_assert_cmpuint (list->len, ==, 2);
  g_assert_cmpuint (((DictateCall *) list->pdata[0])->state, ==, DICTATE_CALL_ACTIVE);
  g_assert_true (dictate_call_is_ringing (list->pdata[1]));
  g_assert_cmpstr (dictate_calls_context (list), ==, "a call is ringing");
  g_ptr_array_remove_index (list, 1);
  g_assert_cmpstr (dictate_calls_context (list), ==, "a call is going on");
  g_ptr_array_set_size (list, 0);
  g_assert_null (dictate_calls_context (list));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/calls/announcement", test_announcement);
  g_test_add_func ("/calls/quiet", test_quiet);
  g_test_add_func ("/calls/may-announce", test_may_announce);
  g_test_add_func ("/calls/parse", test_parse);
  return g_test_run ();
}
