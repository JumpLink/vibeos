/* The Highlights tab (ct-tabs.h): the favourites, the birthdays coming up and the contacts opened lately. */
#include "ct-tabs.h"
#include "ct-avatar.h"
#include "ct-people.h"
#include "ct-picker.h"
#include "ct-popup.h"
#include "ct-rows.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#define RECENTS_KEPT 12         /* how many recently opened contacts are remembered */
#define RECENTS_SHOWN 5
#define BIRTHDAY_DAYS 30        /* how far ahead "For you" looks */
#define BIRTHDAYS_SHOWN 3

typedef struct {
  GtkWidget *root, *window;
  CtStore *store;
  GSettings *settings;
  GtkWidget *grid, *no_favourites;
  GtkWidget *for_you_header, *for_you;
  GtkWidget *recents_header, *recents;
} Highlights;

void
ct_recents_add (GSettings *settings, const char *uid)
{
  g_auto(GStrv) old = g_settings_get_strv (settings, "recent-contacts");
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) recent = NULL;
  guint n = 1;

  if (!uid || !*uid || (old[0] && g_str_equal (old[0], uid)))
    return;
  g_strv_builder_add (builder, uid);
  for (guint i = 0; old[i] && n < RECENTS_KEPT; i++)
    if (!g_str_equal (old[i], uid))
      {
        g_strv_builder_add (builder, old[i]);
        n++;
      }
  recent = g_strv_builder_end (builder);
  g_settings_set_strv (settings, "recent-contacts", (const char *const *) recent);
}

static const char *
name_of (Highlights *h, CtEntry *entry)
{
  const char *name = entry->name[ct_display_order (h->settings)];

  return name && *name ? name : _("(No name)");
}

static void
on_contact (GtkWidget *widget, Highlights *h)
{
  g_autofree char *uid = g_strdup (g_object_get_data (G_OBJECT (widget), "ct-uid"));

  ct_window_show_contact (h->window, uid);
}

/* a row of "For you" or Recents: the contact's picture, the name and a line under it */
static GtkWidget *
contact_row (Highlights *h, CtEntry *entry, const char *summary)
{
  const char *name = name_of (h, entry);
  GtkWidget *row = vibeos_list_row_new (name, summary);
  /* the list's own tile, letter and all, as a picture */
  g_autoptr(GdkPaintable) avatar = ct_avatar_paintable (h->root, entry->uid, name, entry->photo, CT_ROW_PHOTO_DP);
  GdkRGBA fill = ct_avatar_tile_color (entry->uid);

  vibeos_list_row_set_avatar (VIBEOS_LIST_ROW (row), avatar, NULL, &fill);
  g_object_set_data_full (G_OBJECT (row), "ct-uid", g_strdup (entry->uid), g_free);
  g_signal_connect (row, "clicked", G_CALLBACK (on_contact), h);
  return row;
}

/* ===== the favourites ========================================================================================== */

static GtkWidget *
favourite_cell (Highlights *h, CtEntry *entry)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *avatar = ct_avatar_view_new (CT_FAVOURITE_AVATAR_DP);
  GtkWidget *label = ct_text (name_of (h, entry), "ct-favourite-name");
  GtkWidget *cell;

  ct_avatar_view_set (avatar, entry->uid, name_of (h, entry), entry->photo);
  gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (avatar, "ct-favourite-avatar");
  gtk_box_append (GTK_BOX (box), avatar);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_label_set_yalign (GTK_LABEL (label), 0);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines (GTK_LABEL (label), 2);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (box, "ct-favourite-box");
  cell = vibeos_pressable_new (box);
  gtk_widget_add_css_class (cell, "ct-favourite");
  gtk_widget_set_size_request (cell, (int) CT_FAVOURITE_CELL_W_DP, -1);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (cell), VIBEOS_RIPPLE_BOUNDED, 16);
  gtk_accessible_update_property (GTK_ACCESSIBLE (cell), GTK_ACCESSIBLE_PROPERTY_LABEL, name_of (h, entry), -1);
  g_object_set_data_full (G_OBJECT (cell), "ct-uid", g_strdup (entry->uid), g_free);
  g_signal_connect (cell, "clicked", G_CALLBACK (on_contact), h);
  return cell;
}

static void
on_favourites_picked (GPtrArray *uids, gpointer data)
{
  Highlights *h = data;

  for (guint i = 0; i < uids->len; i++)
    ct_store_set_starred (h->store, uids->pdata[i], TRUE);
}

/* "Add": the picker, without those starred already */
static void
on_add_favourites (VibeosPressable *button, Highlights *h)
{
  g_autoptr(GPtrArray) entries = ct_shown_entries (h->store, h->settings);
  g_autoptr(GPtrArray) starred = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < entries->len; i++)
    if (((CtEntry *) entries->pdata[i])->starred)
      g_ptr_array_add (starred, g_strdup (((CtEntry *) entries->pdata[i])->uid));
  ct_window_push (h->window, ct_picker_new (h->window, _("Add favorites"), starred, TRUE, on_favourites_picked, h));
}

