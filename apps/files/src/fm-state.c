/* What the app remembers (fm-state.h). */
#include "fm-state.h"

#include <errno.h>

struct _FmState {
  char *bookmarks_path, *state_path;
  GPtrArray *bookmarks, *history, *visited;
};

static void
bookmark_free (FmBookmark *bookmark)
{
  g_free (bookmark->uri);
  g_free (bookmark->name);
  g_free (bookmark);
}

static void
history_free (FmHistoryEntry *entry)
{
  g_free (entry->uri);
  g_free (entry->root_name);
  g_free (entry);
}

static void
visited_free (FmVisited *visited)
{
  g_free (visited->root_uri);
  g_free (visited->name);
  g_free (visited->uri);
  g_free (visited->path);
  g_free (visited);
}

static void
save_file (GKeyFile *file, const char *path)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr(GError) error = NULL;

  if (g_mkdir_with_parents (dir, 0700) != 0)
    g_warning ("%s: %s", dir, g_strerror (errno));
  else if (!g_key_file_save_to_file (file, path, &error))
    g_warning ("%s", error->message);
}

static void
save_bookmarks (FmState *state)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();

  for (guint i = 0; i < state->bookmarks->len; i++)
    {
      FmBookmark *bookmark = state->bookmarks->pdata[i];
      g_autofree char *group = g_strdup_printf ("bookmark-%u", i + 1);

      g_key_file_set_string (file, group, "uri", bookmark->uri);
      g_key_file_set_string (file, group, "name", bookmark->name);
    }
  save_file (file, state->bookmarks_path);
}

static void
save_state (FmState *state)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();

  for (guint i = 0; i < state->history->len; i++)
    {
      FmHistoryEntry *entry = state->history->pdata[i];
      g_autofree char *group = g_strdup_printf ("history-%u", i + 1);

      g_key_file_set_string (file, group, "uri", entry->uri);
      g_key_file_set_string (file, group, "root-name", entry->root_name);
      g_key_file_set_int64 (file, group, "time", entry->time);
    }
  for (guint i = 0; i < state->visited->len; i++)
    {
      FmVisited *visited = state->visited->pdata[i];
      g_autofree char *group = g_strdup_printf ("visited-%u", i + 1);

      g_key_file_set_string (file, group, "root-uri", visited->root_uri);
      g_key_file_set_string (file, group, "name", visited->name);
      g_key_file_set_string (file, group, "uri", visited->uri);
      g_key_file_set_string (file, group, "path", visited->path);
      g_key_file_set_boolean (file, group, "pinned", visited->pinned);
      g_key_file_set_int64 (file, group, "time", visited->time);
    }
  save_file (file, state->state_path);
}

static char *
get (GKeyFile *file, const char *group, const char *key)
{
  char *value = g_key_file_get_string (file, group, key, NULL);

  return value ? value : g_strdup ("");
}

FmState *
fm_state_new (const char *config_dir, const char *state_dir)
{
  FmState *state = g_new0 (FmState, 1);
  g_autoptr(GKeyFile) bookmarks = g_key_file_new ();
  g_autoptr(GKeyFile) saved = g_key_file_new ();

  state->bookmarks_path = g_build_filename (config_dir, "bookmarks.ini", NULL);
  state->state_path = g_build_filename (state_dir, "state.ini", NULL);
  state->bookmarks = g_ptr_array_new_with_free_func ((GDestroyNotify) bookmark_free);
  state->history = g_ptr_array_new_with_free_func ((GDestroyNotify) history_free);
  state->visited = g_ptr_array_new_with_free_func ((GDestroyNotify) visited_free);
  if (g_key_file_load_from_file (bookmarks, state->bookmarks_path, G_KEY_FILE_NONE, NULL))
    {
      g_auto(GStrv) groups = g_key_file_get_groups (bookmarks, NULL);

      for (int i = 0; groups[i]; i++)
        {
          FmBookmark *bookmark = g_new0 (FmBookmark, 1);

          bookmark->uri = get (bookmarks, groups[i], "uri");
          bookmark->name = get (bookmarks, groups[i], "name");
          if (*bookmark->uri)
            g_ptr_array_add (state->bookmarks, bookmark);
          else
            bookmark_free (bookmark);
        }
    }
  if (g_key_file_load_from_file (saved, state->state_path, G_KEY_FILE_NONE, NULL))
    {
      g_auto(GStrv) groups = g_key_file_get_groups (saved, NULL);

      for (int i = 0; groups[i]; i++)
        if (g_str_has_prefix (groups[i], "history-"))
          {
            FmHistoryEntry *entry = g_new0 (FmHistoryEntry, 1);

            entry->uri = get (saved, groups[i], "uri");
            entry->root_name = get (saved, groups[i], "root-name");
            entry->time = g_key_file_get_int64 (saved, groups[i], "time", NULL);
            g_ptr_array_add (state->history, entry);
          }
        else if (g_str_has_prefix (groups[i], "visited-"))
          {
            FmVisited *visited = g_new0 (FmVisited, 1);

            visited->root_uri = get (saved, groups[i], "root-uri");
            visited->name = get (saved, groups[i], "name");
            visited->uri = get (saved, groups[i], "uri");
            visited->path = get (saved, groups[i], "path");
            visited->pinned = g_key_file_get_boolean (saved, groups[i], "pinned", NULL);
            visited->time = g_key_file_get_int64 (saved, groups[i], "time", NULL);
            g_ptr_array_add (state->visited, visited);
          }
    }
  return state;
}

