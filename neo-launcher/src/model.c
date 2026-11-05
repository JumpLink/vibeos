/* The home layout. The first start fills the dock from Phosh's favourites and the pages with every other app, A→Z;
 * apps installed later go to the first free cell. A cell may be empty, as on Launcher3's grid: an icon removed or
 * uninstalled leaves its cell empty, and a drag can drop an icon on any cell. The GJS Neo's file, plus null for an
 * empty cell. */
#include "model.h"
#include "apps.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* desktop ids that another app took over */
static const char *replaced[][2] = {
  { "de.yesman.Browser.desktop", "de.yesman.NeoBrowser.desktop" },
};

static char *
layout_file (void)
{
  return g_build_filename (g_get_user_config_dir (), "neo-launcher", "layout.json", NULL);
}

static GPtrArray *
id_list (void)
{
  return g_ptr_array_new_with_free_func (g_free);
}

static GPtrArray *
page_new (void)
{
  return id_list ();
}

Model *
model_new (int cols, int rows, int dock_size)
{
  Model *self = g_new0 (Model, 1);
  self->cols = cols;
  self->rows = rows;
  self->dock_size = dock_size;
  self->dock = id_list ();
  self->pages = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  self->hidden = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return self;
}

void
model_free (Model *self)
{
  if (!self)
    return;
  g_ptr_array_unref (self->dock);
  g_ptr_array_unref (self->pages);
  g_hash_table_unref (self->hidden);
  g_free (self);
}

int
model_per_page (Model *self)
{
  return self->cols * self->rows;
}

int
model_capacity (Model *self, int page)
{
  return MAX (1, model_per_page (self) - (page == 0 ? self->reserved : 0));
}

/* no empty cells at the end */
static void
trim (GPtrArray *cells)
{
  guint n = cells->len;
  while (n && !cells->pdata[n - 1])
    n--;
  g_ptr_array_set_size (cells, n);
}

/* a page with more icons than cells (the widget row came after them): its last empty cell closes up, or its last
 * icon goes on to the next page */
static void
spill (Model *self, guint from)
{
  for (guint p = from; p < self->pages->len; p++) {
    GPtrArray *cur = self->pages->pdata[p];
    trim (cur);
    while ((int) cur->len > model_capacity (self, p)) {
      guint hole = cur->len;
      for (guint i = 0; i < cur->len; i++)
        if (!cur->pdata[i])
          hole = i;
      if (hole < cur->len) {
        g_ptr_array_remove_index (cur, hole);
        continue;
      }
      if (p + 1 >= self->pages->len)
        g_ptr_array_add (self->pages, page_new ());
      GPtrArray *next = self->pages->pdata[p + 1];
      g_ptr_array_insert (next, 0, g_ptr_array_steal_index (cur, cur->len - 1));
    }
  }
}

void
model_set_reserved (Model *self, int slots)
{
  self->reserved = CLAMP (slots, 0, model_per_page (self) - 1);
  spill (self, 0);
}

void
model_set_hidden (Model *self, char **hidden)
{
  g_hash_table_remove_all (self->hidden);
  for (char **h = hidden; h && *h; h++)
    g_hash_table_add (self->hidden, g_strdup (*h));
}

static gboolean
list_has (GPtrArray *list, const char *id)
{
  for (guint i = 0; i < list->len; i++)
    if (!g_strcmp0 (list->pdata[i], id))
      return TRUE;
  return FALSE;
}

static guint
list_count (GPtrArray *list, const char *id)
{
  guint n = 0;
  for (guint i = 0; i < list->len; i++)
    n += !g_strcmp0 (list->pdata[i], id);
  return n;
}

/* every icon of `id` leaves its cell empty */
static void
list_clear_all (GPtrArray *list, const char *id)
{
  for (guint i = 0; i < list->len; i++)
    if (!g_strcmp0 (list->pdata[i], id))
      g_clear_pointer (&list->pdata[i], g_free);
  trim (list);
}

static void
list_replace_all (GPtrArray *list, const char *from, const char *to)
{
  for (guint i = 0; i < list->len; i++)
    if (!g_strcmp0 (list->pdata[i], from)) {
      g_free (list->pdata[i]);
      list->pdata[i] = g_strdup (to);
    }
}

gboolean
model_on_home (Model *self, const char *id)
{
  if (list_has (self->dock, id))
    return TRUE;
  for (guint p = 0; p < self->pages->len; p++)
    if (list_has (self->pages->pdata[p], id))
      return TRUE;
  return FALSE;
}

