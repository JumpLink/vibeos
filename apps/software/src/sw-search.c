/* Search: VibeOS's search view over the window for typing, and a page for the results.
 *
 * While typing, GET /search/suggest (after a short pause, the last request dropped when another key comes) suggests
 * apps (a tap opens one) and words that complete the query (a tap searches for it, the arrow puts it in the field to
 * type on). With nothing typed, the recent searches show. Enter, or a suggested word, closes the view and opens the
 * results: GET /search, page after page, with "Showing results for …" when the store corrected a word. The results
 * page's search button opens the view again with the query. */
#include "sw-pages.h"
#include "sw-image.h"
#include "sw-json.h"
#include "sw-widgets.h"
#include "sw-window.h"

#define SUGGEST_MS 200
#define RECENT_MAX 8

typedef struct {
  GtkWidget    *view, *from, *box;
  char         *type;
  GCancellable *cancellable;
  guint         timeout;
  GSettings    *settings;
} Search;

static GSettings *
settings_new (void)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                             "vibeos.Software", TRUE);

  if (!schema)
    return NULL;
  g_settings_schema_unref (schema);
  return g_settings_new ("vibeos.Software");
}

static void
search_free (gpointer data)
{
  Search *s = data;

  g_clear_handle_id (&s->timeout, g_source_remove);
  if (s->cancellable)
    g_cancellable_cancel (s->cancellable);
  g_clear_object (&s->cancellable);
  g_clear_object (&s->settings);
  g_clear_object (&s->from);
  g_free (s->type);
  g_free (s);
}

static void
clear_box (Search *s)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (s->box)))
    gtk_box_remove (GTK_BOX (s->box), child);
}

static void
remember (Search *s, const char *query)
{
  g_auto (GStrv) old = s->settings ? g_settings_get_strv (s->settings, "recent-searches") : NULL;
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_auto (GStrv) now = NULL;
  guint n = 1;

  if (!s->settings)
    return;
  g_strv_builder_add (b, query);
  for (guint i = 0; old && old[i] && n < RECENT_MAX; i++)
    if (g_utf8_collate (old[i], query))
      {
        g_strv_builder_add (b, old[i]);
        n++;
      }
  now = g_strv_builder_end (b);
  g_settings_set_strv (s->settings, "recent-searches", (const char *const *) now);
}

/* ---- the results page ------------------------------------------------------------------------------------------ */

static void
on_meta (GtkWidget *list, JsonObject *meta, gpointer data)
{
  const char *corrected = sw_json_string (meta, "corrected_query");
  g_autofree char *text = NULL;
  GtkWidget *label;

  if (!corrected)
    {
      sw_app_list_set_header (list, NULL);
      return;
    }
  text = g_markup_printf_escaped ("Showing results for <b>%s</b>", corrected);
  label = sw_label_new (NULL, "sw-corrected");
  gtk_label_set_markup (GTK_LABEL (label), text);
  sw_app_list_set_header (list, label);
}

static void
on_search_again (GtkWidget *button, GtkWidget *page)
{
  sw_search_open (page, g_object_get_data (G_OBJECT (page), "sw-type"),
                  g_object_get_data (G_OBJECT (page), "sw-query"));
}

GtkWidget *
sw_search_results_new (const char *query, const char *type)
{
  GtkWidget *page = sw_page_new (query), *button, *list;
  g_autofree char *q = sw_api_escape (query);
  g_autofree char *path = type ? g_strdup_printf ("/search?q=%s&type=%s", q, type)
                               : g_strdup_printf ("/search?q=%s", q);
  g_autofree char *empty = NULL;

  g_object_set_data_full (G_OBJECT (page), "sw-query", g_strdup (query), g_free);
  g_object_set_data_full (G_OBJECT (page), "sw-type", g_strdup (type), g_free);
  button = vibeos_top_bar_add_action (sw_page_get_bar (page), "sw-search-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search", -1);
  g_signal_connect (button, "clicked", G_CALLBACK (on_search_again), page);
  list = sw_app_list_new (path);
  empty = g_strdup_printf ("Nothing in the store matches \u201c%s\u201d. Try other words.", query);
  sw_app_list_set_empty (list, empty);
  sw_app_list_set_meta_func (list, on_meta, NULL);
  sw_page_set_body (page, list);
  return page;
}

