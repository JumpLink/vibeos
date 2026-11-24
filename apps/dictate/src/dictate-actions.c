/* dictate-actions: see dictate-actions.h. The file is read whole and appended to under one lock; past LOG_MAX lines
 * it is written again with the newest LOG_KEEP. */
#include "dictate-actions.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_KEEP 200
#define LOG_MAX  300

static GMutex lock;

static char *
path_of (const char *name)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-dictate", name, NULL);
}

/* the lines of the log, oldest first (empty when there is none) */
static GStrv
read_lines (const char *path)
{
  g_autofree char *contents = NULL;
  GPtrArray *lines = g_ptr_array_new ();
  g_auto (GStrv) all = NULL;

  if (g_file_get_contents (path, &contents, NULL, NULL))
    {
      all = g_strsplit (contents, "\n", -1);
      for (guint i = 0; all[i]; i++)
        if (*all[i])
          g_ptr_array_add (lines, g_strdup (all[i]));
    }
  g_ptr_array_add (lines, NULL);
  return (GStrv) g_ptr_array_free (lines, FALSE);
}

static JsonObject *
parse (const char *line)
{
  g_autoptr (JsonNode) node = json_from_string (line, NULL);

  return node && JSON_NODE_HOLDS_OBJECT (node) ? json_object_ref (json_node_get_object (node)) : NULL;
}

/* `text` written whole as the file, 0600, by a rename: a reader never sees half of it */
static gboolean
write_file (const char *path, const char *text)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  if (g_mkdir_with_parents (dir, 0700) != 0)
    return FALSE;
  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    {
      g_warning ("actions: %s", error->message);
      return FALSE;
    }
  return TRUE;
}

static gboolean
append (const char *path, const char *line)
{
  g_autofree char *dir = g_path_get_dirname (path);
  int fd;
  gsize len = strlen (line);
  gboolean ok;

  if (g_mkdir_with_parents (dir, 0700) != 0)
    return FALSE;
  fd = g_open (path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      g_warning ("actions: %s", g_strerror (errno));
      return FALSE;
    }
  ok = write (fd, line, len) == (gssize) len && write (fd, "\n", 1) == 1;
  close (fd);
  return ok;
}

gint64
dictate_actions_record (const char *tool, DictateTier tier, const char *outcome, const char *const *sent,
                        JsonNode *undo, gint64 undid)
{
  g_autofree char *path = path_of ("agent-log.jsonl");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (JsonNode) owned_undo = undo;
  g_autofree char *line = NULL;
  g_auto (GStrv) lines = NULL;
  gint64 id = 1;
  guint n;

  g_mutex_lock (&lock);
  lines = read_lines (path);
  n = g_strv_length (lines);
  if (n)
    {
      g_autoptr (JsonObject) last = parse (lines[n - 1]);
      if (last)
        id = json_object_get_int_member_with_default (last, "id", 0) + 1;
    }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_int_value (b, id);
  json_builder_set_member_name (b, "t");
  json_builder_add_int_value (b, g_get_real_time () / G_USEC_PER_SEC);
  json_builder_set_member_name (b, "tool");
  json_builder_add_string_value (b, tool);
  json_builder_set_member_name (b, "tier");
  json_builder_add_int_value (b, tier);
  json_builder_set_member_name (b, "outcome");
  json_builder_add_string_value (b, outcome);
  json_builder_set_member_name (b, "sent");
  json_builder_begin_array (b);
  for (guint i = 0; sent && sent[i]; i++)
    json_builder_add_string_value (b, sent[i]);
  json_builder_end_array (b);
  if (owned_undo)
    {
      json_builder_set_member_name (b, "undo");
      json_builder_add_value (b, g_steal_pointer (&owned_undo));
    }
  if (undid > 0)
    {
      json_builder_set_member_name (b, "undid");
      json_builder_add_int_value (b, undid);
    }
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  line = json_to_string (root, FALSE);
  if (n + 1 > LOG_MAX)
    {
      /* the newest LOG_KEEP - 1, and this one */
      GString *text = g_string_new (NULL);
      for (guint i = n - (LOG_KEEP - 1); i < n; i++)
        g_string_append_printf (text, "%s\n", lines[i]);
      g_string_append_printf (text, "%s\n", line);
      if (!write_file (path, text->str))
        id = 0;
      g_string_free (text, TRUE);
    }
  else if (!append (path, line))
    id = 0;
  g_mutex_unlock (&lock);
  return id;
}

GPtrArray *
dictate_actions_recent (guint max)
{
  g_autofree char *path = path_of ("agent-log.jsonl");
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);
  g_auto (GStrv) lines = NULL;
  guint n;

  g_mutex_lock (&lock);
  lines = read_lines (path);
  g_mutex_unlock (&lock);
  n = g_strv_length (lines);
  for (guint i = n; i > 0 && entries->len < max; i--)
    {
      JsonObject *o = parse (lines[i - 1]);
      if (o)
        g_ptr_array_add (entries, o);
    }
  return entries;
}

gboolean
dictate_actions_undone (GPtrArray *entries, gint64 id)
{
  for (guint i = 0; i < entries->len; i++)
    if (json_object_get_int_member_with_default (entries->pdata[i], "undid", 0) == id)
      return TRUE;
  return FALSE;
}

gint64
dictate_actions_missed_since (void)
{
  g_autofree char *path = path_of ("agent-missed-since");
  g_autofree char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return 0;
  return g_ascii_strtoll (text, NULL, 10);
}

void
dictate_actions_set_missed_since (gint64 when)
{
  g_autofree char *path = path_of ("agent-missed-since");
  g_autofree char *text = g_strdup_printf ("%" G_GINT64_FORMAT "\n", when);

  write_file (path, text);
}

void
dictate_actions_forget (void)
{
  g_autofree char *log = path_of ("agent-log.jsonl");
  g_autofree char *since = path_of ("agent-missed-since");

  g_mutex_lock (&lock);
  g_unlink (log);
  g_unlink (since);
  g_mutex_unlock (&lock);
}
