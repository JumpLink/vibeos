/* dictate-debuglog: see dictate-debuglog.h. A turn is built as one JSON object in memory, its steps an array, and
 * written whole at its end, by a rename (a reader never sees half of it); then the oldest turns go until the rest
 * fit DEBUGLOG_KEEP and DEBUGLOG_BYTES. */
#include "dictate-debuglog.h"
#include "dictate-brain.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

struct _DictateDebugTurn {
  JsonObject *root;
  JsonArray *steps;
  gint64 t0;                    /* monotonic, µs */
  gint64 started;               /* real, µs */
  guint turn;
  char *tools_sent;             /* the tools of the turn's first request, to say "the same" after it */
};

static GPrivate current;        /* DictateDebugTurn *, the thread's */
static GMutex files;            /* the directory: a turn written and the old ones removed, or all of them cleared */

char *
dictate_debuglog_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "debug-log", NULL);
}

/* --- what is never written whole */

/* a file's first line, if the file is there: the gateway's key and the phone's id, read, never made, here */
static char *
kept_value (const char *sub, const char *name)
{
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), sub, name, NULL);
  g_autofree char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;
  g_strstrip (contents);
  return strlen (contents) >= 8 ? g_steal_pointer (&contents) : NULL;
}

static void
cut (GString *s, const char *secret)
{
  g_autofree char *shown = NULL;

  if (!secret || strlen (secret) < 8)
    return;
  shown = g_strdup_printf ("%.4s…", secret);
  g_string_replace (s, secret, shown, 0);
}

char *
dictate_debuglog_redact (const char *text)
{
  static GRegex *bearer;
  GString *s = g_string_new (text ? text : "");
  g_autofree char *secret = kept_value ("vibeos-dictate", "agent-secret");
  g_autofree char *id = kept_value ("vibeos-software", "device-id");
  char *out;

  cut (s, secret);
  cut (s, id);
  if (g_once_init_enter_pointer (&bearer))
    g_once_init_leave_pointer (&bearer, g_regex_new ("(Bearer\\s+)([^\\s\"\\\\]{4})[^\\s\"\\\\]*",
                                                     G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL));
  out = g_regex_replace (bearer, s->str, -1, 0, "\\1\\2…", 0, NULL);
  g_string_free (s, TRUE);
  return out ? out : g_strdup ("");
}

/* --- a turn */

static DictateDebugTurn *
turn_now (void)
{
  return g_private_get (&current);
}

static double
since (DictateDebugTurn *t)
{
  return (g_get_monotonic_time () - t->t0) / 1000.0;
}

/* JSON text as a node (pretty in the viewer), or as a string when it is not JSON */
static JsonNode *
as_json (const char *text, gsize length)
{
  g_autofree char *copy = g_strndup (text ? text : "", text ? length : 0);
  JsonNode *node = *copy ? json_from_string (copy, NULL) : NULL;

  if (!node)
    {
      node = json_node_new (JSON_NODE_VALUE);
      json_node_set_string (node, copy);
    }
  return node;
}

static JsonObject *
step (DictateDebugTurn *t, const char *kind)
{
  JsonObject *o = json_object_new ();

  json_object_set_string_member (o, "kind", kind);
  json_object_set_int_member (o, "at_ms", (gint64) since (t));
  json_array_add_object_element (t->steps, o);
  return o;
}

DictateDebugTurn *
dictate_debuglog_begin (guint turn, const char *said, const char *language, const char *brain, gboolean locked,
                        gboolean private_mode)
{
  DictateDebugTurn *t = g_new0 (DictateDebugTurn, 1);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *started = g_date_time_format_iso8601 (now);

  t->t0 = g_get_monotonic_time ();
  t->started = g_date_time_to_unix_usec (now);
  t->turn = turn;
  t->root = json_object_new ();
  t->steps = json_array_new ();
  json_object_set_int_member (t->root, "version", 1);
  json_object_set_string_member (t->root, "started", started);
  json_object_set_int_member (t->root, "started_us", t->started);
  json_object_set_int_member (t->root, "turn", turn);
  json_object_set_string_member (t->root, "said", said ? said : "");
  json_object_set_string_member (t->root, "language", language ? language : "");
  json_object_set_string_member (t->root, "brain", brain ? brain : "");
  json_object_set_boolean_member (t->root, "locked", locked);
  json_object_set_boolean_member (t->root, "private_mode", private_mode);
  g_private_set (&current, t);
  return t;
}

