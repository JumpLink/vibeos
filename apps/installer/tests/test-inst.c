/* App Installer's core without PackageKit: Debian version comparison (checked against dpkg --compare-versions too),
 * the Description field, and the .deb reader on packages built here with dpkg-deb: the fields, the app's desktop
 * file and its icon (the largest, the scalable one first, through a symlink, under a name longer than tar's 100
 * characters). */
#include "inst-deb.h"
#include "inst-version.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* a, b, and how a compares to b */
static const struct { const char *a, *b; int result; } versions[] = {
  { "1.0", "1.0", 0 },
  { "1.0", "1.00", 0 },
  { "0:1.0", "1.0", 0 },
  { "1.0-0", "1.0", 0 },             /* an empty revision is "0" */
  { "1.2", "1.3", -1 },
  { "1.10", "1.9", 1 },              /* numbers, not characters */
  { "1.0~rc1", "1.0", -1 },          /* the tilde sorts before anything, even the end */
  { "1.0~rc1", "1.0~rc2", -1 },
  { "1.0~~", "1.0~", -1 },
  { "1.0+b1", "1.0", 1 },
  { "1.0a", "1.0", 1 },
  { "1.0a", "1.0+", -1 },            /* letters before other characters */
  { "1:0.9", "2.0", 1 },             /* the epoch first */
  { "2:1.0", "10:0.1", -1 },
  { "1.0-1", "1.0-2", -1 },
  { "1.0-10", "1.0-9", 1 },
  { "1.2-3ubuntu1", "1.2-3", 1 },
  { "1.2-3~bpo1", "1.2-3", -1 },
  { "1.0.0", "1.0", 1 },
  { "1.001", "1.1", 0 },             /* leading zeroes do not count */
  { "1:1.0-1-1", "1:1.0-1-2", -1 },  /* the revision is after the last hyphen */
  { "0.9", "", 1 },
  { NULL, "1", -1 },
};

static void
test_versions (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (versions); i++)
    {
      int r = inst_version_compare (versions[i].a, versions[i].b);
      int back = inst_version_compare (versions[i].b, versions[i].a);

      if (r != versions[i].result)
        g_error ("%s vs %s: %d, expected %d", versions[i].a, versions[i].b, r, versions[i].result);
      g_assert_cmpint (back, ==, -versions[i].result);
    }
}

static int
compare_versions (gconstpointer a, gconstpointer b)
{
  return inst_version_compare (*(const char *const *) a, *(const char *const *) b);
}

/* more versions, sorted by inst_version_compare, and each one checked against the next by dpkg itself (when it is
 * there): the order is dpkg's only if every neighbour is */
static void
test_versions_dpkg (void)
{
  const char *more[] = { "1.0", "1.0~", "1.0~~", "1.0+", "1.0a", "1.0-1", "1:1.0", "1.0.1", "1.0~b", "2.0~beta3",
                         "2.0~beta10", "2.0", "2.0-0.1", "2.0-1~exp1", "0.0", "a", "a~", "1.2.3-4+deb12u1",
                         "1.2.3-4+deb12u10", "7.4p1", "7.4", "1.00", "0:1.0-0" };
  g_autofree char *dpkg = g_find_program_in_path ("dpkg");

  if (!dpkg)
    {
      g_test_skip ("no dpkg");
      return;
    }
  qsort (more, G_N_ELEMENTS (more), sizeof more[0], compare_versions);
  for (guint i = 0; i + 1 < G_N_ELEMENTS (more); i++)
    {
      gboolean same = inst_version_compare (more[i], more[i + 1]) == 0;
      const char *argv[] = { dpkg, "--compare-versions", more[i], same ? "eq" : "lt", more[i + 1], NULL };
      int status = 0;

      g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL,
                                   &status, NULL));
      if (!g_spawn_check_wait_status (status, NULL))
        g_error ("dpkg does not agree that %s %s %s", more[i], same ? "eq" : "lt", more[i + 1]);
    }
}