/* `id` in the first empty cell, on a new page when every page is full */
static void
place (Model *self, const char *id)
{
  for (guint p = 0; p < self->pages->len; p++) {
    GPtrArray *page = self->pages->pdata[p];
    for (guint i = 0; i < page->len; i++)
      if (!page->pdata[i]) {
        page->pdata[i] = g_strdup (id);
        return;
      }
    if ((int) page->len < model_capacity (self, p)) {
      g_ptr_array_add (page, g_strdup (id));
      return;
    }
  }
  GPtrArray *page = page_new ();
  g_ptr_array_add (page, g_strdup (id));
  g_ptr_array_add (self->pages, page);
}

static void
drop_empty_pages (Model *self)
{
  /* any empty page but the first: one emptied by a move, or skipped over while turning pages */
  for (guint p = self->pages->len; p-- > 1;) {
    GPtrArray *page = self->pages->pdata[p];
    trim (page);
    if (page->len == 0)
      g_ptr_array_remove_index (self->pages, p);
  }
}

/* ids and nulls (empty cells) */
static GPtrArray *
json_ids (JsonArray *arr)
{
  GPtrArray *list = id_list ();
  for (guint i = 0; arr && i < json_array_get_length (arr); i++) {
    JsonNode *n = json_array_get_element (arr, i);
    g_ptr_array_add (list, g_strdup (JSON_NODE_HOLDS_VALUE (n) ? json_node_get_string (n) : NULL));
  }
  trim (list);
  return list;
}

static void
fresh (Model *self, GHashTable *apps)
{
  g_auto (GStrv) favs = NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), "sm.puri.phosh", TRUE);
  if (schema) {
    g_autoptr (GSettings) phosh = g_settings_new ("sm.puri.phosh");
    favs = g_settings_get_strv (phosh, "favorites");
    g_settings_schema_unref (schema);
  }
  g_ptr_array_set_size (self->dock, 0);
  for (char **f = favs; f && *f && (int) self->dock->len < self->dock_size; f++)
    if (g_hash_table_contains (apps, *f))
      g_ptr_array_add (self->dock, g_strdup (*f));

  GList *rest = NULL;
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, apps);
  while (g_hash_table_iter_next (&it, &key, &value))
    if (!list_has (self->dock, key))
      rest = g_list_prepend (rest, value);
  rest = g_list_sort (rest, app_compare);

  g_ptr_array_set_size (self->pages, 0);
  for (GList *l = rest; l; l = l->next)
    place (self, g_app_info_get_id (l->data));
  g_list_free (rest);
  if (!self->pages->len)
    g_ptr_array_add (self->pages, page_new ());
}

void
model_load (Model *self, GHashTable *apps)
{
  g_autofree char *file = layout_file ();
  g_autoptr (JsonParser) parser = json_parser_new ();
  gboolean loaded = FALSE;

  if (json_parser_load_from_file (parser, file, NULL)) {
    JsonNode *root = json_parser_get_root (parser);
    JsonObject *obj = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    if (obj && json_object_has_member (obj, "dock") && json_object_has_member (obj, "pages")) {
      g_ptr_array_unref (self->dock);
      self->dock = json_ids (json_object_get_array_member (obj, "dock"));
      g_ptr_array_set_size (self->pages, 0);
      JsonArray *pages = json_object_get_array_member (obj, "pages");
      for (guint p = 0; pages && p < json_array_get_length (pages); p++) {
        JsonNode *n = json_array_get_element (pages, p);
        g_ptr_array_add (self->pages, json_ids (JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL));
      }
      loaded = TRUE;
    }
  }
  if (!loaded)
    fresh (self, apps);
  model_reconcile (self, apps);
  model_tidy (self);
}

gboolean
model_reconcile (Model *self, GHashTable *apps)
{
  gboolean changed = FALSE;

  /* an app that was replaced by another keeps its places (the dock slot, the home cells) */
  for (guint r = 0; r < G_N_ELEMENTS (replaced); r++) {
    const char *from = replaced[r][0], *to = replaced[r][1];
    if (!model_on_home (self, from) || g_hash_table_contains (apps, from) || !g_hash_table_contains (apps, to))
      continue;
    /* the new app's own icon (put on a free cell when it was installed) gives way to the old one's places */
    list_clear_all (self->dock, to);
    for (guint p = 0; p < self->pages->len; p++)
      list_clear_all (self->pages->pdata[p], to);
    list_replace_all (self->dock, from, to);
    for (guint p = 0; p < self->pages->len; p++)
      list_replace_all (self->pages->pdata[p], from, to);
    changed = TRUE;
  }

  /* uninstalled and hidden apps leave their cells empty */
  GPtrArray *lists[1 + self->pages->len];
  lists[0] = self->dock;
  for (guint p = 0; p < self->pages->len; p++)
    lists[1 + p] = self->pages->pdata[p];
  for (guint l = 0; l < G_N_ELEMENTS (lists); l++) {
    for (guint i = 0; i < lists[l]->len; i++) {
      const char *id = lists[l]->pdata[i];
      if (id && (!g_hash_table_contains (apps, id) || g_hash_table_contains (self->hidden, id))) {
        g_clear_pointer (&lists[l]->pdata[i], g_free);
        changed = TRUE;
      }
    }
    trim (lists[l]);
  }

  /* a smaller dock (dock-num-icons) loses its extra icons; they come back on the pages below */
  if ((int) self->dock->len > self->dock_size) {
    g_ptr_array_set_size (self->dock, self->dock_size);
    changed = TRUE;
  }

  /* new apps: the first free cell, A→Z among themselves */
  GList *fresh_apps = NULL;
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, apps);
  while (g_hash_table_iter_next (&it, &key, &value))
    if (!model_on_home (self, key) && !g_hash_table_contains (self->hidden, key))
      fresh_apps = g_list_prepend (fresh_apps, value);
  fresh_apps = g_list_sort (fresh_apps, app_compare);
  for (GList *l = fresh_apps; l; l = l->next) {
    place (self, g_app_info_get_id (l->data));
    changed = TRUE;
  }
  g_list_free (fresh_apps);

  /* a page it emptied stays until the caller tidies (home_tidy_pages keeps the page on show where it is) */
  if (!self->pages->len)
    g_ptr_array_add (self->pages, page_new ());
  if (changed)
    model_save (self);
  return changed;
}

