/* The launcher reads the desktop ids an older install left behind (legacy-ids.h: gnome-phone's, up to 0.1.6 and from
 * 0.1.7 to 0.1.11) as the new ones: in the user's layout, in a ROM's default layout (/etc/xdg) and in
 * hidden-from-home. An app still installed under an old id (another product's, a ROM's own entry) keeps its id and
 * its cell. */
#include "apps.h"
#include "legacy-ids.h"
#include "model.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define OLD(name) LEGACY_PREFIX name ".desktop"   /* up to 0.1.6 */
#define NEO(name) NEO_PREFIX name ".desktop"      /* 0.1.7 to 0.1.11 */

static char *root;

static void
write_file (const char *path, const char *text)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static void
desktop (const char *data_dir, const char *id)
{
  g_autofree char *path = g_build_filename (data_dir, "applications", id, NULL);
  g_autofree char *text = g_strdup_printf ("[Desktop Entry]\nType=Application\nName=%s\nExec=true\n", id);
  write_file (path, text);
}

static const char *
cell (GPtrArray *list, guint i)
{
  return i < list->len ? list->pdata[i] : NULL;
}

/* a home layout, 5 × 5: the dock, and the first page's first row left to the widgets */
static char *
layout (void)
{
  return g_strdup_printf ("{\"columns\": 5, \"rows\": 5,\n"
                          " \"dock\": [\"%s\", \"%s\", null, \"org.gnome.Console.desktop\", \"%s\"],\n"
                          " \"pages\": [[null, null, null, null, null, \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"],\n"
                          "            [\"%s\", \"%s\"]]}\n",
                          OLD ("Dialer"), NEO ("Contacts"), OLD ("Messaging"), NEO ("Camera"), OLD ("NeoBrowser"),
                          OLD ("app"), OLD ("BootAndroid"), LEGACY_SHORTCUT_PREFIX "7.desktop",
                          NEO ("Dictate"), NEO_SHORTCUT_PREFIX "8.desktop");
}

static Model *
load (void)
{
  g_autoptr (GHashTable) apps = apps_installed ();
  Model *m = model_new (5, 5, 5);
  model_set_reserved (m, 5);
  char *hidden[] = { (char *) OLD ("Clock"), (char *) NEO ("Compass"), NULL };
  model_set_hidden (m, hidden);
  model_load (m, apps);
  return m;
}

static void
check_home (Model *m)
{
  g_assert_cmpstr (cell (m->dock, 0), ==, "vibeos.Dialer.desktop");
  g_assert_cmpstr (cell (m->dock, 1), ==, "vibeos.Contacts.desktop");
  g_assert_null (cell (m->dock, 2));
  g_assert_cmpstr (cell (m->dock, 3), ==, "org.gnome.Console.desktop");
  g_assert_cmpstr (cell (m->dock, 4), ==, "vibeos.Messaging.desktop");
  GPtrArray *page = m->pages->pdata[0];
  g_assert_cmpstr (cell (page, 5), ==, "vibeos.Camera.desktop");
  g_assert_cmpstr (cell (page, 6), ==, "vibeos.Browser.desktop");
  /* installed under the old prefix: not ours to rename */
  g_assert_cmpstr (cell (page, 7), ==, OLD ("app"));
  g_assert_cmpstr (cell (page, 8), ==, OLD ("BootAndroid"));
  /* a browser shortcut vibeos-migrate-ids renamed */
  g_assert_cmpstr (cell (page, 9), ==, SHORTCUT_PREFIX "7.desktop");
  GPtrArray *second = m->pages->pdata[1];
  g_assert_cmpstr (cell (second, 0), ==, "vibeos.Dictate.desktop");
  g_assert_cmpstr (cell (second, 1), ==, SHORTCUT_PREFIX "8.desktop");
  /* hidden-from-home's old ids hide the renamed apps */
  g_assert_true (g_hash_table_contains (m->hidden, "vibeos.Clock.desktop"));
  g_assert_false (model_on_home (m, "vibeos.Clock.desktop"));
  g_assert_false (model_on_home (m, OLD ("Clock")));
  g_assert_true (g_hash_table_contains (m->hidden, "vibeos.Compass.desktop"));
}

