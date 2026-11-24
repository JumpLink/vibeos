/* call-recording: the recordings' file names, the default folder, how the folder is shown, and the settings
 * (GSETTINGS_SCHEMA_DIR: the build's compiled schema; GSETTINGS_BACKEND=memory) */
#include "../src/call-recording.h"

#include <glib/gstdio.h>
#include <string.h>

static GDateTime *
at (int hour, int minute, double seconds)
{
  g_autoptr(GTimeZone) tz = g_time_zone_new_identifier ("UTC");

  return g_date_time_new (tz, 2026, 11, 24, hour, minute, seconds);
}

static void
test_name (void)
{
  g_autoptr(GDateTime) when = at (14, 3, 22.123);
  g_autoptr(GDateTime) midnight = at (0, 0, 0);
  g_autofree char *contact = call_recording_file_name ("Anna Schmidt", when);
  g_autofree char *number = call_recording_file_name ("+447700900123", when);
  g_autofree char *zero = call_recording_file_name ("+447700900123", midnight);

  /* Lineage's order: the other side, then yyMMdd_HHmmssSSS */
  g_assert_cmpstr (contact, ==, "Anna Schmidt_261124_140322123.opus");
  g_assert_cmpstr (number, ==, "+447700900123_261124_140322123.opus");
  g_assert_cmpstr (zero, ==, "+447700900123_261124_000000000.opus");
}

static void
test_unknown (void)
{
  g_autoptr(GDateTime) when = at (9, 5, 7);
  g_autofree char *none = call_recording_file_name (NULL, when);
  g_autofree char *empty = call_recording_file_name ("", when);
  g_autofree char *dots = call_recording_file_name (" .. ", when);

  /* Lineage's word for a call with no number; nothing left after the cleaning is the same */
  g_assert_cmpstr (none, ==, "unknown_261124_090507000.opus");
  g_assert_cmpstr (empty, ==, "unknown_261124_090507000.opus");
  g_assert_cmpstr (dots, ==, "unknown_261124_090507000.opus");
}

static void
test_sanitized (void)
{
  g_autoptr(GDateTime) when = at (14, 3, 22);
  g_autofree char *slashes = call_recording_file_name ("../etc/passwd", when);
  g_autofree char *fat = call_recording_file_name ("A\\B:C*D?E\"F<G>H|I", when);
  g_autofree char *control = call_recording_file_name ("Line\none\ttab", when);
  g_autofree char *override = call_recording_file_name ("Evil" "\xe2\x80\xae" "gpj.exe", when);
  g_autofree char *hidden = call_recording_file_name (".hidden", when);
  g_autofree char *trailing = call_recording_file_name ("Dr. Who. ", when);
  g_autofree char *broken = call_recording_file_name ("caf\xc3", when);
  g_autofree char *unicode = call_recording_file_name ("Иван Петров", when);

  g_assert_cmpstr (slashes, ==, "_etc_passwd_261124_140322000.opus");
  g_assert_cmpstr (fat, ==, "A_B_C_D_E_F_G_H_I_261124_140322000.opus");
  g_assert_cmpstr (control, ==, "Line_one_tab_261124_140322000.opus");
  /* an invisible right-to-left override would make the name read "Evilexe.jpg" */
  g_assert_cmpstr (override, ==, "Evil_gpj.exe_261124_140322000.opus");
  g_assert_cmpstr (hidden, ==, "hidden_261124_140322000.opus");
  g_assert_cmpstr (trailing, ==, "Dr. Who_261124_140322000.opus");
  g_assert_true (g_utf8_validate (broken, -1, NULL));
  g_assert_true (g_str_has_prefix (broken, "caf"));
  g_assert_cmpstr (unicode, ==, "Иван Петров_261124_140322000.opus");
}

static void
test_long (void)
{
  g_autoptr(GDateTime) when = at (14, 3, 22);
  g_autoptr(GString) party = g_string_new (NULL);
  g_autofree char *name = NULL;
  const char *suffix = "_261124_140322000.opus";
  size_t part;

  /* two-byte letters, so a cut at a byte count lands inside one */
  for (int i = 0; i < 120; i++)
    g_string_append (party, "ä");
  name = call_recording_file_name (party->str, when);
  g_assert_true (g_utf8_validate (name, -1, NULL));
  g_assert_true (g_str_has_suffix (name, suffix));
  part = strlen (name) - strlen (suffix);
  g_assert_cmpuint (part, <=, 100);
  g_assert_cmpuint (part, ==, 100);     /* 50 whole letters */
  g_assert_cmpuint (strlen (name), <, 255);
}

static void
test_default_folder (void)
{
  g_autofree char *folder = call_recording_default_folder ();
  g_autofree char *expected = g_build_filename (g_get_home_dir (), "Recordings", "Call recordings", NULL);

  g_assert_cmpstr (folder, ==, expected);
}

