/* vibeos-unifiedpush: the registrations, and how far the stream has got, kept in
 * $XDG_STATE_HOME/vibeos-unifiedpush/state.json (the directory 0700, the file 0600: it holds every app's token and
 * topic). One JSON object:
 *
 *   "version"        1
 *   "server"         the push server the "active" flags below are about
 *   "since"          the id of the last message that has been dealt with, and all before it: after a restart the
 *                    stream starts from there
 *   "done"           ids of later messages already dealt with (the ones before them were still being delivered)
 *   "registrations"  [ { "token", "service", "topic", "description", "vapid", "created", "last-message",
 *                        "messages", "active", "activatable" } ]
 *
 * A file that cannot be read is moved aside (state.json.broken-TIME), and the distributor starts with nothing. */
#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "vup.h"

#define STATE_VERSION 1
#define SAVE_DELAY_MS 1000

static GPtrArray *registrations;        /* VupRegistration*, in the order they were made; owns them */
static GHashTable *by_token, *by_topic, *by_id;
static char *server;
static char *since;
static GStrv done;
static guint save_id;

static void
registration_free (gpointer data)
{
  VupRegistration *reg = data;

  g_free (reg->token);
  g_free (reg->service);
  g_free (reg->topic);
  g_free (reg->description);
  g_free (reg->vapid);
  g_free (reg->id);
  g_free (reg);
}

static char *
state_dir (void)
{
  return g_build_filename (g_get_user_state_dir (), "vibeos-unifiedpush", NULL);
}

static char *
state_file (void)
{
  g_autofree char *dir = state_dir ();

  return g_build_filename (dir, "state.json", NULL);
}

static void
index_registration (VupRegistration *reg)
{
  g_ptr_array_add (registrations, reg);
  g_hash_table_insert (by_token, reg->token, reg);
  g_hash_table_insert (by_topic, reg->topic, reg);
  g_hash_table_insert (by_id, reg->id, reg);
}

static gboolean
is_topic (const char *topic)
{
  if (!topic || strlen (topic) != VUP_TOPIC_LENGTH || !g_str_has_prefix (topic, "up"))
    return FALSE;
  for (const char *c = topic + 2; *c; c++)
    if (!g_ascii_isalnum (*c) && *c != '-' && *c != '_')
      return FALSE;
  return TRUE;
}

static const char *
member_string (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
    ? json_node_get_string (node) : NULL;
}

static void
load_registration (JsonArray *array, guint index, JsonNode *node, gpointer data)
{
  JsonObject *object = JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
  const char *token, *service, *topic, *description, *vapid;
  VupRegistration *reg;

  if (!object)
    return;
  token = member_string (object, "token");
  service = member_string (object, "service");
  topic = member_string (object, "topic");
  description = member_string (object, "description");
  vapid = member_string (object, "vapid");
  if (!token || !*token || !vup_utf8_within (token, VUP_TOKEN_MAX) || !service || !g_dbus_is_name (service)
      || g_dbus_is_unique_name (service) || !is_topic (topic) || g_hash_table_contains (by_token, token)
      || g_hash_table_contains (by_topic, topic) || (description && !vup_utf8_within (description, VUP_DESCRIPTION_MAX))
      || (vapid && !vup_vapid_is_valid (vapid)))
    {
      g_warning ("state: registration %u is not one, left out", index);
      return;
    }
  reg = g_new0 (VupRegistration, 1);
  reg->token = g_strdup (token);
  reg->service = g_strdup (service);
  reg->topic = g_strdup (topic);
  reg->description = g_strdup (description);
  reg->vapid = g_strdup (vapid);
  reg->id = vup_registration_id (token);
  reg->created = json_object_get_int_member_with_default (object, "created", 0);
  reg->last_message = json_object_get_int_member_with_default (object, "last-message", 0);
  reg->messages = json_object_get_int_member_with_default (object, "messages", 0);
  reg->active = json_object_get_boolean_member_with_default (object, "active", FALSE);
  reg->activatable = json_object_get_boolean_member_with_default (object, "activatable", FALSE);
  index_registration (reg);
}

static void
move_aside (const char *path, const char *why)
{
  g_autofree char *aside = g_strdup_printf ("%s.broken-%" G_GINT64_FORMAT, path, g_get_real_time () / G_USEC_PER_SEC);

  g_warning ("state: %s: %s; moved to %s, starting with no registrations", path, why, aside);
  if (g_rename (path, aside) < 0)
    g_warning ("state: could not move %s aside: %s", path, g_strerror (errno));
}

