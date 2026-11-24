/* The Featured, Apps and Games tabs.
 *
 * Each is one request for its whole first screen (GET /featured, /browse/apps, /browse/games): the kept copy shows
 * at once, and the server is asked with its ETag (sw-api.h), so a start with nothing changed costs one 304. The
 * sections come in the store team's order:
 *
 *   banners    a carousel of pictures; a tap opens the app, collection or category it names
 *   apps       a titled section with "See all" when there are more (the collection's full list). The first apps
 *              section of a tab is a carousel of columns of three rows (each row with its Install button); the ones
 *              after it are rows of tiles, so the screen does not repeat one shape all the way down.
 *
 * Apps and Games add a Categories row over the sections, which opens the list of their categories (those with apps
 * in them), and a second sub-tab, Popular: GET /apps sorted by installs, page after page. A tab loads when it first
 * shows, so starting costs only Featured's request. */
#include "sw-pages.h"
#include "sw-image.h"
#include "sw-json.h"
#include "sw-widgets.h"
#include "sw-window.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define COLUMN_W 296
#define COLUMN_PEEK 72   /* a wide strip: the next column's icon and the gap after it, peeking in at the end */
#define STRIP_START 24   /* the strip's padding at its start (software.css, .sw-strip) */
#define TILE_W   76
#define BANNER_W 312

typedef struct {
  SwHomeKind kind;
  GtkWidget *home, *tabs, *pages, *status, *scroll, *column, *popular;
  char      *shown;      /* the data last drawn, to skip drawing the same again */
  gboolean   loaded;
} Home;

static void load (Home *home);

static const char *
home_path (Home *home)
{
  return home->kind == SW_HOME_APPS ? "/browse/apps" : home->kind == SW_HOME_GAMES ? "/browse/games" : "/featured";
}

/* ---- sections -------------------------------------------------------------------------------------------------- */

static void
on_see_all (GtkWidget *button, gpointer data)
{
  JsonObject *section = g_object_get_data (G_OBJECT (button), "sw-section");
  g_autofree char *id = sw_api_escape (sw_json_string (section, "id"));
  g_autofree char *path = g_strdup_printf ("/collections/%s/apps", id);

  sw_window_open_list (button, sw_json_string (section, "title"), path);
}

/* the title as this device says it, once the header is in the window */
static void
on_title_map (GtkWidget *label, gpointer data)
{
  g_autofree char *title = sw_device_title (label, g_object_get_data (G_OBJECT (label), "sw-title"));

  gtk_label_set_text (GTK_LABEL (label), title);
}

static GtkWidget *
section_header (JsonObject *section)
{
  gboolean more = sw_json_bool (section, "has_more", FALSE);
  GtkWidget *header = sw_section_header_new (sw_json_string (section, "title"), NULL, more ? _("See all") : NULL,
                                             G_CALLBACK (on_see_all), NULL);
  /* the title is the header's first child */
  GtkWidget *title = gtk_widget_get_first_child (header);

  g_object_set_data_full (G_OBJECT (title), "sw-title", g_strdup (sw_json_string (section, "title")), g_free);
  g_signal_connect (title, "map", G_CALLBACK (on_title_map), NULL);

  if (more)
    {
      /* the button is the header's last child */
      GtkWidget *button = gtk_widget_get_last_child (header);

      g_object_set_data_full (G_OBJECT (button), "sw-section", json_object_ref (section),
                              (GDestroyNotify) json_object_unref);
    }
  return header;
}

/* The columns' width for a strip `view` wide. A phone's are COLUMN_W, the second peeking in. In a wider strip (a
 * tablet's) as many whole columns as there is room for share it, the next one's icon peeking in after them:
 * COLUMN_W wide, the strip cut its fourth column through its Install button. */
static int
column_width (int view)
{
  int n = (view - STRIP_START - COLUMN_PEEK) / (COLUMN_W + SW_STRIP_SPACING);

  return n < 2 ? COLUMN_W : (view - STRIP_START - COLUMN_PEEK) / n - SW_STRIP_SPACING;
}

