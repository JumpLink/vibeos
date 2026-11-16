#include "store.h"

#include <glib/gstdio.h>
#include <string.h>

#define HISTORY_MAX 2000
#define SAVE_DELAY_MS 800        /* writes are batched: a page load touches history several times */

void
nb_entry_free (NbEntry *e)
{
  g_free (e->url);
  g_free (e->title);
  g_free (e->color);
  g_free (e);
}

static void
entry_free (gpointer p)
{
  nb_entry_free (p);
}

static NbEntry *
entry_new (const char *url, const char *title, double time, int visits)
{
  NbEntry *e = g_new0 (NbEntry, 1);

  e->url = g_strdup (url);
  e->title = g_strdup (title ? title : "");
  e->time = time;
  e->visits = visits;
  return e;
}

static double
now_s (void)
{
  return g_get_real_time () / 1e6;
}

char *
nb_host (const char *url)
{
  if (!url)
    return g_strdup ("");
  const char *sep = strstr (url, "://");
  if (!sep)
    return g_strdup (url);
  const char *start = sep + 3, *end = start;
  while (*end && *end != '/')
    end++;
  if (end - start > 4 && g_ascii_strncasecmp (start, "www.", 4) == 0)
    start += 4;
  return g_strndup (start, end - start);
}

char *
nb_letter (const char *url)
{
  g_autofree char *host = nb_host (url);

  if (!*host)
    return g_strdup ("?");
  gunichar c = g_unichar_toupper (g_utf8_get_char_validated (host, -1));
  char buf[8] = {0};
  g_unichar_to_utf8 (c ? c : '?', buf);
  return g_strdup (buf);
}

/* ---- files ---- */