void
fm_state_free (FmState *state)
{
  if (!state)
    return;
  g_ptr_array_unref (state->bookmarks);
  g_ptr_array_unref (state->history);
  g_ptr_array_unref (state->visited);
  g_free (state->bookmarks_path);
  g_free (state->state_path);
  g_free (state);
}

/* ---- bookmarks */

GPtrArray *
fm_state_bookmarks (FmState *state)
{
  return state->bookmarks;
}

gboolean
fm_state_is_bookmarked (FmState *state, const char *uri)
{
  for (guint i = 0; i < state->bookmarks->len; i++)
    if (!g_strcmp0 (((FmBookmark *) state->bookmarks->pdata[i])->uri, uri))
      return TRUE;
  return FALSE;
}

gboolean
fm_state_add_bookmark (FmState *state, const char *uri, const char *name)
{
  FmBookmark *bookmark;

  if (fm_state_is_bookmarked (state, uri))
    return FALSE;
  bookmark = g_new0 (FmBookmark, 1);
  bookmark->uri = g_strdup (uri);
  bookmark->name = g_strdup (name);
  g_ptr_array_add (state->bookmarks, bookmark);
  save_bookmarks (state);
  return TRUE;
}

void
fm_state_rename_bookmark (FmState *state, guint index, const char *name)
{
  FmBookmark *bookmark;

  if (index >= state->bookmarks->len)
    return;
  bookmark = state->bookmarks->pdata[index];
  g_free (bookmark->name);
  bookmark->name = g_strdup (name);
  save_bookmarks (state);
}

void
fm_state_remove_bookmark (FmState *state, guint index)
{
  if (index >= state->bookmarks->len)
    return;
  g_ptr_array_remove_index (state->bookmarks, index);
  save_bookmarks (state);
}

/* ---- history: one entry per folder, the newest first */

GPtrArray *
fm_state_history (FmState *state)
{
  return state->history;
}

void
fm_state_add_history (FmState *state, const char *uri, const char *root_name)
{
  FmHistoryEntry *entry;

  for (guint i = 0; i < state->history->len; i++)
    if (!g_strcmp0 (((FmHistoryEntry *) state->history->pdata[i])->uri, uri))
      {
        g_ptr_array_remove_index (state->history, i);
        break;
      }
  entry = g_new0 (FmHistoryEntry, 1);
  entry->uri = g_strdup (uri);
  entry->root_name = g_strdup (root_name);
  entry->time = g_get_real_time ();
  g_ptr_array_insert (state->history, 0, entry);
  if (state->history->len > FM_HISTORY_MAX)
    g_ptr_array_set_size (state->history, FM_HISTORY_MAX);
  save_state (state);
}

void
fm_state_clear_history (FmState *state)
{
  g_ptr_array_set_size (state->history, 0);
  save_state (state);
}

/* ---- last-visited locations */

static int
compare_visited (gconstpointer pa, gconstpointer pb)
{
  const FmVisited *a = *(FmVisited **) pa, *b = *(FmVisited **) pb;

  if (a->pinned != b->pinned)
    return a->pinned ? -1 : 1;
  return a->time > b->time ? -1 : a->time < b->time ? 1 : 0;
}

static void
order_visited (FmState *state)
{
  guint unpinned = 0;

  g_ptr_array_sort (state->visited, compare_visited);
  for (guint i = 0; i < state->visited->len; i++)
    if (!((FmVisited *) state->visited->pdata[i])->pinned && ++unpinned > FM_VISITED_MAX)
      {
        g_ptr_array_remove_index (state->visited, i);
        i--;
      }
}

GPtrArray *
fm_state_visited (FmState *state)
{
  return state->visited;
}

static FmVisited *
find_visited (FmState *state, const char *root_uri)
{
  for (guint i = 0; i < state->visited->len; i++)
    if (!g_strcmp0 (((FmVisited *) state->visited->pdata[i])->root_uri, root_uri))
      return state->visited->pdata[i];
  return NULL;
}

void
fm_state_visit (FmState *state, const char *root_uri, const char *name, const char *uri, const char *path)
{
  FmVisited *visited = find_visited (state, root_uri);

  if (!visited)
    {
      visited = g_new0 (FmVisited, 1);
      visited->root_uri = g_strdup (root_uri);
      g_ptr_array_add (state->visited, visited);
    }
  g_free (visited->name);
  visited->name = g_strdup (name);
  g_free (visited->uri);
  visited->uri = g_strdup (uri);
  g_free (visited->path);
  visited->path = g_strdup (path);
  visited->time = g_get_real_time ();
  order_visited (state);
  save_state (state);
}

void
fm_state_pin_visited (FmState *state, const char *root_uri, gboolean pinned)
{
  FmVisited *visited = find_visited (state, root_uri);

  if (!visited)
    return;
  visited->pinned = pinned;
  order_visited (state);
  save_state (state);
}

void
fm_state_remove_visited (FmState *state, const char *root_uri)
{
  FmVisited *visited = find_visited (state, root_uri);

  if (!visited)
    return;
  g_ptr_array_remove (state->visited, visited);
  save_state (state);
}
