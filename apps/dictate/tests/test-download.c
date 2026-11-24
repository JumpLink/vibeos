/* test-download: what dictate-download.c unpacks and removes, and its table of packs. Nothing is downloaded: the
 * archives are made here, with libarchive, the bad ones as an attacker would make them. */
#include "dictate-download.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  const char *path;
  mode_t type;          /* AE_IFREG, AE_IFDIR, AE_IFLNK; 0 for a hard link */
  const char *data;     /* a file's; a link's target */
} Entry;

static char *
make_archive (const char *dir, const char *name, gboolean zip, const Entry *entries)
{
  char *path = g_build_filename (dir, name, NULL);
  struct archive *a = archive_write_new ();

  if (zip)
    archive_write_set_format_zip (a);
  else
    archive_write_set_format_pax_restricted (a);
  g_assert_cmpint (archive_write_open_filename (a, path), ==, ARCHIVE_OK);
  for (const Entry *e = entries; e->path; e++)
    {
      struct archive_entry *entry = archive_entry_new ();

      archive_entry_set_pathname (entry, e->path);
      if (e->type == 0)
        {
          archive_entry_set_filetype (entry, AE_IFREG);
          archive_entry_set_hardlink (entry, e->data);
        }
      else
        archive_entry_set_filetype (entry, e->type);
      archive_entry_set_perm (entry, e->type == AE_IFDIR ? 0755 : 0644);
      if (e->type == AE_IFLNK)
        archive_entry_set_symlink (entry, e->data);
      if (e->type == AE_IFREG)
        archive_entry_set_size (entry, strlen (e->data));
      g_assert_cmpint (archive_write_header (a, entry), ==, ARCHIVE_OK);
      if (e->type == AE_IFREG)
        g_assert_cmpint (archive_write_data (a, e->data, strlen (e->data)), ==, (la_ssize_t) strlen (e->data));
      archive_entry_free (entry);
    }
  archive_write_close (a);
  archive_write_free (a);
  return path;
}

/* the names in `dir`, sorted, as one string */
static char *
listing (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  g_ptr_array_add (names, NULL);
  return g_strjoinv (" ", (char **) names->pdata);
}

typedef struct {
  char *base;           /* the test's own directory */
} Fixture;

static void
setup (Fixture *f, gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  f->base = g_dir_make_tmp ("test-download-XXXXXX", &error);
  g_assert_no_error (error);
}

static void
teardown (Fixture *f, gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (dictate_download_remove_tree (f->base, &error));
  g_assert_no_error (error);
  g_free (f->base);
}

/* `entries` unpacked into a new directory beside the archive: TRUE if it unpacked; either way nothing appears
 * outside that directory */
static gboolean
unpack (Fixture *f, gboolean zip, const Entry *entries, guint64 limit, GError **error)
{
  g_autofree char *archive = make_archive (f->base, zip ? "a.zip" : "a.tar", zip, entries);
  g_autofree char *into = g_build_filename (f->base, "into", NULL);
  g_autofree char *before = NULL, *after = NULL;
  gboolean ok;

  g_assert_cmpint (g_mkdir (into, 0700), ==, 0);
  before = listing (f->base);
  ok = dictate_download_unpack (archive, into, "top", limit, NULL, error);
  after = listing (f->base);
  g_assert_cmpstr (before, ==, after);
  return ok;
}