static void
search_for (Search *s, const char *query)
{
  g_autofree char *q = g_strstrip (g_strdup (query ? query : ""));
  GtkWidget *from = s->from;
  GtkWidget *top;

  if (!*q)
    return;
  /* at most 100 characters (the API's limit) */
  if (g_utf8_strlen (q, -1) > 100)
    {
      char *cut = g_utf8_substring (q, 0, 100);

      g_free (q);
      q = cut;
    }
  remember (s, q);
  top = sw_search_results_new (q, s->type);
  vibeos_search_view_close (VIBEOS_SEARCH_VIEW (s->view));
  /* a search from a results page replaces it: Back goes where the first search began */
  if (g_object_get_data (G_OBJECT (from), "sw-query"))
    sw_window_replace (from, top);
  else
    sw_window_push (from, top);
}

/* ---- suggestions ----------------------------------------------------------------------------------------------- */

static void
on_query_row (GtkWidget *row, Search *s)
{
  search_for (s, g_object_get_data (G_OBJECT (row), "sw-text"));
}

static void
on_fill (GtkWidget *button, Search *s)
{
  g_autofree char *text = g_strconcat (g_object_get_data (G_OBJECT (button), "sw-text"), " ", NULL);

  vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (s->view), text);
}

static GtkWidget *
query_row (Search *s, const char *text, const char *icon_name, gboolean fill)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16), *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = gtk_label_new (text), *row;

  gtk_widget_add_css_class (box, "sw-suggest-box");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (box), label);
  if (fill)
    {
      GtkWidget *button = sw_icon_button_new ("sw-north-west-symbolic", "Put it in the search field");

      g_object_set_data_full (G_OBJECT (button), "sw-text", g_strdup (text), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_fill), s);
      gtk_box_append (GTK_BOX (box), button);
    }
  row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "sw-suggest");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data_full (G_OBJECT (row), "sw-text", g_strdup (text), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_query_row), s);
  return row;
}

static void
on_app_row (GtkWidget *row, Search *s)
{
  JsonObject *app = g_object_get_data (G_OBJECT (row), "sw-app");
  GtkWidget *from = s->from;

  vibeos_search_view_close (VIBEOS_SEARCH_VIEW (s->view));
  sw_window_open_app (from, sw_json_string (app, "id"), app);
}

static GtkWidget *
app_row (Search *s, JsonObject *app)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16), *icon = sw_image_new (40, 40, 10);
  GtkWidget *label = gtk_label_new (sw_json_string (app, "name")), *row;

  gtk_widget_add_css_class (box, "sw-suggest-box");
  sw_image_set_fallback (SW_IMAGE (icon), "sw-package-symbolic");
  sw_image_set_url (SW_IMAGE (icon), sw_app_icon_url (app, 40));
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "sw-suggest-app");
  gtk_box_append (GTK_BOX (box), label);
  row = vibeos_pressable_new (box);
  gtk_widget_add_css_class (row, "sw-suggest");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  g_object_set_data_full (G_OBJECT (row), "sw-app", json_object_ref (app), (GDestroyNotify) json_object_unref);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, sw_json_string (app, "name"),
                                  -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_app_row), s);
  return row;
}

static void
on_clear_recent (GtkWidget *button, Search *s)
{
  if (s->settings)
    g_settings_reset (s->settings, "recent-searches");
  clear_box (s);
}

static void
show_recent (Search *s)
{
  g_auto (GStrv) recent = s->settings ? g_settings_get_strv (s->settings, "recent-searches") : NULL;

  clear_box (s);
  if (!recent || !recent[0])
    return;
  gtk_box_append (GTK_BOX (s->box), sw_section_header_new ("Recent searches", NULL, "Clear",
                                                           G_CALLBACK (on_clear_recent), s));
  for (guint i = 0; recent[i]; i++)
    gtk_box_append (GTK_BOX (s->box), query_row (s, recent[i], "sw-history-symbolic", TRUE));
}

