/* Bookmarks, history and the last-visited list, kept across runs. */
#include "fm-state.h"

#include <glib/gstdio.h>

static void
test_state (void)
{
  g_autofree char *dir = g_dir_make_tmp ("fm-state-XXXXXX", NULL);
  g_autofree char *config = g_build_filename (dir, "config", NULL);
  g_autofree char *stated = g_build_filename (dir, "state", NULL);
  FmState *state = fm_state_new (config, stated);
  GPtrArray *visited;

  g_assert_true (fm_state_add_bookmark (state, "file:///home/user/Music", "Music"));
  g_assert_false (fm_state_add_bookmark (state, "file:///home/user/Music", "Again"));
  fm_state_add_bookmark (state, "smb://nas/VIDEO/", "Films");
  fm_state_rename_bookmark (state, 1, "Movies");
  fm_state_add_history (state, "file:///a", "Main storage");
  fm_state_add_history (state, "file:///b", "Main storage");
  fm_state_add_history (state, "file:///a", "Main storage");
  for (int i = 0; i < 12; i++)
    {
      g_autofree char *root = g_strdup_printf ("smb://host%d/", i);

      fm_state_visit (state, root, "host", root, "/");
    }
  fm_state_pin_visited (state, "smb://host0/", TRUE);
  fm_state_free (state);

  state = fm_state_new (config, stated);
  g_assert_cmpuint (fm_state_bookmarks (state)->len, ==, 2);
  g_assert_cmpstr (((FmBookmark *) fm_state_bookmarks (state)->pdata[1])->name, ==, "Movies");
  g_assert_cmpuint (fm_state_history (state)->len, ==, 2);
  g_assert_cmpstr (((FmHistoryEntry *) fm_state_history (state)->pdata[0])->uri, ==, "file:///a");
  visited = fm_state_visited (state);
  /* host0 was dropped as the oldest of 12 unpinned, so pinning it did nothing; 10 remain */
  g_assert_cmpuint (visited->len, ==, FM_VISITED_MAX);
  fm_state_pin_visited (state, "smb://host5/", TRUE);
  g_assert_cmpstr (((FmVisited *) fm_state_visited (state)->pdata[0])->root_uri, ==, "smb://host5/");
  fm_state_remove_visited (state, "smb://host5/");
  g_assert_cmpuint (fm_state_visited (state)->len, ==, FM_VISITED_MAX - 1);
  fm_state_remove_bookmark (state, 0);
  fm_state_clear_history (state);
  fm_state_free (state);

  state = fm_state_new (config, stated);
  g_assert_cmpuint (fm_state_bookmarks (state)->len, ==, 1);
  g_assert_cmpuint (fm_state_history (state)->len, ==, 0);
  fm_state_free (state);

  g_autofree char *b = g_build_filename (config, "bookmarks.ini", NULL);
  g_autofree char *s = g_build_filename (stated, "state.ini", NULL);
  g_remove (b);
  g_remove (s);
  g_rmdir (config);
  g_rmdir (stated);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/state/round-trip", test_state);
  return g_test_run ();
}