static gboolean
fit_columns (gpointer data)
{
  GtkWidget *strip = data;
  GtkAdjustment *adjustment = spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (strip));
  GtkWidget *box = spongy_scroll_view_get_child (SPONGY_SCROLL_VIEW (strip));
  int width = column_width ((int) gtk_adjustment_get_page_size (adjustment));

  g_object_set_data (G_OBJECT (strip), "sw-fit", NULL);
  /* a fling ends with a whole column at the left */
  sw_strip_snap (strip, width);
  for (GtkWidget *column = gtk_widget_get_first_child (box); column; column = gtk_widget_get_next_sibling (column))
    {
      int now;

      gtk_widget_get_size_request (column, &now, NULL);
      if (now != width)
        gtk_widget_set_size_request (column, width, -1);
    }
  return G_SOURCE_REMOVE;
}

/* the strip's width changed (it is told while it is being laid out): the columns follow it once that is over */
static void
on_strip_width (GtkAdjustment *adjustment, GParamSpec *pspec, GtkWidget *strip)
{
  if (g_object_get_data (G_OBJECT (strip), "sw-fit"))
    return;
  g_object_set_data_full (G_OBJECT (strip), "sw-fit",
                          GUINT_TO_POINTER (g_idle_add_full (G_PRIORITY_HIGH_IDLE, fit_columns, g_object_ref (strip),
                                                             g_object_unref)),
                          NULL);
}

static GtkWidget *
apps_columns (JsonArray *items)
{
  GtkWidget *box, *strip = sw_strip_new (&box), *column = NULL;

  gtk_widget_add_css_class (box, "sw-columns");
  /* a fling ends with a whole column at the left */
  sw_strip_snap (strip, COLUMN_W);
  g_signal_connect_object (spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (strip)), "notify::page-size",
                           G_CALLBACK (on_strip_width), strip, 0);
  for (guint i = 0; i < json_array_get_length (items); i++)
    {
      JsonObject *app = sw_json_array_object (items, i);
      GtkWidget *row;

      if (!app)
        continue;
      if (i % 3 == 0)
        {
          column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
          gtk_widget_set_size_request (column, COLUMN_W, -1);
          gtk_widget_set_valign (column, GTK_ALIGN_START);
          gtk_box_append (GTK_BOX (box), column);
        }
      row = sw_app_row_new (app, NULL);
      gtk_widget_add_css_class (row, "in-column");
      gtk_box_append (GTK_BOX (column), row);
    }
  return strip;
}

static GtkWidget *
apps_tiles (JsonArray *items)
{
  GtkWidget *box, *strip = sw_strip_new (&box);

  for (guint i = 0; i < json_array_get_length (items); i++)
    {
      JsonObject *app = sw_json_array_object (items, i);

      if (app)
        gtk_box_append (GTK_BOX (box), sw_app_tile_new (app, TILE_W));
    }
  return strip;
}

static void
on_banner_clicked (GtkWidget *card, gpointer data)
{
  JsonObject *banner = g_object_get_data (G_OBJECT (card), "sw-banner");

  sw_window_open_target (card, sw_json_object (banner, "target"), sw_json_string (banner, "title"));
}

static GtkWidget *
banners (JsonArray *items)
{
  GtkWidget *box, *strip = sw_strip_new (&box);

  gtk_widget_add_css_class (box, "sw-banners");
  sw_strip_snap (strip, BANNER_W);
  for (guint i = 0; i < json_array_get_length (items); i++)
    {
      JsonObject *banner = sw_json_array_object (items, i), *image = sw_json_object (banner, "image");
      GtkWidget *overlay = gtk_overlay_new (), *picture, *text, *title, *card;
      double w = sw_json_double (image, "width"), h = sw_json_double (image, "height");
      const char *subtitle = sw_json_string (banner, "subtitle");

      if (!banner)
        continue;
      /* the picture's own shape, 2:1 when it does not say */
      picture = sw_image_new (BANNER_W, (int) (BANNER_W / (w > 0 && h > 0 ? w / h : 2.0)), 20);
      sw_image_set_url (SW_IMAGE (picture), sw_json_string (image, "url"));
      gtk_overlay_set_child (GTK_OVERLAY (overlay), picture);
      text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_add_css_class (text, "sw-banner-text");
      gtk_widget_set_valign (text, GTK_ALIGN_END);
      title = gtk_label_new (sw_json_string (banner, "title"));
      gtk_label_set_xalign (GTK_LABEL (title), 0);
      gtk_label_set_wrap (GTK_LABEL (title), TRUE);
      gtk_widget_add_css_class (title, "sw-banner-title");
      gtk_box_append (GTK_BOX (text), title);
      if (subtitle)
        {
          GtkWidget *line = gtk_label_new (subtitle);

          gtk_label_set_xalign (GTK_LABEL (line), 0);
          gtk_label_set_ellipsize (GTK_LABEL (line), PANGO_ELLIPSIZE_END);
          gtk_widget_add_css_class (line, "sw-banner-subtitle");
          gtk_box_append (GTK_BOX (text), line);
        }
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), text);
      card = vibeos_pressable_new (overlay);
      gtk_widget_add_css_class (card, "sw-banner");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (card), VIBEOS_RIPPLE_BOUNDED, 20);
      g_object_set_data_full (G_OBJECT (card), "sw-banner", json_object_ref (banner),
                              (GDestroyNotify) json_object_unref);
      gtk_accessible_update_property (GTK_ACCESSIBLE (card), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      sw_json_string (banner, "title"), -1);
      g_signal_connect (card, "clicked", G_CALLBACK (on_banner_clicked), NULL);
      gtk_box_append (GTK_BOX (box), card);
    }
  return strip;
}