void
vup_store_load (void)
{
  g_autofree char *dir = state_dir (), *path = state_file (), *contents = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonObject *root;
  JsonNode *node;

  registrations = g_ptr_array_new_with_free_func (registration_free);
  by_token = g_hash_table_new (g_str_hash, g_str_equal);
  by_topic = g_hash_table_new (g_str_hash, g_str_equal);
  by_id = g_hash_table_new (g_str_hash, g_str_equal);

  if (g_mkdir_with_parents (dir, 0700) < 0 || g_chmod (dir, 0700) < 0)
    g_warning ("state: %s: %s", dir, g_strerror (errno));

  if (!g_file_get_contents (path, &contents, NULL, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("state: %s", error->message);
      return;
    }
  if (!json_parser_load_from_data (parser, contents, -1, &error))
    {
      move_aside (path, error->message);
      return;
    }
  node = json_parser_get_root (parser);
  if (!node || !JSON_NODE_HOLDS_OBJECT (node))
    {
      move_aside (path, "not a JSON object");
      return;
    }
  root = json_node_get_object (node);
  if (json_object_get_int_member_with_default (root, "version", 0) != STATE_VERSION)
    {
      move_aside (path, "another version");
      return;
    }
  server = g_strdup (member_string (root, "server"));
  since = g_strdup (member_string (root, "since"));
  if (json_object_has_member (root, "done") && JSON_NODE_HOLDS_ARRAY (json_object_get_member (root, "done")))
    {
      JsonArray *array = json_object_get_array_member (root, "done");
      g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();

      for (guint i = 0; i < json_array_get_length (array); i++)
        {
          JsonNode *id = json_array_get_element (array, i);
          if (JSON_NODE_HOLDS_VALUE (id) && json_node_get_value_type (id) == G_TYPE_STRING)
            g_strv_builder_add (builder, json_node_get_string (id));
        }
      done = g_strv_builder_end (builder);
    }
  if (json_object_has_member (root, "registrations")
      && JSON_NODE_HOLDS_ARRAY (json_object_get_member (root, "registrations")))
    json_array_foreach_element (json_object_get_array_member (root, "registrations"), load_registration, NULL);
  g_message ("%u registrations", registrations->len);
}

void
vup_store_save (void)
{
  g_autoptr(JsonBuilder) builder = json_builder_new ();
  g_autoptr(JsonGenerator) generator = json_generator_new ();
  g_autoptr(JsonNode) root = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = state_file (), *text = NULL;

  g_clear_handle_id (&save_id, g_source_remove);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, STATE_VERSION);
  if (server)
    {
      json_builder_set_member_name (builder, "server");
      json_builder_add_string_value (builder, server);
    }
  if (since)
    {
      json_builder_set_member_name (builder, "since");
      json_builder_add_string_value (builder, since);
    }
  json_builder_set_member_name (builder, "done");
  json_builder_begin_array (builder);
  for (guint i = 0; done && done[i]; i++)
    json_builder_add_string_value (builder, done[i]);
  json_builder_end_array (builder);
  json_builder_set_member_name (builder, "registrations");
  json_builder_begin_array (builder);
  for (guint i = 0; i < registrations->len; i++)
    {
      VupRegistration *reg = registrations->pdata[i];

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "token");
      json_builder_add_string_value (builder, reg->token);
      json_builder_set_member_name (builder, "service");
      json_builder_add_string_value (builder, reg->service);
      json_builder_set_member_name (builder, "topic");
      json_builder_add_string_value (builder, reg->topic);
      if (reg->description)
        {
          json_builder_set_member_name (builder, "description");
          json_builder_add_string_value (builder, reg->description);
        }
      if (reg->vapid)
        {
          json_builder_set_member_name (builder, "vapid");
          json_builder_add_string_value (builder, reg->vapid);
        }
      json_builder_set_member_name (builder, "created");
      json_builder_add_int_value (builder, reg->created);
      json_builder_set_member_name (builder, "last-message");
      json_builder_add_int_value (builder, reg->last_message);
      json_builder_set_member_name (builder, "messages");
      json_builder_add_int_value (builder, (gint64) reg->messages);
      json_builder_set_member_name (builder, "active");
      json_builder_add_boolean_value (builder, reg->active);
      json_builder_set_member_name (builder, "activatable");
      json_builder_add_boolean_value (builder, reg->activatable);
      json_builder_end_object (builder);
    }
  json_builder_end_array (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  text = json_generator_to_data (generator, NULL);
  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_ONLY_EXISTING,
                                 0600, &error))
    g_warning ("state: %s", error->message);
}

