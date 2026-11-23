/* test-access: what Agent's model is told it can use, as the settings' switches are (dictate-access.h) */
#include "dictate-access.h"

#include <string.h>

static GSettings *
fresh_settings (void)
{
  GSettings *settings = g_settings_new ("vibeos.Dictate");
  guint n;
  const DictateAccess *access = dictate_access_list (&n);

  for (guint i = 0; i < n; i++)
    g_settings_reset (settings, access[i].key);
  return settings;
}

static void
test_all_on (void)
{
  g_autoptr (GSettings) settings = fresh_settings ();
  g_autofree char *prompt = dictate_access_prompt (settings);
  guint n;
  const DictateAccess *access = dictate_access_list (&n);

  g_assert_cmpuint (n, ==, 11);
  for (guint i = 0; i < n; i++)
    g_assert_true (g_settings_get_boolean (settings, access[i].key));
  g_assert_true (g_str_has_prefix (prompt, "You have tools for everything on the phone: calls, messages, contacts, "));
  g_assert_nonnull (strstr (prompt, "the camera, maps, navigation and the phone's location, and the web. "));
  g_assert_nonnull (strstr (prompt, "your alarm is set for seven"));
  g_assert_null (strstr (prompt, "no access"));
}

static void
test_some_off (void)
{
  g_autoptr (GSettings) settings = fresh_settings ();
  g_autofree char *prompt = NULL;

  g_settings_set_boolean (settings, "access-calls", FALSE);
  g_settings_set_boolean (settings, "access-web", FALSE);
  prompt = dictate_access_prompt (settings);
  g_test_message ("%s", prompt);
  g_assert_true (g_str_has_prefix (prompt, "You have tools for these things on the phone: messages, contacts, "));
  g_assert_null (strstr (prompt, "tools for these things on the phone: calls"));
  g_assert_nonnull (strstr (prompt, "and the phone's location. When"));
  g_assert_nonnull (strstr (prompt, " You have no access to calls and the web, because the user has turned that off"));
  g_assert_nonnull (strstr (prompt, "it can be turned on in Agent's settings."));
}

static void
test_alarms_off (void)
{
  g_autoptr (GSettings) settings = fresh_settings ();
  g_autofree char *prompt = NULL;

  g_settings_set_boolean (settings, "access-alarms", FALSE);
  prompt = dictate_access_prompt (settings);
  g_assert_null (strstr (prompt, "your alarm is set"));
  g_assert_nonnull (strstr (prompt, "\"Okay, that's done.\""));
  g_assert_nonnull (strstr (prompt, "You have no access to alarms and timers, because"));
}

static void
test_all_off (void)
{
  g_autoptr (GSettings) settings = fresh_settings ();
  g_autofree char *prompt = NULL;
  guint n;
  const DictateAccess *access = dictate_access_list (&n);

  for (guint i = 0; i < n; i++)
    g_settings_set_boolean (settings, access[i].key, FALSE);
  prompt = dictate_access_prompt (settings);
  g_assert_true (g_str_has_prefix (prompt, "You have no access to anything on the phone: calls, messages, "));
  g_assert_null (strstr (prompt, "You have tools"));
  g_assert_nonnull (strstr (prompt, ", and the web, because"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/access/all-on", test_all_on);
  g_test_add_func ("/access/some-off", test_some_off);
  g_test_add_func ("/access/alarms-off", test_alarms_off);
  g_test_add_func ("/access/all-off", test_all_off);
  return g_test_run ();
}