gboolean
dictate_debuglog_active (void)
{
  return turn_now () != NULL;
}

void
dictate_debuglog_request (const char *method, const char *url, const char *body, gsize length)
{
  DictateDebugTurn *t = turn_now ();
  JsonObject *o;
  JsonNode *node;

  if (!t)
    return;
  o = step (t, "request");
  json_object_set_string_member (o, "method", method);
  json_object_set_string_member (o, "url", url);
  node = as_json (body, length);
  /* the tools are the same in every step of a turn: written whole once, then said to be the same */
  if (JSON_NODE_HOLDS_OBJECT (node) && json_object_has_member (json_node_get_object (node), "tools"))
    {
      JsonObject *b = json_node_get_object (node);
      g_autofree char *tools = json_to_string (json_object_get_member (b, "tools"), FALSE);

      if (!t->tools_sent)
        t->tools_sent = g_steal_pointer (&tools);
      else if (!strcmp (t->tools_sent, tools))
        json_object_set_string_member (b, "tools", "the same as in this turn's first request");
    }
  json_object_set_member (o, "body", node);
}

void
dictate_debuglog_http (const char *method, const char *url, guint status, double ms)
{
  DictateDebugTurn *t = turn_now ();
  JsonObject *o;

  if (!t)
    return;
  o = step (t, "http");
  json_object_set_string_member (o, "method", method);
  json_object_set_string_member (o, "url", url);
  json_object_set_int_member (o, "status", status);
  json_object_set_int_member (o, "ms", (gint64) ms);
}

void
dictate_debuglog_response (const char *content, GPtrArray *calls, int prompt_tokens, int completion_tokens,
                           double first_ms, double total_ms)
{
  DictateDebugTurn *t = turn_now ();
  JsonObject *o;
  JsonArray *list;

  if (!t)
    return;
  o = step (t, "response");
  json_object_set_string_member (o, "content", content ? content : "");
  list = json_array_new ();
  for (guint i = 0; calls && i < calls->len; i++)
    {
      DictateBrainCall *c = calls->pdata[i];
      JsonObject *call = json_object_new ();
      const char *args = c->arguments ? c->arguments : "";

      json_object_set_string_member (call, "id", c->id ? c->id : "");
      json_object_set_string_member (call, "name", c->name ? c->name : "");
      json_object_set_member (call, "arguments", as_json (args, strlen (args)));
      json_array_add_object_element (list, call);
    }
  json_object_set_array_member (o, "tool_calls", list);
  if (prompt_tokens >= 0)
    json_object_set_int_member (o, "prompt_tokens", prompt_tokens);
  if (completion_tokens >= 0)
    json_object_set_int_member (o, "completion_tokens", completion_tokens);
  json_object_set_int_member (o, "first_ms", (gint64) first_ms);
  json_object_set_int_member (o, "total_ms", (gint64) total_ms);
}

void
dictate_debuglog_tool (const char *name, const char *arguments, const char *result, double ms, gboolean ok)
{
  DictateDebugTurn *t = turn_now ();
  JsonObject *o;

  if (!t)
    return;
  o = step (t, "tool");
  json_object_set_string_member (o, "name", name);
  json_object_set_member (o, "arguments", as_json (arguments, arguments ? strlen (arguments) : 0));
  json_object_set_member (o, "result", as_json (result, result ? strlen (result) : 0));
  json_object_set_int_member (o, "ms", (gint64) ms);
  json_object_set_boolean_member (o, "ok", ok);
}

void
dictate_debuglog_note (const char *kind, const char *text)
{
  DictateDebugTurn *t = turn_now ();

  if (!t)
    return;
  json_object_set_string_member (step (t, kind), "text", text ? text : "");
}

/* --- the files */

static gint
newest_first (gconstpointer a, gconstpointer b)
{
  return -g_strcmp0 (*(char *const *) a, *(char *const *) b);
}

/* the turns' file names, newest first (their names start with when they began) */
static GPtrArray *
names (const char *dir)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    if (g_str_has_prefix (name, "turn-") && g_str_has_suffix (name, ".json"))
      g_ptr_array_add (all, g_strdup (name));
  g_ptr_array_sort (all, newest_first);
  return all;
}