/* Takes one icon away, leaving its cell empty: the one at `from` when it is `id`, else every icon of `id`. An app
 * may sit in several places (a home shortcut of a dock app). */
static void
take (Model *self, const char *id, ModelPos from)
{
  GPtrArray *cells = model_cells (self, from);
  int i = from.dock >= 0 ? from.dock : from.index;
  if (cells && i >= 0 && i < (int) cells->len && !g_strcmp0 (cells->pdata[i], id)) {
    g_clear_pointer (&cells->pdata[i], g_free);
    trim (cells);
    return;
  }
  list_clear_all (self->dock, id);
  for (guint p = 0; p < self->pages->len; p++)
    list_clear_all (self->pages->pdata[p], id);
}

gboolean
model_remove_one (Model *self, const char *id, ModelPos from)
{
  guint count = list_count (self->dock, id);
  for (guint p = 0; p < self->pages->len; p++)
    count += list_count (self->pages->pdata[p], id);
  if (count < 2)
    return FALSE;
  take (self, id, from);
  model_save (self);
  return TRUE;
}

void
model_add_shortcut (Model *self, const char *id)
{
  place (self, id);
  model_save (self);
}

GPtrArray *
model_cells (Model *self, ModelPos pos)
{
  if (pos.dock >= 0)
    return self->dock;
  return pos.page >= 0 && pos.page < (int) self->pages->len ? self->pages->pdata[pos.page] : NULL;
}

const char *
model_at (Model *self, ModelPos pos)
{
  GPtrArray *cells = model_cells (self, pos);
  int i = pos.dock >= 0 ? pos.dock : pos.index;
  return cells && i >= 0 && i < (int) cells->len ? cells->pdata[i] : NULL;
}

void
model_drop (Model *self, const char *id, ModelPos from, gboolean copy, int page, char **cells, int n)
{
  g_autofree char *moving = g_strdup (id);
  if (!copy)
    take (self, moving, from);
  GPtrArray *to;
  if (page < 0) {
    to = self->dock;
  } else {
    while ((int) self->pages->len <= page)
      g_ptr_array_add (self->pages, page_new ());
    to = self->pages->pdata[page];
  }
  g_ptr_array_set_size (to, 0);
  for (int i = 0; i < n; i++)
    g_ptr_array_add (to, g_strdup (cells[i]));
  trim (to);
  model_save (self);
}

void
model_tidy (Model *self)
{
  guint before = self->pages->len;
  drop_empty_pages (self);
  if (self->pages->len != before)
    model_save (self);
}

static void
add_ids (JsonBuilder *b, GPtrArray *list)
{
  json_builder_begin_array (b);
  for (guint i = 0; i < list->len; i++)
    if (list->pdata[i])
      json_builder_add_string_value (b, list->pdata[i]);
    else
      json_builder_add_null_value (b);
  json_builder_end_array (b);
}

void
model_save (Model *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "dock");
  add_ids (b, self->dock);
  json_builder_set_member_name (b, "pages");
  json_builder_begin_array (b);
  for (guint p = 0; p < self->pages->len; p++)
    add_ids (b, self->pages->pdata[p]);
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_indent (gen, 1);
  g_autofree char *data = json_generator_to_data (gen, NULL);
  g_autofree char *file = layout_file ();
  g_autofree char *dir = g_path_get_dirname (file);
  g_autoptr (GError) err = NULL;
  g_mkdir_with_parents (dir, 0755);
  if (!g_file_set_contents (file, data, -1, &err))
    g_warning ("saving the layout: %s", err->message);
}
