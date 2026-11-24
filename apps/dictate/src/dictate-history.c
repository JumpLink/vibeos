/* dictate-history: see dictate-history.h. */
#include "dictate-history.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

static char *
history_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "conversation.json", NULL);
}

void
dictate_history_entry_free (DictateHistoryEntry *entry)
{
  if (!entry)
    return;
  g_free (entry->said);
  g_free (entry->reply);
  g_free (entry->note);
  g_free (entry);
}

static char *
member (JsonObject *o, const char *name)
{
  const char *value = json_object_get_string_member_with_default (o, name, NULL);

  return value && *value ? g_strdup (value) : NULL;
}

GPtrArray *
dictate_history_load (void)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_history_entry_free);
  g_autofree char *path = history_path ();
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  JsonNode *root;
  JsonArray *list;

  if (!json_parser_load_from_file (parser, path, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("history: %s", error->message);
      return entries;
    }
  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY (root))
    return entries;
  list = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (list); i++)
    {
      JsonObject *o = json_array_get_object_element (list, i);
      DictateHistoryEntry *entry;

      if (!o)
        continue;
      entry = g_new0 (DictateHistoryEntry, 1);
      entry->said = member (o, "said");
      entry->reply = member (o, "reply");
      entry->note = member (o, "note");
      entry->refused = json_object_get_boolean_member_with_default (o, "refused", FALSE);
      g_ptr_array_add (entries, entry);
    }
  return entries;
}

void
dictate_history_save (GPtrArray *entries)
{
  g_autofree char *path = history_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;
  guint from = entries->len > DICTATE_HISTORY_KEEP ? entries->len - DICTATE_HISTORY_KEEP : 0;

  json_builder_begin_array (b);
  for (guint i = from; i < entries->len; i++)
    {
      DictateHistoryEntry *entry = entries->pdata[i];

      json_builder_begin_object (b);
      json_builder_set_member_name (b, "said");
      json_builder_add_string_value (b, entry->said ? entry->said : "");
      json_builder_set_member_name (b, "reply");
      json_builder_add_string_value (b, entry->reply ? entry->reply : "");
      if (entry->note)
        {
          json_builder_set_member_name (b, "note");
          json_builder_add_string_value (b, entry->note);
        }
      if (entry->refused)
        {
          json_builder_set_member_name (b, "refused");
          json_builder_add_boolean_value (b, TRUE);
        }
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  root = json_builder_get_root (b);
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);
  if (g_mkdir_with_parents (dir, 0700) != 0 ||
      !g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("history: %s", error ? error->message : g_strerror (errno));
}

void
dictate_history_clear (void)
{
  g_autofree char *path = history_path ();

  if (g_unlink (path) != 0 && errno != ENOENT)
    g_warning ("history: %s: %s", path, g_strerror (errno));
}