static void
test_display (void)
{
  g_autofree char *standard = call_recording_default_folder ();
  g_autofree char *shown = call_recording_folder_display (standard);
  g_autofree char *home = call_recording_folder_display (g_get_home_dir ());
  g_autofree char *card = call_recording_folder_display ("/media/card/Calls");
  g_autofree char *beside = g_strconcat (g_get_home_dir (), "2/Calls", NULL);
  g_autofree char *beside_shown = call_recording_folder_display (beside);

  /* the home folder as Files names it (the tests run untranslated) */
  g_assert_cmpstr (shown, ==, "Home/Recordings/Call recordings");
  g_assert_cmpstr (home, ==, "Home");
  g_assert_cmpstr (card, ==, "/media/card/Calls");
  /* a folder whose name only starts like the home folder's is not in it */
  g_assert_cmpstr (beside_shown, ==, beside);
}

static void
test_settings (void)
{
  g_autoptr(GSettings) settings = g_settings_new ("vibeos.Dialer");
  g_autofree char *standard = call_recording_default_folder ();
  g_autofree char *folder = NULL, *relative = NULL, *chosen = NULL;

  /* off, announced, the default folder */
  g_assert_false (g_settings_get_boolean (settings, "record-calls"));
  g_assert_true (g_settings_get_boolean (settings, "announce-recording"));
  folder = call_recording_folder (settings);
  g_assert_cmpstr (folder, ==, standard);

  /* a relative path is not a folder the service can find: the default */
  g_settings_set_string (settings, "recordings-folder", "Calls");
  relative = call_recording_folder (settings);
  g_assert_cmpstr (relative, ==, standard);

  g_settings_set_string (settings, "recordings-folder", "/media/card/Calls");
  chosen = call_recording_folder (settings);
  g_assert_cmpstr (chosen, ==, "/media/card/Calls");
  g_settings_reset (settings, "recordings-folder");
}

/* the announcement in the first of the user's languages it is there in, else in English */
static void
test_announcement (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("test-call-recording-XXXXXX", NULL);
  g_autofree char *empty = g_build_filename (tmp, "empty", NULL);
  g_autofree char *data = g_build_filename (tmp, "data", NULL);
  g_autofree char *app = g_build_filename (data, "vibeos-dialer", NULL);
  g_autofree char *sounds = g_build_filename (app, "sounds", NULL);
  const char *dirs[] = { empty, data, NULL };
  const char *none[] = { empty, NULL };
  const char *french[] = { "fr_FR.UTF-8", "fr_FR", "fr", "C", NULL };
  const char *dutch[] = { "nl_NL.UTF-8", "nl_NL", "nl", "C", NULL };
  const char *second[] = { "pt_BR.UTF-8@euro", "pt_BR", "pt", "de_DE", "de", "C", NULL };
  const char *posix[] = { "C", NULL };
  g_autofree char *fr = NULL, *nl = NULL, *de = NULL, *c = NULL, *gone = NULL;

  g_assert_no_errno (g_mkdir_with_parents (sounds, 0700));
  g_assert_no_errno (g_mkdir_with_parents (empty, 0700));
  for (const char **lang = (const char *[]) { "en", "de", "fr", NULL }; *lang; lang++)
    {
      g_autofree char *name = g_strdup_printf ("call-recording-%s.opus", *lang);
      g_autofree char *path = g_build_filename (sounds, name, NULL);

      g_assert_true (g_file_set_contents (path, "OggS", -1, NULL));
    }
  fr = call_recording_announcement (french, dirs);
  nl = call_recording_announcement (dutch, dirs);
  de = call_recording_announcement (second, dirs);
  c = call_recording_announcement (posix, dirs);
  gone = call_recording_announcement (french, none);
  g_assert_true (g_str_has_suffix (fr, "/data/vibeos-dialer/sounds/call-recording-fr.opus"));
  /* no Dutch one: English */
  g_assert_true (g_str_has_suffix (nl, "/call-recording-en.opus"));
  /* no Portuguese one: the user's next language, before English */
  g_assert_true (g_str_has_suffix (de, "/call-recording-de.opus"));
  g_assert_true (g_str_has_suffix (c, "/call-recording-en.opus"));
  g_assert_null (gone);
  for (const char **lang = (const char *[]) { "en", "de", "fr", NULL }; *lang; lang++)
    {
      g_autofree char *name = g_strdup_printf ("call-recording-%s.opus", *lang);
      g_autofree char *path = g_build_filename (sounds, name, NULL);

      g_unlink (path);
    }
  g_rmdir (sounds);
  g_rmdir (app);
  g_rmdir (data);
  g_rmdir (empty);
  g_rmdir (tmp);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/call-recording/name", test_name);
  g_test_add_func ("/call-recording/unknown", test_unknown);
  g_test_add_func ("/call-recording/sanitized", test_sanitized);
  g_test_add_func ("/call-recording/long", test_long);
  g_test_add_func ("/call-recording/default-folder", test_default_folder);
  g_test_add_func ("/call-recording/display", test_display);
  g_test_add_func ("/call-recording/settings", test_settings);
  g_test_add_func ("/call-recording/announcement", test_announcement);
  return g_test_run ();
}