/* ===== the birthdays ============================================================================================ */

typedef struct {
  CtEntry *entry;
  int days;
  char *date;
} Birthday;

static int
compare_birthdays (gconstpointer a, gconstpointer b)
{
  const Birthday *x = a, *y = b;

  return x->days - y->days;
}

/* the days from today to the next @month/@day (0: today); a 29 February counts as 1 March in other years */
static int
days_until (GDateTime *today, int month, int day)
{
  int year = g_date_time_get_year (today);

  for (int attempt = 0; attempt < 2; attempt++, year++)
    {
      int m = month, d = day;
      g_autoptr(GDateTime) date = NULL;

      if (m == 2 && d == 29 && !g_date_valid_dmy (29, 2, year))
        m = 3, d = 1;
      date = g_date_time_new_local (year, m, d, 0, 0, 0);
      if (date && g_date_time_compare (date, today) >= 0)
        return (int) (g_date_time_difference (date, today) / G_TIME_SPAN_DAY);
    }
  return G_MAXINT;
}

static GArray *
upcoming_birthdays (Highlights *h, GPtrArray *entries)
{
  GArray *found = g_array_new (FALSE, FALSE, sizeof (Birthday));
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) today = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                                      g_date_time_get_day_of_month (now), 0, 0, 0);
  int birthday = ct_kind_default_type (CT_KIND_EVENT);

  for (guint i = 0; i < entries->len; i++)
    {
      CtEntry *entry = entries->pdata[i];
      g_autoptr(GPtrArray) events = ct_contact_get_items (entry->contact, CT_KIND_EVENT);

      for (guint j = 0; j < events->len; j++)
        {
          CtItem *item = events->pdata[j];
          int year, month, day, days;

          if (item->type != birthday || !ct_date_parse (item->value, &year, &month, &day))
            continue;
          days = days_until (today, month, day);
          if (days <= BIRTHDAY_DAYS)
            {
              Birthday b = { entry, days, ct_date_format_display (item->value) };

              g_array_append_val (found, b);
            }
          break;
        }
    }
  g_array_sort (found, compare_birthdays);
  return found;
}

/* ===== Recents ================================================================================================= */

static void
on_recents_menu_chosen (int index, gpointer data)
{
  Highlights *h = data;

  if (index == 0)
    g_settings_set_strv (h->settings, "recent-contacts", NULL);
}

static void
on_recents_more (VibeosPressable *button, Highlights *h)
{
  const char *labels[] = { _("Clear recents") };

  ct_popup_show (GTK_WIDGET (button), labels, 1, CT_POPUP_OVER_END, 0, 0, 0, on_recents_menu_chosen, h);
}

/* the line under a recent contact's name: the default number, else the first, else the first email address */
static char *
recent_summary (CtEntry *entry)
{
  for (CtKind kind = CT_KIND_PHONE; kind <= CT_KIND_EMAIL; kind++)
    {
      g_autoptr(GPtrArray) items = NULL;
      CtItem *chosen = NULL;

      if (kind == CT_KIND_SIP)
        continue;
      items = ct_contact_get_items (entry->contact, kind);
      for (guint i = 0; i < items->len; i++)
        if (!chosen || ((CtItem *) items->pdata[i])->primary)
          chosen = chosen && chosen->primary ? chosen : items->pdata[i];
      if (chosen)
        return g_strdup (chosen->value);
    }
  return NULL;
}

/* ===== the tab ================================================================================================= */

static GtkWidget *
section_header (const char *title, GtkWidget *end)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = ct_text (title, "ct-highlights-title");

  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (row), label);
  if (end)
    gtk_box_append (GTK_BOX (row), end);
  gtk_widget_add_css_class (row, "ct-highlights-header");
  return row;
}