static void
test_description (void)
{
  g_autofree char *summary = NULL, *description = NULL;

  inst_deb_split_description ("A small app\n"
                              " It does one thing\n"
                              " and does it well.\n"
                              " .\n"
                              " Features:\n"
                              "  - fast\n"
                              "  - small",
                              &summary, &description);
  g_assert_cmpstr (summary, ==, "A small app");
  g_assert_cmpstr (description, ==, "It does one thing and does it well.\n\nFeatures:\n- fast\n- small");
}

/* ---- .deb files built here ------------------------------------------------------------------------------------ */

static void
write_file (const char *root, const char *path, const char *contents, gsize length)
{
  g_autofree char *full = g_build_filename (root, path, NULL);
  g_autofree char *dir = g_path_get_dirname (full);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (full, contents, length, NULL));
}

static void
link_file (const char *root, const char *path, const char *target)
{
  g_autofree char *full = g_build_filename (root, path, NULL);
  g_autofree char *dir = g_path_get_dirname (full);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_cmpint (symlink (target, full), ==, 0);
}

static const char png_signature[] = "\x89PNG\r\n\x1a\n";

/* builds a package from the tree `root` into `deb`; FALSE without dpkg-deb */
static gboolean
build (const char *root, const char *deb)
{
  g_autofree char *dpkg_deb = g_find_program_in_path ("dpkg-deb");
  const char *argv[] = { dpkg_deb, "--root-owner-group", "-Zgzip", "--build", root, deb, NULL };
  int status = 0;

  if (!dpkg_deb)
    return FALSE;
  g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL,
                               &status, NULL));
  g_assert_true (g_spawn_check_wait_status (status, NULL));
  return TRUE;
}