static void
on_suggestions (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Search *s = data;
  JsonObject *object = result ? sw_json_node_object (result->data) : NULL;
  JsonArray *apps = sw_json_array (object, "apps"), *queries = sw_json_array (object, "queries");

  if (error)
    {
      g_autofree char *text = sw_api_error_describe (error);

      clear_box (s);
      gtk_box_append (GTK_BOX (s->box), sw_label_new (text, "sw-suggest-error"));
      return;
    }
  clear_box (s);
  for (guint i = 0; queries && i < json_array_get_length (queries); i++)
    {
      JsonNode *node = json_array_get_element (queries, i);

      if (JSON_NODE_HOLDS_VALUE (node) && json_node_get_string (node))
        gtk_box_append (GTK_BOX (s->box), query_row (s, json_node_get_string (node), "sw-search-symbolic", TRUE));
    }
  for (guint i = 0; apps && i < json_array_get_length (apps); i++)
    if (sw_json_array_object (apps, i))
      gtk_box_append (GTK_BOX (s->box), app_row (s, sw_json_array_object (apps, i)));
}

static gboolean
on_suggest_timeout (gpointer data)
{
  Search *s = data;
  g_autofree char *q = g_strstrip (g_strdup (vibeos_search_view_get_text (VIBEOS_SEARCH_VIEW (s->view))));
  g_autofree char *escaped = NULL, *path = NULL;

  s->timeout = 0;
  if (!*q)
    return G_SOURCE_REMOVE;
  escaped = sw_api_escape (q);
  path = g_strdup_printf ("/search/suggest?q=%s", escaped);
  sw_api_get (sw_api_get_default (), path, SW_API_CACHED, s->cancellable, on_suggestions, s);
  return G_SOURCE_REMOVE;
}

static void
on_changed (VibeosSearchView *view, const char *text, Search *s)
{
  g_autofree char *q = g_strstrip (g_strdup (text ? text : ""));

  g_clear_handle_id (&s->timeout, g_source_remove);
  if (s->cancellable)
    g_cancellable_cancel (s->cancellable);
  g_set_object (&s->cancellable, g_cancellable_new ());
  if (!*q)
    {
      show_recent (s);
      return;
    }
  s->timeout = g_timeout_add (SUGGEST_MS, on_suggest_timeout, s);
}

static void
on_activated (VibeosSearchView *view, const char *text, Search *s)
{
  search_for (s, text);
}

static void
on_closed (VibeosSearchView *view, Search *s)
{
  g_clear_handle_id (&s->timeout, g_source_remove);
  if (s->cancellable)
    g_cancellable_cancel (s->cancellable);
}

void
sw_search_open (GtkWidget *from, const char *type, const char *text)
{
  Search *s = g_new0 (Search, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *marker;
  static const char *const hints[] = { "Search apps and games", "Search apps", "Search games" };

  s->from = g_object_ref (from);
  s->type = g_strdup (type);
  s->settings = settings_new ();
  s->cancellable = g_cancellable_new ();
  s->view = vibeos_search_view_new (!g_strcmp0 (type, "app") ? hints[1] : !g_strcmp0 (type, "game") ? hints[2]
                                                                                                    : hints[0]);
  g_object_set_data_full (G_OBJECT (s->view), "sw-search", s, search_free);
  s->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (s->box, "sw-suggestions");
  gtk_box_append (GTK_BOX (content), sw_scroll_new (s->box));
  /* while the view shows, the shell's back gesture sends Escape (which closes it) instead of leaving the app */
  marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (marker), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search", -1);
  gtk_widget_set_can_target (marker, FALSE);
  gtk_box_append (GTK_BOX (content), marker);
  vibeos_search_view_set_content (VIBEOS_SEARCH_VIEW (s->view), content);
  g_signal_connect (s->view, "search-changed", G_CALLBACK (on_changed), s);
  g_signal_connect (s->view, "search-activated", G_CALLBACK (on_activated), s);
  g_signal_connect (s->view, "closed", G_CALLBACK (on_closed), s);
  /* beside the rail on a tablet, which stays */
  vibeos_search_view_open_in (VIBEOS_SEARCH_VIEW (s->view), sw_window_search_overlay (from));
  if (text && *text)
    vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (s->view), text);
  else
    show_recent (s);
}