static void
refresh (Highlights *h)
{
  g_autoptr(GPtrArray) entries = ct_shown_entries (h->store, h->settings);
  g_auto(GStrv) recent = g_settings_get_strv (h->settings, "recent-contacts");
  g_autoptr(GArray) birthdays = NULL;
  GtkWidget *child;
  guint favourites = 0, recents = 0;

  /* the favourites, by the sort order as the list has them */
  while ((child = gtk_widget_get_first_child (h->grid)))
    gtk_flow_box_remove (GTK_FLOW_BOX (h->grid), child);
  {
    g_autoptr(CtRows) rows = ct_rows_new (entries, ct_display_order (h->settings), ct_sort_order (h->settings), NULL,
                                          FALSE);

    for (guint i = 0; i < rows->rows->len; i++)
      {
        CtEntry *entry = g_array_index (rows->rows, CtRow, i).entry;

        if (!entry->starred)
          continue;
        gtk_flow_box_append (GTK_FLOW_BOX (h->grid), favourite_cell (h, entry));
        favourites++;
      }
  }
  gtk_widget_set_visible (h->grid, favourites > 0);
  gtk_widget_set_visible (h->no_favourites, favourites == 0);

  /* For you: the birthdays of the next month */
  birthdays = upcoming_birthdays (h, entries);
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (h->for_you));
  for (guint i = 0; i < birthdays->len && i < BIRTHDAYS_SHOWN; i++)
    {
      Birthday *b = &g_array_index (birthdays, Birthday, i);
      g_autofree char *summary = b->days == 0 ? g_strdup (_("Birthday today"))
                               : b->days == 1 ? g_strdup (_("Birthday tomorrow"))
                                              : g_strdup_printf (_("Birthday on %s"), b->date);
      GtkWidget *row = contact_row (h, b->entry, summary);

      vibeos_card_list_append (VIBEOS_CARD_LIST (h->for_you), row);
    }
  for (guint i = 0; i < birthdays->len; i++)
    g_free (g_array_index (birthdays, Birthday, i).date);
  gtk_widget_set_visible (h->for_you_header, birthdays->len > 0);
  gtk_widget_set_visible (h->for_you, birthdays->len > 0);

  /* Recents */
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (h->recents));
  for (guint i = 0; recent[i] && recents < RECENTS_SHOWN; i++)
    {
      CtEntry *entry = ct_store_lookup (h->store, recent[i]);
      g_autofree char *summary = NULL;

      if (!entry)
        continue;
      summary = recent_summary (entry);
      vibeos_card_list_append (VIBEOS_CARD_LIST (h->recents), contact_row (h, entry, summary));
      recents++;
    }
  gtk_widget_set_visible (h->recents_header, recents > 0);
  gtk_widget_set_visible (h->recents, recents > 0);
}

static void
on_store_changed (CtStore *store, Highlights *h)
{
  refresh (h);
}

static void
on_settings_changed (GSettings *settings, const char *key, Highlights *h)
{
  refresh (h);
}

static void
on_theme_changed (VibeosTheme *theme, Highlights *h)
{
  refresh (h);
}

static void
highlights_free (gpointer data)
{
  Highlights *h = data;

  g_signal_handlers_disconnect_by_data (h->store, h);
  g_signal_handlers_disconnect_by_data (h->settings, h);
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), h);
  g_object_unref (h->store);
  g_object_unref (h->settings);
  g_free (h);
}

GtkWidget *
ct_highlights_new (GtkWidget *window)
{
  Highlights *h = g_new0 (Highlights, 1);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *add = vibeos_text_button_new (_("Add"));
  GtkWidget *more = ct_icon_button ("ct-more-vert-symbolic", _("More options"), 48, 48);

  h->root = root;
  h->window = window;
  h->store = g_object_ref (ct_window_get_store (window));
  h->settings = g_object_ref (ct_window_get_settings (window));
  g_object_set_data_full (G_OBJECT (root), "ct-highlights", h, highlights_free);
  gtk_widget_add_css_class (root, "ct-highlights");

  gtk_box_append (GTK_BOX (root), ct_people_new_pill (ct_window_get_people (window)));

  g_signal_connect (add, "clicked", G_CALLBACK (on_add_favourites), h);
  GtkWidget *favourites_header = section_header (_("Favorites"), add);
  gtk_widget_add_css_class (favourites_header, "first");
  gtk_box_append (GTK_BOX (column), favourites_header);
  h->grid = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (h->grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (h->grid), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (h->grid), CT_FAVOURITE_COLUMNS);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (h->grid), CT_FAVOURITE_COLUMNS_MAX);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (h->grid), FALSE);
  gtk_widget_add_css_class (h->grid, "ct-favourites");
  gtk_box_append (GTK_BOX (column), h->grid);
  h->no_favourites = ct_text (_("Add favorites to see them here"), "ct-no-favourites");
  gtk_box_append (GTK_BOX (column), h->no_favourites);

  h->for_you_header = section_header (_("For you"), NULL);
  gtk_box_append (GTK_BOX (column), h->for_you_header);
  h->for_you = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (h->for_you), 0, 0);
  gtk_box_append (GTK_BOX (column), h->for_you);

  g_signal_connect (more, "clicked", G_CALLBACK (on_recents_more), h);
  h->recents_header = section_header (_("Recents"), more);
  gtk_box_append (GTK_BOX (column), h->recents_header);
  h->recents = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (h->recents), 0, (int) CT_TAB_PAD_BOTTOM_DP);
  gtk_box_append (GTK_BOX (column), h->recents);

  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), column);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (root), scroll);

  g_signal_connect (h->store, "changed", G_CALLBACK (on_store_changed), h);
  g_signal_connect (h->store, "ready", G_CALLBACK (on_store_changed), h);
  g_signal_connect (h->settings, "changed", G_CALLBACK (on_settings_changed), h);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), h);
  refresh (h);
  return root;
}