JsonNode *
nb_store_load (NbStore *store, const char *name)
{
  g_autofree char *path = g_build_filename (store->dir, name, NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_file (parser, path, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  return root ? json_node_copy (root) : NULL;
}

void
nb_store_write_node (NbStore *store, const char *name, JsonNode *node)
{
  g_autofree char *path = g_build_filename (store->dir, name, NULL);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (GError) error = NULL;

  json_generator_set_root (gen, node);
  g_autofree char *text = json_generator_to_data (gen, NULL);
  if (!g_file_set_contents (path, text, -1, &error))
    g_warning ("vibeos-browser: %s not saved: %s", name, error->message);
}

static JsonNode *
entries_node (GPtrArray *entries, gboolean visits)
{
  JsonArray *array = json_array_sized_new (entries->len);

  for (guint i = 0; i < entries->len; i++) {
    NbEntry *e = entries->pdata[i];
    JsonObject *o = json_object_new ();
    json_object_set_string_member (o, "url", e->url);
    json_object_set_string_member (o, "title", e->title);
    json_object_set_double_member (o, "time", e->time);
    if (visits)
      json_object_set_int_member (o, "visits", e->visits);
    if (e->color)
      json_object_set_string_member (o, "color", e->color);
    json_array_add_object_element (array, o);
  }
  JsonNode *node = json_node_new (JSON_NODE_ARRAY);
  json_node_take_array (node, array);
  return node;
}

static GPtrArray *
entries_from (JsonNode *node)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (entry_free);

  if (!node || !JSON_NODE_HOLDS_ARRAY (node))
    return out;
  JsonArray *array = json_node_get_array (node);
  for (guint i = 0; i < json_array_get_length (array); i++) {
    JsonNode *n = json_array_get_element (array, i);
    if (!JSON_NODE_HOLDS_OBJECT (n))
      continue;
    JsonObject *o = json_node_get_object (n);
    const char *url = json_object_get_string_member_with_default (o, "url", NULL);
    if (!url || !*url)
      continue;
    NbEntry *e = entry_new (url, json_object_get_string_member_with_default (o, "title", ""),
                            json_object_get_double_member_with_default (o, "time", 0),
                            (int) json_object_get_int_member_with_default (o, "visits", 1));
    const char *color = json_object_get_string_member_with_default (o, "color", NULL);
    e->color = color && *color ? g_strdup (color) : NULL;
    g_ptr_array_add (out, e);
  }
  return out;
}

static void
write_now (NbStore *store, const char *name)
{
  JsonNode *node;

  if (g_str_equal (name, "history.json"))
    node = entries_node (store->history, TRUE);
  else if (g_str_equal (name, "bookmarks.json"))
    node = entries_node (store->bookmarks, FALSE);
  else {
    node = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object (node, store->prefs);
  }
  nb_store_write_node (store, name, node);
  json_node_unref (node);
}

typedef struct {
  NbStore *store;
  char    *name;
} Pending;

static gboolean
on_save (gpointer data)
{
  Pending *p = data;

  g_hash_table_remove (p->store->pending, p->name);
  write_now (p->store, p->name);
  return G_SOURCE_REMOVE;
}

static void
pending_free (gpointer data)
{
  Pending *p = data;

  g_free (p->name);
  g_free (p);
}

static void
save (NbStore *store, const char *name)
{
  if (g_hash_table_contains (store->pending, name))
    return;
  Pending *p = g_new0 (Pending, 1);
  p->store = store;
  p->name = g_strdup (name);
  guint id = g_timeout_add_full (G_PRIORITY_LOW, SAVE_DELAY_MS, on_save, p, pending_free);
  g_hash_table_insert (store->pending, g_strdup (name), GUINT_TO_POINTER (id));
}

void
nb_store_flush (NbStore *store)
{
  g_autoptr (GPtrArray) names = g_hash_table_get_keys_as_ptr_array (store->pending);

  for (guint i = 0; i < names->len; i++) {
    g_autofree char *name = g_strdup (names->pdata[i]);
    g_source_remove (GPOINTER_TO_UINT (g_hash_table_lookup (store->pending, name)));
    g_hash_table_remove (store->pending, name);
    write_now (store, name);
  }
}

NbStore *
nb_store_new (const char *dir, const char *old_dir)
{
  NbStore *store = g_new0 (NbStore, 1);
  static const char *const carried[] = {"history.json", "bookmarks.json"};

  store->dir = g_strdup (dir);
  g_mkdir_with_parents (dir, 0700);
  /* the Qt browser's history and bookmarks, once */
  for (guint i = 0; i < G_N_ELEMENTS (carried); i++) {
    g_autofree char *to = g_build_filename (dir, carried[i], NULL);
    g_autofree char *from = g_build_filename (old_dir, carried[i], NULL);
    if (!g_file_test (to, G_FILE_TEST_EXISTS) && g_file_test (from, G_FILE_TEST_EXISTS)) {
      g_autoptr (GFile) src = g_file_new_for_path (from);
      g_autoptr (GFile) dst = g_file_new_for_path (to);
      g_file_copy (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    }
  }
  store->pending = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  JsonNode *node = nb_store_load (store, "history.json");
  store->history = entries_from (node);
  g_clear_pointer (&node, json_node_unref);
  node = nb_store_load (store, "bookmarks.json");
  store->bookmarks = entries_from (node);
  g_clear_pointer (&node, json_node_unref);
  node = nb_store_load (store, "prefs.json");
  if (node && JSON_NODE_HOLDS_OBJECT (node))
    store->prefs = json_object_ref (json_node_get_object (node));
  else
    store->prefs = json_object_new ();
  g_clear_pointer (&node, json_node_unref);
  if (!json_object_has_member (store->prefs, "adblock"))
    json_object_set_boolean_member (store->prefs, "adblock", TRUE);
  return store;
}

void
nb_store_free (NbStore *store)
{
  nb_store_flush (store);
  g_hash_table_unref (store->pending);
  g_ptr_array_unref (store->history);
  g_ptr_array_unref (store->bookmarks);
  json_object_unref (store->prefs);
  g_free (store->dir);
  g_free (store);
}

/* ---- history ---- */

static gboolean
skipped (const char *url)
{
  return !url || !*url || g_str_has_prefix (url, "about:") || g_str_has_prefix (url, "data:") ||
         g_str_has_prefix (url, "blob:") || g_str_has_prefix (url, "chrome:");
}

static int
find_url (GPtrArray *entries, const char *url)
{
  for (guint i = 0; i < entries->len; i++)
    if (g_str_equal (((NbEntry *) entries->pdata[i])->url, url))
      return i;
  return -1;
}

void
nb_store_visit (NbStore *store, const char *url, const char *title)
{
  if (skipped (url))
    return;
  int at = find_url (store->history, url);
  int visits = 1;
  g_autofree char *old_title = NULL;
  if (at >= 0) {
    NbEntry *old = store->history->pdata[at];
    visits = old->visits + 1;
    old_title = g_strdup (old->title);
    g_ptr_array_remove_index (store->history, at);
  }
  g_ptr_array_insert (store->history, 0,
                      entry_new (url, title && *title ? title : old_title ? old_title : "", now_s (), visits));
  if (store->history->len > HISTORY_MAX)
    g_ptr_array_set_size (store->history, HISTORY_MAX);
  save (store, "history.json");
}

void
nb_store_retitle (NbStore *store, const char *url, const char *title)
{
  if (!url || !title || !*title)
    return;
  int at = find_url (store->history, url);
  if (at < 0)
    return;
  NbEntry *e = store->history->pdata[at];
  if (g_str_equal (e->title, title))
    return;
  g_free (e->title);
  e->title = g_strdup (title);
  save (store, "history.json");
}

void
nb_store_clear_history (NbStore *store)
{
  g_ptr_array_set_size (store->history, 0);
  save (store, "history.json");
}

static gboolean
contains_fold (const char *hay, const char *needle_lower)
{
  g_autofree char *lower = g_utf8_strdown (hay ? hay : "", -1);
  return strstr (lower, needle_lower) != NULL;
}

/* the URL without its scheme starts with the text: such a hit goes first */
static gboolean
url_starts (const NbEntry *e, const char *t)
{
  const char *sep = strstr (e->url, "://");
  g_autofree char *lower = g_utf8_strdown (sep ? sep + 3 : e->url, -1);
  return g_str_has_prefix (lower, t);
}

static const char *suggest_text;

/* g_ptr_array_sort_values () hands the comparison the array's pointers themselves (NbEntry *), not pointers to them
 * as g_ptr_array_sort () does: dereferencing once more read a URL's text as a pointer and crashed at the first
 * keystroke with two hits */
static int
by_suggestion (gconstpointer a, gconstpointer b)
{
  const NbEntry *x = a, *y = b;
  int sx = url_starts (x, suggest_text), sy = url_starts (y, suggest_text);

  if (sx != sy)
    return sy - sx;
  return y->visits - x->visits;
}

GPtrArray *
nb_store_suggest (NbStore *store, const char *text, guint limit)
{
  GPtrArray *hits = g_ptr_array_new ();
  g_autofree char *stripped = g_strstrip (g_strdup (text));
  g_autofree char *t = g_utf8_strdown (stripped, -1);

  if (!*t)
    return hits;
  for (guint i = 0; i < store->history->len; i++) {
    NbEntry *e = store->history->pdata[i];
    if (contains_fold (e->url, t) || contains_fold (e->title, t))
      g_ptr_array_add (hits, e);
  }
  suggest_text = t;
  g_ptr_array_sort_values (hits, by_suggestion);   /* stable, as JS's sort */
  suggest_text = NULL;
  if (hits->len > limit)
    g_ptr_array_set_size (hits, limit);
  return hits;
}

static int
by_visits (gconstpointer a, gconstpointer b)
{
  return ((const NbEntry *) b)->visits - ((const NbEntry *) a)->visits;   /* the entries themselves, as above */
}

GPtrArray *
nb_store_top_sites (NbStore *store, guint limit)
{
  g_autoptr (GPtrArray) sorted = g_ptr_array_copy (store->history, NULL, NULL);
  g_ptr_array_set_free_func (sorted, NULL);   /* the copy takes the history's free func: it would free the entries */
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *out = g_ptr_array_new ();

  g_ptr_array_sort_values (sorted, by_visits);
  for (guint i = 0; i < sorted->len && out->len < limit; i++) {
    NbEntry *e = sorted->pdata[i];
    char *host = nb_host (e->url);
    if (g_hash_table_contains (seen, host)) {
      g_free (host);
      continue;
    }
    g_hash_table_add (seen, host);
    g_ptr_array_add (out, e);
  }
  return out;
}

/* ---- bookmarks ---- */

gboolean
nb_store_is_bookmarked (NbStore *store, const char *url)
{
  return url && find_url (store->bookmarks, url) >= 0;
}

void
nb_store_toggle_bookmark (NbStore *store, const char *url, const char *title)
{
  int at = find_url (store->bookmarks, url);

  if (at >= 0)
    g_ptr_array_remove_index (store->bookmarks, at);
  else
    g_ptr_array_insert (store->bookmarks, 0, entry_new (url, title, now_s (), 0));
  save (store, "bookmarks.json");
}

/* ---- preferences ---- */

gboolean
nb_store_adblock (NbStore *store)
{
  return json_object_get_boolean_member_with_default (store->prefs, "adblock", TRUE);
}

void
nb_store_set_adblock (NbStore *store, gboolean on)
{
  json_object_set_boolean_member (store->prefs, "adblock", on);
  save (store, "prefs.json");
}

double
nb_store_filter_time (NbStore *store)
{
  return json_object_get_double_member_with_default (store->prefs, "filterTime", 0);
}

void
nb_store_set_filter_time (NbStore *store, double t)
{
  json_object_set_double_member (store->prefs, "filterTime", t);
  save (store, "prefs.json");
}

/* ---- favourites (Jelly) ---- */

void
nb_store_add_favorite (NbStore *store, const char *url, const char *title, const char *color)
{
  int at = find_url (store->bookmarks, url);

  if (at >= 0) {
    NbEntry *e = store->bookmarks->pdata[at];
    g_free (e->title);
    e->title = g_strdup (title ? title : "");
    if (color) {
      g_free (e->color);
      e->color = g_strdup (color);
    }
  } else {
    NbEntry *e = entry_new (url, title, now_s (), 0);
    e->color = g_strdup (color);
    g_ptr_array_insert (store->bookmarks, 0, e);
  }
  save (store, "bookmarks.json");
}

void
nb_store_edit_favorite (NbStore *store, const char *old_url, const char *url, const char *title)
{
  int at = find_url (store->bookmarks, old_url);

  if (at < 0)
    return;
  NbEntry *e = store->bookmarks->pdata[at];
  g_free (e->url);
  e->url = g_strdup (url);
  g_free (e->title);
  e->title = g_strdup (title ? title : "");
  save (store, "bookmarks.json");
}

void
nb_store_remove_favorite (NbStore *store, const char *url)
{
  int at = find_url (store->bookmarks, url);

  if (at >= 0) {
    g_ptr_array_remove_index (store->bookmarks, at);
    save (store, "bookmarks.json");
  }
}

NbEntry *
nb_store_take_history (NbStore *store, guint index)
{
  if (index >= store->history->len)
    return NULL;
  NbEntry *e = g_ptr_array_steal_index (store->history, index);
  save (store, "history.json");
  return e;
}

void
nb_store_put_history (NbStore *store, guint index, NbEntry *entry)
{
  /* a page visited meanwhile may have brought the same URL back: the newer one stays */
  if (find_url (store->history, entry->url) >= 0) {
    nb_entry_free (entry);
    return;
  }
  g_ptr_array_insert (store->history, MIN (index, store->history->len), entry);
  save (store, "history.json");
}

/* ---- Jelly's settings ---- */

gboolean
nb_store_get_bool (NbStore *store, const char *key, gboolean fallback)
{
  return json_object_get_boolean_member_with_default (store->prefs, key, fallback);
}

void
nb_store_set_bool (NbStore *store, const char *key, gboolean value)
{
  json_object_set_boolean_member (store->prefs, key, value);
  save (store, "prefs.json");
}

int
nb_store_get_origin (NbStore *store, const char *key, const char *origin)
{
  JsonNode *n = json_object_get_member (store->prefs, key);
  if (!n || !JSON_NODE_HOLDS_OBJECT (n))
    return -1;
  JsonNode *v = json_object_get_member (json_node_get_object (n), origin);
  if (!v || json_node_get_value_type (v) != G_TYPE_BOOLEAN)
    return -1;
  return json_node_get_boolean (v);
}

void
nb_store_set_origin (NbStore *store, const char *key, const char *origin, gboolean value)
{
  JsonNode *n = json_object_get_member (store->prefs, key);
  JsonObject *o;
  if (n && JSON_NODE_HOLDS_OBJECT (n)) {
    o = json_node_get_object (n);
  } else {
    o = json_object_new ();
    json_object_set_object_member (store->prefs, key, o);
  }
  json_object_set_boolean_member (o, origin, value);
  save (store, "prefs.json");
}

const char *
nb_store_get_string (NbStore *store, const char *key, const char *fallback)
{
  const char *v = json_object_get_string_member_with_default (store->prefs, key, NULL);
  return v ? v : fallback;
}

void
nb_store_set_string (NbStore *store, const char *key, const char *value)
{
  json_object_set_string_member (store->prefs, key, value);
  save (store, "prefs.json");
}

GStrv
nb_store_get_strv (NbStore *store, const char *key)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  JsonNode *n = json_object_get_member (store->prefs, key);

  if (n && JSON_NODE_HOLDS_ARRAY (n)) {
    JsonArray *a = json_node_get_array (n);
    for (guint i = 0; i < json_array_get_length (a); i++) {
      JsonNode *e = json_array_get_element (a, i);
      if (JSON_NODE_HOLDS_VALUE (e) && json_node_get_value_type (e) == G_TYPE_STRING)
        g_strv_builder_add (b, json_node_get_string (e));
    }
  }
  return g_strv_builder_end (b);
}

void
nb_store_set_strv (NbStore *store, const char *key, const char *const *value)
{
  JsonArray *a = json_array_new ();

  for (int i = 0; value && value[i]; i++)
    json_array_add_string_element (a, value[i]);
  json_object_set_array_member (store->prefs, key, a);
  save (store, "prefs.json");
}