static void
on_categories (VibeosListRow *row, Home *home)
{
  sw_window_push (GTK_WIDGET (row), sw_categories_page_new (home->kind == SW_HOME_GAMES ? "games" : "apps"));
}

/* The way into the tab's categories: one row that opens their list, saying how many there are. Only categories with
 * apps in them count (an older server lists empty ones too); with none, there is no row. */
static GtkWidget *
categories_row (Home *home, JsonArray *list)
{
  GdkRGBA fill = vibeos_color (VIBEOS_SECONDARY_CONTAINER), glyph = vibeos_color (VIBEOS_ON_SECONDARY_CONTAINER);
  g_autofree char *summary = NULL;
  GtkWidget *cards, *row;
  guint n = 0;

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    if (sw_json_int (sw_json_array_object (list, i), "app_count", 0) > 0)
      n++;
  if (!n)
    return NULL;
  summary = g_strdup_printf (g_dngettext (NULL, "%u category", "%u categories", n), n);
  row = vibeos_list_row_new (_("Categories"), summary);
  vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (row), "sw-fa-shapes-symbolic", &fill, &glyph);
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_signal_connect (row, "clicked", G_CALLBACK (on_categories), home);
  cards = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 16, 0);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  return cards;
}

static void
show (Home *home, JsonObject *data)
{
  JsonArray *sections = sw_json_array (data, "sections");
  GtkWidget *child, *categories = categories_row (home, sw_json_array (data, "categories"));
  int apps_sections = 0;

  while ((child = gtk_widget_get_first_child (home->column)))
    gtk_box_remove (GTK_BOX (home->column), child);
  if (categories)
    gtk_box_append (GTK_BOX (home->column), categories);
  for (guint i = 0; sections && i < json_array_get_length (sections); i++)
    {
      JsonObject *section = sw_json_array_object (sections, i);
      const char *type = sw_json_string (section, "type");
      JsonArray *items = sw_json_array (section, "items");

      if (!items || !json_array_get_length (items))
        continue;
      if (!g_strcmp0 (type, "banners"))
        {
          if (sw_json_string (section, "title"))
            gtk_box_append (GTK_BOX (home->column), section_header (section));
          gtk_box_append (GTK_BOX (home->column), banners (items));
        }
      else if (!g_strcmp0 (type, "apps"))
        {
          gtk_box_append (GTK_BOX (home->column), section_header (section));
          gtk_box_append (GTK_BOX (home->column), apps_sections++ == 0 ? apps_columns (items) : apps_tiles (items));
        }
      /* a section type this version does not know: left out */
    }
  if (!gtk_widget_get_first_child (home->column))
    {
      sw_status_empty (SW_STATUS (home->status), "sw-apps-symbolic", _("Nothing here yet. Come back later."),
                       _("Retry"), (SwRetryFunc) load, home);
      return;
    }
  sw_status_show (SW_STATUS (home->status));
}

static void
on_loaded (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Home *home = data;
  g_autofree char *text = NULL;

  if (error)
    {
      /* what is shown stays (an old copy); only an empty tab shows the error */
      if (!sw_status_is_showing (SW_STATUS (home->status)))
        sw_status_error (SW_STATUS (home->status), error, (SwRetryFunc) load, home);
      return;
    }
  if (result->not_modified && home->shown)
    return;
  text = result->data ? json_to_string (result->data, FALSE) : g_strdup ("");
  if (!g_strcmp0 (text, home->shown))
    return;
  g_free (home->shown);
  home->shown = g_steal_pointer (&text);
  show (home, sw_json_node_object (result->data));
}