static gboolean
save_now (gpointer data)
{
  save_id = 0;
  vup_store_save ();
  return G_SOURCE_REMOVE;
}

/* for what a crash may lose (the stream's position: a message then comes again, with the same id) */
void
vup_store_save_soon (void)
{
  if (!save_id)
    save_id = g_timeout_add (SAVE_DELAY_MS, save_now, NULL);
}

void
vup_store_flush (void)
{
  if (save_id)
    vup_store_save ();
}

GList *
vup_store_list (void)
{
  GList *list = NULL;

  for (guint i = registrations->len; i > 0; i--)
    list = g_list_prepend (list, registrations->pdata[i - 1]);
  return list;
}

VupRegistration *
vup_store_by_token (const char *token)
{
  return token ? g_hash_table_lookup (by_token, token) : NULL;
}

VupRegistration *
vup_store_by_topic (const char *topic)
{
  return topic ? g_hash_table_lookup (by_topic, topic) : NULL;
}

VupRegistration *
vup_store_by_id (const char *id)
{
  return id ? g_hash_table_lookup (by_id, id) : NULL;
}

guint
vup_store_count (const char *service)
{
  guint count = 0;

  if (!service)
    return registrations->len;
  for (guint i = 0; i < registrations->len; i++)
    if (g_str_equal (((VupRegistration *) registrations->pdata[i])->service, service))
      count++;
  return count;
}

VupRegistration *
vup_store_add (const char *token, const char *service, const char *description, const char *vapid)
{
  VupRegistration *reg = g_new0 (VupRegistration, 1);

  reg->token = g_strdup (token);
  reg->service = g_strdup (service);
  do
    {
      g_free (reg->topic);
      reg->topic = vup_new_topic ();
    }
  while (g_hash_table_contains (by_topic, reg->topic));
  reg->description = g_strdup (description);
  reg->vapid = g_strdup (vapid);
  reg->id = vup_registration_id (token);
  reg->created = g_get_real_time () / G_USEC_PER_SEC;
  index_registration (reg);
  vup_store_save ();
  return reg;
}

void
vup_store_remove (VupRegistration *reg)
{
  g_hash_table_remove (by_token, reg->token);
  g_hash_table_remove (by_topic, reg->topic);
  g_hash_table_remove (by_id, reg->id);
  g_ptr_array_remove (registrations, reg);
  vup_store_save ();
}

char *
vup_store_endpoint (VupRegistration *reg)
{
  return g_strdup_printf ("%s/%s?up=1", server, reg->topic);
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

/* every registration's topic, sorted (the same set gives the same text), comma separated; NULL for none */
char *
vup_store_topics (void)
{
  g_autoptr(GPtrArray) topics = g_ptr_array_new ();

  if (registrations->len == 0)
    return NULL;
  for (guint i = 0; i < registrations->len; i++)
    g_ptr_array_add (topics, ((VupRegistration *) registrations->pdata[i])->topic);
  g_ptr_array_sort (topics, compare_strings);
  g_ptr_array_add (topics, NULL);
  return g_strjoinv (",", (char **) topics->pdata);
}

const char *
vup_store_get_server (void)
{
  return server;
}

/* a new push server: no topic is known there yet, and the old server's message ids mean nothing on it. TRUE when
 * it is a new one */
gboolean
vup_store_set_server (const char *new_server)
{
  if (g_strcmp0 (server, new_server) == 0)
    return FALSE;
  g_free (server);
  server = g_strdup (new_server);
  for (guint i = 0; i < registrations->len; i++)
    ((VupRegistration *) registrations->pdata[i])->active = FALSE;
  g_clear_pointer (&since, g_free);
  g_clear_pointer (&done, g_strfreev);
  vup_store_save ();
  return TRUE;
}

const char *
vup_store_get_since (void)
{
  return since;
}

void
vup_store_set_since (const char *id)
{
  if (g_strcmp0 (since, id) == 0)
    return;
  g_free (since);
  since = g_strdup (id);
  vup_store_save_soon ();
}

GStrv
vup_store_get_done (void)
{
  return done;
}

void
vup_store_set_done (GPtrArray *ids)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) next = NULL;

  for (guint i = 0; ids && i < ids->len; i++)
    g_strv_builder_add (builder, ids->pdata[i]);
  next = g_strv_builder_end (builder);
  if (done ? g_strv_equal ((const char *const *) done, (const char *const *) next) : !next[0])
    return;
  g_strfreev (done);
  done = g_steal_pointer (&next);
  vup_store_save_soon ();
}