static void
test_deb_app (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("inst-test-XXXXXX", NULL);
  g_autofree char *root = g_build_filename (tmp, "root", NULL);
  g_autofree char *deb_path = g_build_filename (tmp, "notes_1.3-1_all.deb", NULL);
  g_autofree char *long_dir = g_strnfill (120, 'd');
  g_autofree char *long_name = g_strconcat ("usr/share/doc/notes/", long_dir, "/README", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (InstDeb) deb = NULL;
  gsize length;
  const char *data;

  write_file (root, "DEBIAN/control",
              "Package: notes\n"
              "Version: 1:1.3-1\n"
              "Architecture: all\n"
              "Maintainer: Someone <someone@example.org>\n"
              "Installed-Size: 120\n"
              "Homepage: https://example.org/notes\n"
              "Description: Take notes\n"
              " Write things down.\n", -1);
  /* two desktop files: a hidden helper, and the app */
  write_file (root, "usr/share/applications/notes-helper.desktop",
              "[Desktop Entry]\nType=Application\nName=Helper\nExec=notes-helper\nNoDisplay=true\n", -1);
  write_file (root, "usr/share/applications/org.example.Notes.desktop",
              "[Desktop Entry]\nType=Application\nName=Notes\nName[de]=Notizen\nExec=notes %U\n"
              "Icon=org.example.Notes\n", -1);
  /* a 48 px PNG, a scalable SVG (preferred), and a 256 px PNG of another theme reached by a symlink */
  write_file (root, "usr/share/icons/hicolor/48x48/apps/org.example.Notes.png", png_signature, 8);
  write_file (root, "usr/share/icons/hicolor/scalable/apps/org.example.Notes.svg",
              "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'/>", -1);
  write_file (root, "usr/share/notes/big.png", png_signature, 8);
  link_file (root, "usr/share/icons/Other/256x256/apps/org.example.Notes.png", "../../../../notes/big.png");
  write_file (root, long_name, "long", -1);

  if (!build (root, deb_path))
    {
      g_test_skip ("no dpkg-deb");
      return;
    }
  deb = inst_deb_inspect (deb_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (deb->package, ==, "notes");
  g_assert_cmpstr (deb->version, ==, "1:1.3-1");
  g_assert_cmpstr (deb->architecture, ==, "all");
  g_assert_cmpstr (deb->summary, ==, "Take notes");
  g_assert_cmpstr (deb->description, ==, "Write things down.");
  g_assert_cmpstr (deb->homepage, ==, "https://example.org/notes");
  g_assert_cmpuint (deb->installed_size, ==, 120 * 1024);
  g_assert_cmpuint (deb->file_size, >, 0);
  g_assert_cmpstr (deb->app_name, ==, "Notes");
  g_assert_cmpstr (deb->desktop_path, ==, "/usr/share/applications/org.example.Notes.desktop");
  g_assert_nonnull (deb->icon);
  g_assert_cmpstr (deb->icon_type, ==, "svg");
  data = g_bytes_get_data (deb->icon, &length);
  g_assert_true (g_str_has_prefix (data, "<svg"));

  /* the scalable icon gone: the largest bitmap, through its symlink */
  {
    g_autofree char *svg = g_build_filename (root, "usr/share/icons/hicolor/scalable/apps/org.example.Notes.svg",
                                             NULL);
    g_autoptr (InstDeb) again = NULL;

    g_assert_cmpint (g_unlink (svg), ==, 0);
    g_assert_cmpint (g_unlink (deb_path), ==, 0);
    build (root, deb_path);
    again = inst_deb_inspect (deb_path, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (again->icon_type, ==, "png");
  }

  /* the second check used the 256 px one: same bytes as big.png (the 48 px one has the same, so look at the size
   * order through a third build with a distinct big icon) */
  {
    g_autoptr (InstDeb) third = NULL;
    static const char big[] = "\x89PNG\r\n\x1a\nBIG";

    write_file (root, "usr/share/notes/big.png", big, sizeof big - 1);
    g_assert_cmpint (g_unlink (deb_path), ==, 0);
    build (root, deb_path);
    third = inst_deb_inspect (deb_path, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (g_bytes_get_size (third->icon), ==, sizeof big - 1);
  }
}

static void
test_deb_no_app (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("inst-test-XXXXXX", NULL);
  g_autofree char *root = g_build_filename (tmp, "root", NULL);
  g_autofree char *deb_path = g_build_filename (tmp, "libthing.deb", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (InstDeb) deb = NULL;

  write_file (root, "DEBIAN/control",
              "Package: libthing1\nVersion: 2.0\nArchitecture: arm64\nMaintainer: X <x@example.org>\n"
              "Description: A library\n", -1);
  write_file (root, "usr/lib/libthing.so.1", "elf", -1);
  if (!build (root, deb_path))
    {
      g_test_skip ("no dpkg-deb");
      return;
    }
  deb = inst_deb_inspect (deb_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (deb->package, ==, "libthing1");
  g_assert_null (deb->app_name);
  g_assert_null (deb->icon);
  g_assert_cmpstr (deb->description, ==, "");
}

static void
test_not_a_deb (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("inst-test-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (tmp, "fake.deb", NULL);
  g_autoptr (GError) error = NULL;
  InstDeb *deb;

  if (!g_find_program_in_path ("dpkg-deb"))
    {
      g_test_skip ("no dpkg-deb");
      return;
    }
  g_assert_true (g_file_set_contents (path, "this is not an ar archive", -1, NULL));
  deb = inst_deb_inspect (path, NULL, &error);
  g_assert_null (deb);
  g_assert_error (error, INST_DEB_ERROR, INST_DEB_ERROR_NOT_A_PACKAGE);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "C");
  g_setenv ("LANGUAGE", "C", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/version/compare", test_versions);
  g_test_add_func ("/version/dpkg", test_versions_dpkg);
  g_test_add_func ("/deb/description", test_description);
  g_test_add_func ("/deb/app", test_deb_app);
  g_test_add_func ("/deb/no-app", test_deb_no_app);
  g_test_add_func ("/deb/not-a-deb", test_not_a_deb);
  return g_test_run ();
}
