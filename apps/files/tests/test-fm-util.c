/* Names and numbers as the screens show them. */
#include "fm-util.h"

#include <string.h>

static void
test_names (void)
{
  int end;
  g_autofree char *a = fm_copy_name ("report.pdf", FALSE, 1);
  g_autofree char *b = fm_copy_name ("report.pdf", FALSE, 3);
  g_autofree char *c = fm_copy_name ("Photos.2024", TRUE, 1);
  g_autofree char *d = fm_copy_name ("backup.tar.gz", FALSE, 1);
  g_autofree char *e = fm_copy_name (".bashrc", FALSE, 2);

  g_assert_cmpstr (a, ==, "report - Copy.pdf");
  g_assert_cmpstr (b, ==, "report - Copy (3).pdf");
  g_assert_cmpstr (c, ==, "Photos.2024 - Copy");
  g_assert_cmpstr (d, ==, "backup - Copy.tar.gz");
  g_assert_cmpstr (e, ==, ".bashrc - Copy (2)");

  fm_stem_range ("Fotó.jpeg", FALSE, &end);
  g_assert_cmpint (end, ==, 4);
  fm_stem_range ("folder.d", TRUE, &end);
  g_assert_cmpint (end, ==, 8);

  g_assert_cmpint (fm_check_name (""), ==, FM_NAME_EMPTY);
  g_assert_cmpint (fm_check_name ("  "), ==, FM_NAME_EMPTY);
  g_assert_cmpint (fm_check_name ("a/b"), ==, FM_NAME_BAD_CHARACTER);
  g_assert_cmpint (fm_check_name (".."), ==, FM_NAME_BAD_CHARACTER);
  g_assert_cmpint (fm_check_name ("a:b?"), ==, FM_NAME_OK);
  g_assert_nonnull (fm_name_check_message (FM_NAME_EMPTY));
  g_assert_null (fm_name_check_message (FM_NAME_OK));
}

static void
test_formats (void)
{
  g_autofree char *items = fm_format_items (5);
  g_autofree char *one = fm_format_items (1);
  g_autofree char *unknown = fm_format_items (FM_NAME_OK - 1);
  g_autofree char *perms = fm_format_permissions (0755);
  g_autofree char *time = fm_format_duration (3725);
  g_autofree char *size = fm_format_size (9800000000);
  g_autofree char *zero = fm_format_size (0);

  g_assert_cmpstr (items, ==, "5 items");
  g_assert_cmpstr (one, ==, "1 item");
  g_assert_cmpstr (unknown, ==, "- items");
  g_assert_cmpstr (perms, ==, "rwxr-xr-x");
  g_assert_cmpstr (time, ==, "1:02:05");
  g_assert_cmpstr (size, ==, "9.8\u00a0GB");  /* g_format_size keeps the number and unit together */
  g_assert_cmpstr (zero, ==, "0\u00a0B");
  g_assert_true (fm_name_matches ("Futurama S01", "futu"));
  g_assert_false (fm_name_matches ("Futurama", "zz"));
}

static void
test_paths (void)
{
  g_autoptr(GFile) root = g_file_new_for_uri ("smb://nas/");
  g_autoptr(GFile) video = g_file_new_for_uri ("smb://nas/VIDEO/a%20b");
  g_autoptr(GFile) local = g_file_new_for_path ("/home/user/x");
  g_autoptr(GFile) secret = g_file_new_for_uri ("ftp://me:pw@host/dir%20x/");
  g_autofree char *rel = fm_relative_path (root, video);
  g_autofree char *top = fm_relative_path (root, root);
  g_autofree char *shown = fm_display_path (local);
  g_autofree char *safe = fm_display_path (secret);

  /* GVfs lowers an smb share's name in its URIs */
  g_assert_cmpint (g_ascii_strcasecmp (rel, "/VIDEO/a b"), ==, 0);
  g_assert_cmpstr (top, ==, "/");
  g_assert_cmpstr (shown, ==, "/home/user/x");
  g_assert_null (strstr (safe, "pw"));
  {
    g_autoptr(GFile) up = fm_parent (video);
    g_autoptr(GFile) share = g_file_new_for_uri ("smb://nas/VIDEO/");
    g_autoptr(GFile) server = fm_parent (share);
    g_autoptr(GFile) none = fm_parent (root);
    g_autofree char *up_uri = g_file_get_uri (up);
    g_autofree char *server_uri = g_file_get_uri (server);

    g_assert_cmpint (g_ascii_strcasecmp (up_uri, "smb://nas/VIDEO/"), ==, 0);
    g_assert_cmpstr (server_uri, ==, "smb://nas/");
    g_assert_null (none);
    g_assert_true (fm_file_is_under (video, root));
    g_assert_true (fm_file_is_under (share, root));
    g_assert_false (fm_file_is_under (root, share));
  }
  g_assert_nonnull (strstr (safe, "dir x"));
}

static GFileInfo *
info_new (const char *name, GFileType type)
{
  GFileInfo *info = g_file_info_new ();

  g_file_info_set_name (info, name);
  g_file_info_set_display_name (info, name);
  g_file_info_set_file_type (info, type);
  return info;
}

static void
test_hidden (void)
{
  g_autoptr(GFileInfo) dot = info_new (".config", G_FILE_TYPE_DIRECTORY);
  g_autoptr(GFileInfo) plain = info_new ("Music", G_FILE_TYPE_DIRECTORY);
  g_autoptr(GFileInfo) ipc = info_new ("IPC$", G_FILE_TYPE_MOUNTABLE);
  g_autoptr(GFileInfo) share = info_new ("share", G_FILE_TYPE_MOUNTABLE);
  g_autoptr(GFileInfo) money = info_new ("price$", G_FILE_TYPE_REGULAR);

  g_assert_true (fm_info_is_hidden (dot));
  g_assert_false (fm_info_is_hidden (plain));
  /* a server's '$' shares are hidden, a file's '$' is only a character */
  g_assert_true (fm_info_is_hidden (ipc));
  g_assert_false (fm_info_is_hidden (share));
  g_assert_false (fm_info_is_hidden (money));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/util/names", test_names);
  g_test_add_func ("/util/formats", test_formats);
  g_test_add_func ("/util/paths", test_paths);
  g_test_add_func ("/util/hidden", test_hidden);
  return g_test_run ();
}