/* the oldest turns removed until the rest fit */
static void
prune (const char *dir)
{
  g_autoptr (GPtrArray) all = names (dir);
  goffset bytes = 0;

  for (guint i = 0; i < all->len; i++)
    {
      g_autofree char *path = g_build_filename (dir, all->pdata[i], NULL);
      GStatBuf st;

      if (g_stat (path, &st) != 0)
        continue;
      bytes += st.st_size;
      if (i >= DEBUGLOG_KEEP || (i > 0 && bytes > DEBUGLOG_BYTES))
        g_unlink (path);
    }
}

void
dictate_debuglog_end (DictateDebugTurn *t, const char *reply, const char *outcome)
{
  g_autofree char *dir = dictate_debuglog_dir ();
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (GDateTime) started = NULL;
  g_autofree char *stamp = NULL, *name = NULL, *path = NULL, *text = NULL, *redacted = NULL;
  g_autoptr (GError) error = NULL;

  if (!t)
    return;
  if (turn_now () == t)
    g_private_set (&current, NULL);
  json_object_set_string_member (t->root, "reply", reply ? reply : "");
  json_object_set_string_member (t->root, "outcome", outcome ? outcome : "");
  json_object_set_int_member (t->root, "ms", (gint64) since (t));
  json_object_set_array_member (t->root, "steps", t->steps);
  root = json_node_init_object (json_node_alloc (), t->root);
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);
  redacted = dictate_debuglog_redact (text);

  started = g_date_time_new_from_unix_local_usec (t->started);
  stamp = g_date_time_format (started, "%Y%m%d-%H%M%S");
  name = g_strdup_printf ("turn-%s-%03d-%u.json", stamp, (int) (t->started / 1000 % 1000), t->turn);
  path = g_build_filename (dir, name, NULL);
  g_mutex_lock (&files);
  if (g_mkdir_with_parents (dir, 0700) != 0 || g_chmod (dir, 0700) != 0)
    g_warning ("debug log: %s: %s", dir, g_strerror (errno));
  else if (!g_file_set_contents_full (path, redacted, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("debug log: %s", error->message);
  else
    prune (dir);
  g_mutex_unlock (&files);

  json_object_unref (t->root);
  g_free (t->tools_sent);
  g_free (t);
}

void
dictate_debug_entry_free (DictateDebugEntry *entry)
{
  if (!entry)
    return;
  g_free (entry->path);
  g_free (entry->said);
  g_free (entry->outcome);
  g_free (entry);
}

JsonNode *
dictate_debuglog_read (const char *path)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_mapped_file (parser, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return NULL;
  return json_parser_steal_root (parser);
}

GPtrArray *
dictate_debuglog_list (void)
{
  g_autofree char *dir = dictate_debuglog_dir ();
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_debug_entry_free);
  g_autoptr (GPtrArray) all = NULL;

  g_mutex_lock (&files);
  all = names (dir);
  g_mutex_unlock (&files);
  for (guint i = 0; i < all->len; i++)
    {
      g_autofree char *path = g_build_filename (dir, all->pdata[i], NULL);
      g_autoptr (JsonNode) node = dictate_debuglog_read (path);
      JsonObject *o;
      JsonArray *steps;
      DictateDebugEntry *e;

      if (!node)
        continue;
      o = json_node_get_object (node);
      steps = json_object_has_member (o, "steps") ? json_object_get_array_member (o, "steps") : NULL;
      e = g_new0 (DictateDebugEntry, 1);
      e->path = g_steal_pointer (&path);
      e->started = json_object_get_int_member_with_default (o, "started_us", 0);
      e->said = g_strdup (json_object_get_string_member_with_default (o, "said", ""));
      e->outcome = g_strdup (json_object_get_string_member_with_default (o, "outcome", ""));
      e->ms = json_object_get_int_member_with_default (o, "ms", 0);
      e->steps = steps ? json_array_get_length (steps) : 0;
      for (guint s = 0; s < e->steps; s++)
        {
          JsonObject *st = json_array_get_object_element (steps, s);
          if (st && !g_strcmp0 (json_object_get_string_member_with_default (st, "kind", ""), "tool"))
            e->tools++;
        }
      g_ptr_array_add (entries, e);
    }
  return entries;
}

void
dictate_debuglog_clear (void)
{
  g_autofree char *dir = dictate_debuglog_dir ();
  g_autoptr (GPtrArray) all = NULL;

  g_mutex_lock (&files);
  all = names (dir);
  for (guint i = 0; i < all->len; i++)
    {
      g_autofree char *path = g_build_filename (dir, all->pdata[i], NULL);
      g_unlink (path);
    }
  g_mutex_unlock (&files);
}
