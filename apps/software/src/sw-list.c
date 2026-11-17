/* Lists of apps, page after page, and the categories page.
 *
 * A list asks for its first page (kept and revalidated as every catalog read), then for the next one with the
 * server's cursor whenever the end comes within a screen of view. An app that a changed list repeats on the next page
 * is shown once (the API says to de-duplicate by id). An error on a later page leaves what is shown and puts Retry at
 * the end. */
#include "sw-pages.h"
#include "sw-json.h"
#include "sw-widgets.h"
#include "sw-window.h"

#include <spongy-gtk.h>

#define NEAR_END 600

typedef struct {
  GtkWidget  *list, *status, *scroll, *column, *rows, *footer;
  char       *path, *cursor;
  GHashTable *seen;
  gboolean    loading, done, first_shown;
  SwListMeta  meta_func;
  gpointer    meta_data;
  char       *empty;       /* the sentence for no apps at all */
} List;

static void load_more (List *list);

static void
list_free (gpointer data)
{
  List *list = data;

  g_free (list->path);
  g_free (list->empty);
  g_free (list->cursor);
  g_hash_table_unref (list->seen);
  g_free (list);
}

static void
set_footer (List *list, GtkWidget *child)
{
  GtkWidget *old;

  while ((old = gtk_widget_get_first_child (list->footer)))
    gtk_box_remove (GTK_BOX (list->footer), old);
  if (child)
    gtk_box_append (GTK_BOX (list->footer), child);
}

static void
append (List *list, JsonArray *items)
{
  for (guint i = 0; items && i < json_array_get_length (items); i++)
    {
      JsonObject *app = sw_json_array_object (items, i);
      const char *id = sw_json_string (app, "id");

      if (!id || g_hash_table_contains (list->seen, id))
        continue;
      g_hash_table_add (list->seen, g_strdup (id));
      gtk_box_append (GTK_BOX (list->rows), sw_app_row_new (app, NULL));
    }
}

static void
clear (List *list)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (list->rows)))
    gtk_box_remove (GTK_BOX (list->rows), child);
  g_hash_table_remove_all (list->seen);
}

static void
check_near_end (List *list)
{
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (list->scroll));

  if (list->loading || list->done || !list->cursor || !gtk_widget_get_mapped (list->list))
    return;
  if (gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj) + NEAR_END >= gtk_adjustment_get_upper (adj))
    load_more (list);
}

static void
on_scrolled (GtkAdjustment *adj, List *list)
{
  check_near_end (list);
}

static void
on_more (SwApiResult *result, const SwApiError *error, gpointer data)
{
  List *list = data;

  list->loading = FALSE;
  if (error)
    {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
      g_autofree char *text = sw_api_error_describe (error);
      GtkWidget *label = sw_label_new (text, "sw-list-error"), *button = sw_button_new ("Retry", SW_BUTTON_TONAL);

      gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
      gtk_label_set_xalign (GTK_LABEL (label), 0.5);
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      g_signal_connect_swapped (button, "clicked", G_CALLBACK (load_more), list);
      gtk_box_append (GTK_BOX (box), label);
      gtk_box_append (GTK_BOX (box), button);
      set_footer (list, box);
      return;
    }
  append (list, sw_json_node_array (result->data));
  g_free (list->cursor);
  list->cursor = g_strdup (result->next_cursor);
  list->done = !list->cursor;
  set_footer (list, NULL);
  /* the page may not fill the screen yet */
  check_near_end (list);
}

static void
load_more (List *list)
{
  g_autofree char *path = sw_api_path_with_cursor (list->path, list->cursor);
  GtkWidget *ring = sw_progress_new (32, 3);

  if (list->loading)
    return;
  list->loading = TRUE;
  gtk_widget_set_halign (ring, GTK_ALIGN_CENTER);
  set_footer (list, ring);
  sw_api_get (sw_api_get_default (), path, SW_API_NONE, sw_cancellable_for (list->list), on_more, list);
}

static void load_first (List *list);

static void
on_first (SwApiResult *result, const SwApiError *error, gpointer data)
{
  List *list = data;
  JsonArray *items;

  if (error)
    {
      if (!list->first_shown)
        sw_status_error (SW_STATUS (list->status), error, (SwRetryFunc) load_first, list);
      list->loading = FALSE;
      return;
    }
  /* the server confirmed what is shown: the pages after it still hold */
  if (result->not_modified && list->first_shown)
    {
      list->loading = FALSE;
      check_near_end (list);
      return;
    }
  items = sw_json_node_array (result->data);
  clear (list);
  append (list, items);
  g_free (list->cursor);
  list->cursor = g_strdup (result->next_cursor);
  list->done = !list->cursor;
  list->first_shown = TRUE;
  if (!result->stale)
    list->loading = FALSE;
  if (list->meta_func)
    list->meta_func (list->list, result->meta, list->meta_data);
  if (!items || !json_array_get_length (items))
    sw_status_empty (SW_STATUS (list->status), "sw-apps-symbolic", list->empty ? list->empty : "No apps here yet.",
                     NULL, NULL, NULL);
  else
    {
      sw_status_show (SW_STATUS (list->status));
      check_near_end (list);
    }
}

static void
load_first (List *list)
{
  list->loading = TRUE;
  if (!list->first_shown)
    sw_status_loading (SW_STATUS (list->status));
  sw_api_get (sw_api_get_default (), list->path, SW_API_CACHED | SW_API_STALE_FIRST, sw_cancellable_for (list->list),
              on_first, list);
}