static void
test_good (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = {
    { "top/", AE_IFDIR }, { "top/am/", AE_IFDIR }, { "top/am/final.mdl", AE_IFREG, "model" },
    { "top/conf/model.conf", AE_IFREG, "--sample-frequency=16000" }, { NULL }
  };
  g_autoptr (GError) error = NULL;
  g_autofree char *file = g_build_filename (f->base, "into", "top", "conf", "model.conf", NULL);
  g_autofree char *contents = NULL;

  g_assert_true (unpack (f, GPOINTER_TO_INT (zip), entries, 1000, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (file, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "--sample-frequency=16000");
}

static void
expect_refused (Fixture *f, gboolean zip, const Entry *entries, guint64 limit)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (unpack (f, zip, entries, limit, &error));
  g_assert_nonnull (error);
  g_test_message ("refused: %s", error->message);
}

static void
test_parent (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top/", AE_IFDIR }, { "top/../../evil", AE_IFREG, "x" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_dot_dot (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "../evil", AE_IFREG, "x" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_absolute (Fixture *f, gconstpointer zip)
{
  g_autofree char *outside = g_build_filename (f->base, "evil", NULL);
  Entry entries[] = { { outside, AE_IFREG, "x" }, { NULL } };

  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
  g_assert_false (g_file_test (outside, G_FILE_TEST_EXISTS));
}

static void
test_outside_top (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top/a", AE_IFREG, "a" }, { "other/b", AE_IFREG, "b" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_symlink (Fixture *f, gconstpointer zip)
{
  /* a link out, then a file through it */
  static const Entry entries[] = {
    { "top/", AE_IFDIR }, { "top/out", AE_IFLNK, ".." }, { "top/out/evil", AE_IFREG, "x" }, { NULL }
  };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_hardlink (Fixture *f, gconstpointer data)
{
  static const Entry entries[] = { { "top/", AE_IFDIR }, { "top/passwd", 0, "/etc/passwd" }, { NULL } };
  expect_refused (f, FALSE, entries, 1000);
}

static void
test_backslash (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top\\..\\..\\evil", AE_IFREG, "x" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_twice (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top/a", AE_IFREG, "a" }, { "top/a", AE_IFREG, "b" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

static void
test_too_big (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top/a", AE_IFREG, "0123456789" }, { "top/b", AE_IFREG, "0123456789" },
                                   { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 15);
}

static void
test_top_is_a_file (Fixture *f, gconstpointer zip)
{
  static const Entry entries[] = { { "top", AE_IFREG, "x" }, { NULL } };
  expect_refused (f, GPOINTER_TO_INT (zip), entries, 1000);
}

/* a tree with a link out of it goes; what the link points at stays */
static void
test_remove_tree (Fixture *f, gconstpointer data)
{
  g_autofree char *keep = g_build_filename (f->base, "keep", NULL);
  g_autofree char *keep_file = g_build_filename (keep, "file", NULL);
  g_autofree char *tree = g_build_filename (f->base, "tree", NULL);
  g_autofree char *deep = g_build_filename (tree, "a", "b", NULL);
  g_autofree char *file = g_build_filename (deep, "file.part", NULL);
  g_autofree char *link = g_build_filename (tree, "a", "out", NULL);
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (keep, 0700), ==, 0);
  g_assert_true (g_file_set_contents (keep_file, "kept", -1, NULL));
  g_assert_cmpint (g_mkdir_with_parents (deep, 0700), ==, 0);
  g_assert_true (g_file_set_contents (file, "part", -1, NULL));
  g_assert_cmpint (symlink (keep, link), ==, 0);
  g_assert_true (dictate_download_remove_tree (tree, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (tree, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (keep_file, G_FILE_TEST_IS_REGULAR));
}

/* every pack: HTTPS, a place in models/ or voices/LANGUAGE (a voice's own directory in it, if it makes files), plain
 * names, sizes and sums, a file from elsewhere by its own HTTPS address, under its name */
static void
test_table (void)
{
  guint n;
  const DictatePack *packs = dictate_packs (&n);
  static const char *const languages[] = { "de", "en", "es", "fr", "it", "ru", NULL };

  for (guint i = 0; i < n; i++)
    {
      const DictatePack *pack = &packs[i];
      guint files = 0;

      g_assert_true (g_str_has_prefix (pack->source, "https://"));
      g_assert_null (strstr (pack->place, ".."));
      if (pack->kind == DICTATE_PACK_VOICE)
        {
          g_autofree char *place = g_strconcat ("voices/", pack->language, NULL);
          g_autofree char *own = g_strconcat (place, "/", NULL);

          if (pack->made)
            {
              /* what it makes goes with it: only from a directory of its own */
              g_assert_true (g_str_has_prefix (pack->place, own));
              g_assert_null (strchr (pack->place + strlen (own), '/'));
              g_assert_cmpuint (strlen (pack->made), >, 0);
            }
          else
            g_assert_cmpstr (pack->place, ==, place);
          /* what it replaces: plain files of a voice of its language, directly in that language's directory, none
           * of them one of its own */
          for (const char *const *old = pack->replaces; old && *old; old++)
            {
              g_assert_true (g_str_has_prefix (*old, own));
              g_assert_null (strchr (*old + strlen (own), '/'));
              g_assert_null (strstr (*old, ".."));
              g_assert_true (g_str_has_suffix (*old, ".onnx") || g_str_has_suffix (*old, ".onnx.json"));
              g_assert_false (pack->made && g_str_has_prefix (*old + strlen (own), pack->made));
              for (const DictateFile *f = pack->files; f->name; f++)
                {
                  g_autofree char *mine = g_strconcat (pack->place, "/", f->name, NULL);
                  g_assert_cmpstr (*old, !=, mine);
                }
            }
        }
      else
        {
          g_assert_null (pack->replaces);
          g_assert_true (g_str_has_prefix (pack->place, "models/"));
          g_assert_null (strchr (pack->place + strlen ("models/"), '/'));
          g_assert_nonnull (strstr (pack->place, pack->language));
          if (pack->archive_top)
            g_assert_true (g_str_has_suffix (pack->place, pack->archive_top));
        }
      for (const DictateFile *f = pack->files; f->name; f++, files++)
        {
          g_assert_null (strchr (f->name, '/'));
          g_assert_false (g_str_has_prefix (f->name, "."));
          g_assert_cmpuint (f->size, >, 0);
          g_assert_cmpuint (strlen (f->sha256), ==, 64);
          for (const char *c = f->sha256; *c; c++)
            g_assert_true (g_ascii_isxdigit (*c) && !g_ascii_isupper (*c));
          if (f->url)
            {
              g_autofree char *end = g_strconcat ("/", f->name, NULL);
              g_assert_true (g_str_has_prefix (f->url, "https://"));
              g_assert_true (g_str_has_suffix (f->url, end));
            }
          /* nothing it makes could be taken for one of its files */
          g_assert_false (pack->made && g_str_has_prefix (f->name, pack->made));
        }
      g_assert_cmpuint (files, >, 0);
      if (pack->archive_top)
        g_assert_cmpuint (files, ==, 1);
    }
  for (guint i = 0; languages[i]; i++)
    {
      g_assert_nonnull (dictate_pack_find (languages[i], DICTATE_PACK_MODEL));
      g_assert_nonnull (dictate_pack_find (languages[i], DICTATE_PACK_VOICE));
    }
}

static void
put_file (const char *dir, const char *name, gsize size)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *data = g_malloc0 (size);

  g_assert_true (g_file_set_contents (path, data, size, NULL));
}

/* a voice in a directory of its own (Russian's tiny-voca): its size and its removal take what it made there too, and
 * the voice it replaced (Piper's Denis, from before it), but leave another file in the language's directory */
static void
test_voice_dir (Fixture *f, gconstpointer data)
{
  const DictatePack *pack = dictate_pack_find ("ru", DICTATE_PACK_VOICE);
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
  g_autofree char *place = NULL, *language = NULL;
  g_autoptr (GError) error = NULL;
  guint64 size = 0;

  g_assert_nonnull (pack);
  g_assert_nonnull (pack->made);
  place = g_build_filename (root, pack->place, NULL);
  language = g_build_filename (root, "voices", "ru", NULL);
  g_assert_cmpint (g_mkdir_with_parents (place, 0700), ==, 0);
  for (const DictateFile *file = pack->files; file->name; file++, size += 3)
    put_file (place, file->name, 3);
  put_file (place, "stress-0123456789abcdef.sorted", 5);
  g_assert_nonnull (pack->replaces);
  put_file (language, "ru_RU-denis-medium.onnx", 7);
  put_file (language, "ru_RU-denis-medium.onnx.json", 2);
  put_file (language, "ru_RU-someone-else.onnx", 11);
  g_assert_cmpuint (dictate_download_downloaded_size ("ru"), ==, size + 5 + 7 + 2);

  g_assert_true (dictate_download_remove ("ru", &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (place, G_FILE_TEST_EXISTS));
  g_assert_cmpuint (dictate_download_downloaded_size ("ru"), ==, 0);
  {
    g_autofree char *denis = g_build_filename (language, "ru_RU-denis-medium.onnx", NULL);
    g_autofree char *config = g_strconcat (denis, ".json", NULL);
    g_autofree char *other = g_build_filename (language, "ru_RU-someone-else.onnx", NULL);
    g_assert_false (g_file_test (denis, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (config, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (other, G_FILE_TEST_IS_REGULAR));
  }
  g_assert_true (dictate_download_remove_tree (root, &error));
}

int
main (int argc, char **argv)
{
  g_autofree char *home = g_dir_make_tmp ("test-download-XXXXXX", NULL);

  /* the user's data directory, read once by GLib: a private one, before anything asks */
  g_assert_nonnull (home);
  g_setenv ("XDG_DATA_HOME", home, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/download/table", test_table);
  for (int zip = 0; zip < 2; zip++)
    {
      const char *kind = zip ? "zip" : "tar";
      struct { const char *name; void (*test) (Fixture *, gconstpointer); } tests[] = {
        { "good", test_good }, { "parent", test_parent }, { "dot-dot", test_dot_dot }, { "absolute", test_absolute },
        { "outside-top", test_outside_top }, { "symlink", test_symlink }, { "backslash", test_backslash },
        { "twice", test_twice }, { "too-big", test_too_big }, { "top-is-a-file", test_top_is_a_file },
      };

      for (guint i = 0; i < G_N_ELEMENTS (tests); i++)
        {
          g_autofree char *path = g_strdup_printf ("/download/unpack/%s/%s", kind, tests[i].name);
          g_test_add (path, Fixture, GINT_TO_POINTER (zip), setup, tests[i].test, teardown);
        }
    }
  g_test_add ("/download/unpack/tar/hardlink", Fixture, NULL, setup, test_hardlink, teardown);
  g_test_add ("/download/remove-tree", Fixture, NULL, setup, test_remove_tree, teardown);
  g_test_add ("/download/voice-dir", Fixture, NULL, setup, test_voice_dir, teardown);
  {
    int result = g_test_run ();
    g_rmdir (home);
    return result;
  }
}