static void
load (Home *home)
{
  home->loaded = TRUE;
  if (!sw_status_is_showing (SW_STATUS (home->status)))
    sw_status_loading (SW_STATUS (home->status));
  sw_api_get (sw_api_get_default (), home_path (home), SW_API_CACHED | SW_API_STALE_FIRST,
              sw_cancellable_for (home->home), on_loaded, home);
}

/* every time the tab shows: a kept copy younger than a minute answers without asking */
static void
on_map (GtkWidget *widget, Home *home)
{
  load (home);
}

/* other apps in the lists now (VibeOS apps only, or all): what shows goes, and the tab asks again, at once if it
 * shows, else when it next does */
static void
on_catalog_changed (SwApi *api, GtkWidget *widget)
{
  Home *home = g_object_get_data (G_OBJECT (widget), "sw-home");

  sw_cancellable_renew (home->home);
  g_clear_pointer (&home->shown, g_free);
  sw_status_loading (SW_STATUS (home->status));
  sw_scroll_to_top (home->scroll);
  if (gtk_widget_get_mapped (home->home))
    load (home);
}

static void
on_search (VibeosSearchBar *bar, Home *home)
{
  sw_search_open (GTK_WIDGET (bar), home->kind == SW_HOME_APPS ? "app" : home->kind == SW_HOME_GAMES ? "game" : NULL,
                  NULL);
}

static void
on_subtab (SwTabs *tabs, int index, Home *home)
{
  if (index == 1 && !home->popular)
    {
      home->popular = sw_app_list_new (home->kind == SW_HOME_GAMES ? "/apps?type=game&sort=popular"
                                                                   : "/apps?type=app&sort=popular");
      gtk_stack_add_named (GTK_STACK (home->pages), home->popular, "popular");
    }
  gtk_stack_set_visible_child_name (GTK_STACK (home->pages), index == 1 ? "popular" : "recommended");
}

static void
home_free (gpointer data)
{
  Home *home = data;

  g_free (home->shown);
  g_free (home);
}

GtkWidget *
sw_home_new (SwHomeKind kind)
{
  Home *home = g_new0 (Home, 1);
  GtkWidget *bar;
  static const char *const hints[] = { N_("Search apps and games"), N_("Search apps"), N_("Search games") };

  home->kind = kind;
  home->home = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (home->home, "sw-home");
  g_object_set_data_full (G_OBJECT (home->home), "sw-home", home, home_free);

  bar = vibeos_search_bar_new (_(hints[kind]));
  gtk_widget_add_css_class (bar, "sw-search-bar");
  g_signal_connect (bar, "activated", G_CALLBACK (on_search), home);
  gtk_box_append (GTK_BOX (home->home), bar);

  home->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (home->column, "sw-home-column");
  home->scroll = sw_scroll_new (home->column);
  home->status = sw_status_new (home->scroll);
  home->pages = gtk_stack_new ();
  gtk_widget_set_vexpand (home->pages, TRUE);
  gtk_stack_add_named (GTK_STACK (home->pages), home->status, "recommended");
  if (kind != SW_HOME_FEATURED)
    {
      home->tabs = sw_tabs_new (_("Recommended"), _("Popular"), NULL);
      g_signal_connect (home->tabs, "changed", G_CALLBACK (on_subtab), home);
      gtk_box_append (GTK_BOX (home->home), home->tabs);
      g_object_set_data (G_OBJECT (home->home), "sw-tabs", home->tabs);
    }
  gtk_box_append (GTK_BOX (home->home), home->pages);
  g_signal_connect (home->home, "map", G_CALLBACK (on_map), home);
  g_signal_connect_object (sw_api_get_default (), "catalog-changed", G_CALLBACK (on_catalog_changed), home->home, 0);
  return home->home;
}

gboolean
sw_home_reset (GtkWidget *widget)
{
  Home *home = g_object_get_data (G_OBJECT (widget), "sw-home");

  if (!home)
    return FALSE;
  if (home->tabs && sw_tabs_get_selected (SW_TABS (home->tabs)) != 0)
    {
      sw_tabs_set_selected (SW_TABS (home->tabs), 0);
      on_subtab (SW_TABS (home->tabs), 0, home);
      return TRUE;
    }
  sw_scroll_to_top (home->scroll);
  return TRUE;
}