static void
test_current_id (void)
{
  g_autofree char *dialer = apps_current_id (OLD ("Dialer"));
  g_autofree char *browser = apps_current_id (OLD ("NeoBrowser"));
  g_autofree char *gjs_browser = apps_current_id (OLD ("Browser"));
  g_autofree char *other = apps_current_id (OLD ("app"));
  g_autofree char *rom = apps_current_id (OLD ("BootAndroid"));
  g_autofree char *weather = apps_current_id (OLD ("NeoWeather"));   /* not installed in either name */
  g_autofree char *gnome = apps_current_id ("org.gnome.Console.desktop");
  g_autofree char *neo_dialer = apps_current_id (NEO ("Dialer"));
  g_autofree char *neo_dictate = apps_current_id (NEO ("Dictate"));
  g_autofree char *neo_weather = apps_current_id (NEO ("Weather"));     /* not installed in either name */
  g_autofree char *neo_shortcut = legacy_desktop_id (NEO_SHORTCUT_PREFIX "8.desktop");
  g_autofree char *unknown = legacy_desktop_id (NEO ("Nothing"));
  g_assert_cmpstr (dialer, ==, "vibeos.Dialer.desktop");
  g_assert_cmpstr (browser, ==, "vibeos.Browser.desktop");
  g_assert_cmpstr (gjs_browser, ==, "vibeos.Browser.desktop");
  g_assert_cmpstr (other, ==, OLD ("app"));
  g_assert_cmpstr (rom, ==, OLD ("BootAndroid"));
  g_assert_cmpstr (weather, ==, OLD ("NeoWeather"));
  g_assert_cmpstr (gnome, ==, "org.gnome.Console.desktop");
  g_assert_cmpstr (neo_dialer, ==, "vibeos.Dialer.desktop");
  g_assert_cmpstr (neo_dictate, ==, "vibeos.Dictate.desktop");
  g_assert_cmpstr (neo_weather, ==, NEO ("Weather"));
  g_assert_cmpstr (neo_shortcut, ==, SHORTCUT_PREFIX "8.desktop");
  g_assert_null (unknown);
}

static void
test_user_layout (void)
{
  g_autofree char *file = g_build_filename (root, "config", "vibeos-launcher", "layout.json", NULL);
  g_autofree char *text = layout ();
  write_file (file, text);
  Model *m = load ();
  check_home (m);
  model_free (m);
  g_unlink (file);
}

static void
test_default_layout (void)
{
  /* the first start: no layout of the user's, a ROM's in /etc/xdg with the old ids */
  g_autofree char *mine = g_build_filename (root, "config", "vibeos-launcher", "layout.json", NULL);
  g_autofree char *rom = g_build_filename (root, "etc-xdg", "vibeos-launcher", "layout.json", NULL);
  g_autofree char *text = layout ();
  write_file (rom, text);
  Model *m = load ();
  check_home (m);
  model_free (m);
  /* the user's own layout from now on, in the new ids */
  g_autofree char *saved = NULL;
  g_assert_true (g_file_get_contents (mine, &saved, NULL, NULL));
  g_assert_nonnull (strstr (saved, "\"vibeos.Dialer.desktop\""));
  g_assert_null (strstr (saved, OLD ("Dialer")));
  g_assert_null (strstr (saved, NEO ("Contacts")));
  g_unlink (mine);
  g_unlink (rom);
}

int
main (int argc, char **argv)
{
  root = g_dir_make_tmp ("test-legacy-ids-XXXXXX", NULL);
  g_assert_nonnull (root);
  g_autofree char *share = g_build_filename (root, "share", NULL);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autofree char *config = g_build_filename (root, "config", NULL);
  g_autofree char *etc = g_build_filename (root, "etc-xdg", NULL);
  /* before GIO reads a desktop file */
  g_setenv ("XDG_DATA_DIRS", share, TRUE);
  g_setenv ("XDG_DATA_HOME", data, TRUE);
  g_setenv ("XDG_CONFIG_HOME", config, TRUE);
  g_setenv ("XDG_CONFIG_DIRS", etc, TRUE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

  const char *installed[] = { "vibeos.Dialer.desktop", "vibeos.Contacts.desktop", "vibeos.Messaging.desktop",
                              "vibeos.Camera.desktop", "vibeos.Browser.desktop", "vibeos.Clock.desktop",
                              "vibeos.Compass.desktop", "vibeos.Dictate.desktop",
                              "org.gnome.Console.desktop", OLD ("app"), OLD ("BootAndroid") };
  for (guint i = 0; i < G_N_ELEMENTS (installed); i++)
    desktop (share, installed[i]);
  desktop (data, SHORTCUT_PREFIX "7.desktop");
  desktop (data, SHORTCUT_PREFIX "8.desktop");

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/legacy-ids/current-id", test_current_id);
  g_test_add_func ("/legacy-ids/user-layout", test_user_layout);
  g_test_add_func ("/legacy-ids/default-layout", test_default_layout);
  return g_test_run ();
}
