/* The saved remotes: addresses typed in, the URIs GVfs gets, and the key file (no password ever). */
#include "fm-remotes.h"

#include <glib/gstdio.h>
#include <string.h>

static void
test_uri (void)
{
  g_autoptr(FmRemote) smb = fm_remote_new (FM_PROTO_SMB);
  g_autoptr(FmRemote) sftp = fm_remote_new (FM_PROTO_SFTP);
  g_autoptr(FmRemote) dav = fm_remote_new (FM_PROTO_DAVS);
  g_autofree char *a = NULL, *b = NULL, *c = NULL, *d = NULL;

  fm_remote_set_address (smb, "plex");
  a = fm_remote_uri (smb);
  g_assert_cmpstr (a, ==, "smb://plex/");

  fm_remote_set_address (smb, "192.168.1.101/my share");
  g_free (smb->user);
  smb->user = g_strdup ("jo doe");
  g_free (smb->domain);
  smb->domain = g_strdup ("WORK");
  b = fm_remote_uri (smb);
  g_assert_cmpstr (b, ==, "smb://WORK;jo%20doe@192.168.1.101/my%20share/");
  smb->anonymous = TRUE;
  g_free (b);
  b = fm_remote_uri (smb);
  g_assert_cmpstr (b, ==, "smb://192.168.1.101/my%20share/");

  fm_remote_set_address (sftp, "127.0.0.1:2222/home/user");
  g_assert_cmpint (sftp->port, ==, 2222);
  g_assert_cmpstr (sftp->path, ==, "home/user");
  g_free (sftp->user);
  sftp->user = g_strdup ("user");
  c = fm_remote_uri (sftp);
  g_assert_cmpstr (c, ==, "sftp://user@127.0.0.1:2222/home/user/");

  /* the default port is left out */
  fm_remote_set_address (dav, "example.com:443/webdav/");
  d = fm_remote_uri (dav);
  g_assert_cmpstr (d, ==, "davs://example.com/webdav/");
}

static void
test_whole_uri (void)
{
  g_autoptr(FmRemote) remote = fm_remote_new (FM_PROTO_FTP);
  g_autofree char *address = NULL;

  fm_remote_set_address (remote, "smb://DOM;bob@nas/media/");
  g_assert_cmpint (remote->proto, ==, FM_PROTO_SMB);
  g_assert_cmpstr (remote->host, ==, "nas");
  g_assert_cmpstr (remote->path, ==, "media");
  g_assert_cmpstr (remote->user, ==, "bob");
  g_assert_cmpstr (remote->domain, ==, "DOM");
  address = fm_remote_get_address (remote);
  g_assert_cmpstr (address, ==, "nas/media");

  fm_remote_set_address (remote, "[fe80::1]/x");
  g_assert_cmpstr (remote->host, ==, "fe80::1");
  g_assert_cmpstr (remote->path, ==, "x");
}

static void
test_contains (void)
{
  g_autoptr(FmRemote) remote = fm_remote_new (FM_PROTO_SMB);
  g_autoptr(FmRemote) sftp = fm_remote_new (FM_PROTO_SFTP);
  g_autofree char *rest = NULL;

  fm_remote_set_address (remote, "plex");
  g_assert_true (fm_remote_contains (remote, "smb://plex/VIDEO/a%20b/", &rest));
  g_assert_cmpstr (rest, ==, "/VIDEO/a b");
  g_clear_pointer (&rest, g_free);
  g_assert_true (fm_remote_contains (remote, "smb://plex/", &rest));
  g_assert_cmpstr (rest, ==, "/");
  g_assert_false (fm_remote_contains (remote, "smb://other/", NULL));
  g_assert_false (fm_remote_contains (remote, "sftp://plex/", NULL));

  fm_remote_set_address (sftp, "host:2222/srv");
  g_free (sftp->user);
  sftp->user = g_strdup ("me");
  g_assert_true (fm_remote_contains (sftp, "sftp://me@host:2222/srv/x", NULL));
  g_assert_true (fm_remote_contains (sftp, "sftp://host:2222/srv", NULL));
  g_assert_false (fm_remote_contains (sftp, "sftp://you@host:2222/srv/x", NULL));
  g_assert_false (fm_remote_contains (sftp, "sftp://me@host/srv/x", NULL));
  g_assert_false (fm_remote_contains (sftp, "sftp://me@host:2222/srvx", NULL));
}

static void
test_key_file (void)
{
  g_autofree char *dir = g_dir_make_tmp ("fm-remotes-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "sub", "remotes.ini", NULL);
  g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func ((GDestroyNotify) fm_remote_free);
  g_autoptr(GPtrArray) loaded = NULL;
  FmRemote *remote = fm_remote_new (FM_PROTO_SFTP);
  g_autofree char *contents = NULL;
  g_autofree char *id = NULL;
  g_autoptr(GError) error = NULL;
  FmRemote *back;

  g_free (remote->id);
  remote->id = fm_remotes_new_id (remotes);
  g_free (remote->name);
  remote->name = g_strdup ("Test box");
  fm_remote_set_address (remote, "127.0.0.1:2222/tmp");
  g_free (remote->user);
  remote->user = g_strdup ("user");
  g_ptr_array_add (remotes, remote);
  g_assert_true (fm_remotes_save (remotes, path, &error));
  g_assert_no_error (error);

  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_null (strstr (contents, "password"));
  g_assert_nonnull (strstr (contents, "Test box"));

  loaded = fm_remotes_load (path);
  g_assert_cmpuint (loaded->len, ==, 1);
  back = loaded->pdata[0];
  g_assert_cmpstr (back->id, ==, remote->id);
  g_assert_cmpstr (back->name, ==, "Test box");
  g_assert_cmpint (back->proto, ==, FM_PROTO_SFTP);
  g_assert_cmpstr (back->host, ==, "127.0.0.1");
  g_assert_cmpint (back->port, ==, 2222);
  g_assert_cmpstr (back->path, ==, "tmp");
  g_assert_cmpstr (back->user, ==, "user");

  id = fm_remotes_new_id (loaded);
  g_assert_cmpstr (id, !=, back->id);

  g_remove (path);
  g_autofree char *sub = g_path_get_dirname (path);
  g_rmdir (sub);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/remotes/uri", test_uri);
  g_test_add_func ("/remotes/whole-uri", test_whole_uri);
  g_test_add_func ("/remotes/contains", test_contains);
  g_test_add_func ("/remotes/key-file", test_key_file);
  return g_test_run ();
}
