/* A tablet's drawer sheet blurs as the shade does: the shade's verdict as shadelook.js writes it, and the blur for no
 * verdict or one not understood (the shade starts with the blur) */
#include "look.h"

#include <glib.h>
#include <glib/gstdio.h>

static char *dir;

static bool
blurs_with (const char *text)
{
  g_autofree char *path = g_build_filename (dir, "shade-look.json", NULL);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
  bool blur = look_blurs (path);
  g_unlink (path);
  return blur;
}

static void
test_none (void)
{
  g_autofree char *path = g_build_filename (dir, "shade-look.json", NULL);
  g_assert_true (look_blurs (path));
}

static void
test_verdict (void)
{
  /* as shadelook.js _save () writes it */
  g_assert_false (blurs_with ("{\n  \"version\": 1,\n  \"blur\": false,\n  \"p95\": 31.2,\n  \"mean\": 22.4,\n"
                              "  \"frames\": 64,\n  \"openings\": 3,\n  \"limit\": 20,\n"
                              "  \"at\": \"2026-11-26T00:00:00.000Z\",\n"
                              "  \"key\": {\n    \"kind\": \"tablet\"\n  }\n}"));
  g_assert_true (blurs_with ("{\"version\": 1, \"blur\": true, \"p95\": 16.1}"));
}

static void
test_not_understood (void)
{
  g_assert_true (blurs_with ("{\"version\": 2, \"blur\": false}"));
  g_assert_true (blurs_with ("{\"blur\": false}"));
  g_assert_true (blurs_with ("{\"version\": 1, \"blur\": \"no\"}"));
  g_assert_true (blurs_with ("{\"version\": 1}"));
  g_assert_true (blurs_with ("[false]"));
  g_assert_true (blurs_with ("{\"version\": 1, \"blur\": fal"));
  g_assert_true (blurs_with (""));
}

static void
test_path (void)
{
  g_autofree char *path = look_path ();
  g_autofree char *want = g_build_filename (g_get_user_state_dir (), "vibeos", "shade-look.json", NULL);
  g_assert_cmpstr (path, ==, want);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  dir = g_dir_make_tmp ("test-look-XXXXXX", NULL);
  g_assert_nonnull (dir);
  g_test_add_func ("/look/none", test_none);
  g_test_add_func ("/look/verdict", test_verdict);
  g_test_add_func ("/look/not-understood", test_not_understood);
  g_test_add_func ("/look/path", test_path);
  int status = g_test_run ();
  g_rmdir (dir);
  g_free (dir);
  return status;
}