static void
on_map (GtkWidget *widget, List *list)
{
  if (!list->first_shown && !list->loading)
    load_first (list);
}

GtkWidget *
sw_app_list_new (const char *path)
{
  List *list = g_new0 (List, 1);

  list->path = g_strdup (path);
  list->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  list->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (list->column, "sw-list");
  list->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  list->footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (list->footer, "sw-list-footer");
  gtk_box_append (GTK_BOX (list->column), list->rows);
  gtk_box_append (GTK_BOX (list->column), list->footer);
  list->scroll = sw_scroll_new (list->column);
  list->status = sw_status_new (list->scroll);
  list->list = list->status;
  g_object_set_data_full (G_OBJECT (list->list), "sw-list", list, list_free);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (list->scroll)), "value-changed",
                    G_CALLBACK (on_scrolled), list);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (list->scroll)), "changed",
                    G_CALLBACK (on_scrolled), list);
  g_signal_connect (list->list, "map", G_CALLBACK (on_map), list);
  return list->list;
}

void
sw_app_list_set_meta_func (GtkWidget *widget, SwListMeta func, gpointer data)
{
  List *list = g_object_get_data (G_OBJECT (widget), "sw-list");

  list->meta_func = func;
  list->meta_data = data;
}

void
sw_app_list_set_empty (GtkWidget *widget, const char *text)
{
  List *list = g_object_get_data (G_OBJECT (widget), "sw-list");

  g_free (list->empty);
  list->empty = g_strdup (text);
}

void
sw_app_list_set_header (GtkWidget *widget, GtkWidget *header)
{
  List *list = g_object_get_data (G_OBJECT (widget), "sw-list");
  GtkWidget *old = g_object_get_data (G_OBJECT (widget), "sw-list-header");

  if (old)
    gtk_box_remove (GTK_BOX (list->column), old);
  if (header)
    gtk_box_prepend (GTK_BOX (list->column), header);
  g_object_set_data (G_OBJECT (widget), "sw-list-header", header);
}

GtkWidget *
sw_list_page_new (const char *title, const char *path)
{
  GtkWidget *page = sw_page_new (title ? title : "Apps");

  sw_page_set_body (page, sw_app_list_new (path));
  return page;
}

/* ---- categories ------------------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *page, *status, *cards;
  char      *type;
} Categories;

static void load_categories (Categories *c);

static void
on_category_row (VibeosListRow *row, gpointer data)
{
  JsonObject *category = g_object_get_data (G_OBJECT (row), "sw-category");
  g_autofree char *id = sw_api_escape (sw_json_string (category, "id"));
  g_autofree char *path = g_strdup_printf ("/apps?category=%s", id);

  sw_window_open_list (GTK_WIDGET (row), sw_json_string (category, "name"), path);
}

static void
on_categories (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Categories *c = data;
  JsonArray *list;

  if (error)
    {
      sw_status_error (SW_STATUS (c->status), error, (SwRetryFunc) load_categories, c);
      return;
    }
  list = sw_json_node_array (result->data);
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (c->cards));
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *category = sw_json_array_object (list, i);
      gint64 count = sw_json_int (category, "app_count", 0);
      g_autofree char *summary = NULL;
      g_autofree char *icon = NULL;
      GtkWidget *row;
      GdkRGBA fill = vibeos_color (VIBEOS_SECONDARY_CONTAINER), glyph = vibeos_color (VIBEOS_ON_SECONDARY_CONTAINER);

      /* empty categories have nothing to open */
      if (!category || count <= 0)
        continue;
      summary = g_strdup_printf (count == 1 ? "%" G_GINT64_FORMAT " app" : "%" G_GINT64_FORMAT " apps", count);
      row = vibeos_list_row_new (sw_json_string (category, "name"), summary);
      icon = g_strdup_printf ("sw-fa-%s-symbolic", sw_json_string (category, "icon"));
      if (!gtk_icon_theme_has_icon (gtk_icon_theme_get_for_display (gdk_display_get_default ()), icon))
        {
          g_free (icon);
          icon = g_strdup ("sw-fa-shapes-symbolic");
        }
      vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (row), icon, &fill, &glyph);
      vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
      g_object_set_data_full (G_OBJECT (row), "sw-category", json_object_ref (category),
                              (GDestroyNotify) json_object_unref);
      g_signal_connect (row, "clicked", G_CALLBACK (on_category_row), NULL);
      vibeos_card_list_append (VIBEOS_CARD_LIST (c->cards), row);
    }
  sw_status_show (SW_STATUS (c->status));
}

static void
load_categories (Categories *c)
{
  g_autofree char *path = g_strdup_printf ("/categories?type=%s", c->type);

  sw_status_loading (SW_STATUS (c->status));
  sw_api_get (sw_api_get_default (), path, SW_API_CACHED | SW_API_STALE_FIRST, sw_cancellable_for (c->page),
              on_categories, c);
}

static void
categories_free (gpointer data)
{
  Categories *c = data;

  g_free (c->type);
  g_free (c);
}

GtkWidget *
sw_categories_page_new (const char *type)
{
  Categories *c = g_new0 (Categories, 1);

  c->type = g_strdup (!g_strcmp0 (type, "games") ? "games" : "apps");
  c->page = sw_page_new (!g_strcmp0 (c->type, "games") ? "Game categories" : "App categories");
  c->cards = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (c->cards), 8, 24);
  c->status = sw_status_new (sw_scroll_new (c->cards));
  sw_page_set_body (c->page, c->status);
  g_object_set_data_full (G_OBJECT (c->page), "sw-categories", c, categories_free);
  load_categories (c);
  return c->page;
}
