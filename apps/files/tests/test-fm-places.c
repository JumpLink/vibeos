/* Which root a folder is under. */
#include "fm-places.h"

static void
test_places (void)
{
  g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func ((GDestroyNotify) fm_remote_free);
  FmRemote *plex = fm_remote_new (FM_PROTO_SMB);
  g_autofree char *home_uri = NULL;
  g_autoptr(GFile) music = g_file_new_build_filename (g_get_home_dir (), "Music", NULL);
  g_autoptr(GFile) etc = g_file_new_for_path ("/etc");
  g_autoptr(GFile) video = g_file_new_for_uri ("smb://plex/VIDEO/");
  g_autoptr(GFile) other = g_file_new_for_uri ("smb://nas/share/x/");
  g_autoptr(GFile) trash = g_file_new_for_uri ("trash:///a.txt");
  g_autoptr(GFile) net = g_file_new_for_uri ("network:///");
  g_autoptr(FmPlace) a = NULL, b = NULL, c = NULL, d = NULL, e = NULL, f = NULL;
  g_autofree char *root = NULL, *server = NULL;

  g_free (plex->id);
  plex->id = g_strdup ("remote-1");
  g_free (plex->name);
  plex->name = g_strdup ("PLEX");
  fm_remote_set_address (plex, "plex");
  g_ptr_array_add (remotes, plex);

  a = fm_place_find (music, remotes, NULL);
  g_assert_cmpint (a->kind, ==, FM_PLACE_STORAGE);
  g_assert_cmpint (a->glyph, ==, FM_GLYPH_STORAGE);
  b = fm_place_find (etc, remotes, NULL);
  g_assert_cmpint (b->kind, ==, FM_PLACE_SYSTEM);
  c = fm_place_find (video, remotes, NULL);
  g_assert_cmpint (c->kind, ==, FM_PLACE_REMOTE);
  g_assert_cmpstr (c->name, ==, "PLEX");
  g_assert_cmpstr (c->remote_id, ==, "remote-1");
  root = g_file_get_uri (c->root);
  g_assert_cmpstr (root, ==, "smb://plex/");
  d = fm_place_find (other, remotes, NULL);
  g_assert_cmpint (d->kind, ==, FM_PLACE_SERVER);
  g_assert_cmpstr (d->name, ==, "nas");
  server = g_file_get_uri (d->root);
  g_assert_cmpstr (server, ==, "smb://nas/");
  e = fm_place_find (trash, remotes, NULL);
  g_assert_cmpint (e->kind, ==, FM_PLACE_TRASH);
  f = fm_place_find (net, remotes, NULL);
  g_assert_cmpint (f->kind, ==, FM_PLACE_NETWORK);
  g_assert_true (fm_place_is_visitable (c));
  g_assert_false (fm_place_is_visitable (a));

  g_assert_cmpint (fm_glyph_for_file ("image/png", "a.png"), ==, FM_GLYPH_IMAGE);
  g_assert_cmpint (fm_glyph_for_file ("application/pdf", "a.pdf"), ==, FM_GLYPH_PDF);
  g_assert_cmpint (fm_glyph_for_file ("application/x-compressed-tar", "a.tar.gz"), ==, FM_GLYPH_ARCHIVE);
  g_assert_cmpint (fm_glyph_for_file ("application/vnd.debian.binary-package", "a.deb"), ==, FM_GLYPH_PACKAGE);
  g_assert_cmpint (fm_glyph_for_scheme ("sftp"), ==, FM_GLYPH_REMOTE_SFTP);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/places/find", test_places);
  return g_test_run ();
}
