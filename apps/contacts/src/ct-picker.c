/* The contact picker (ct-picker.h).
 *
 * The toolbar is the back arrow, "Select", search, and the ⋮ with "Select contacts" when several may be picked;
 * the search box and the selection bar (✕, the count, ADD) come over it as on the list. A tap on a row picks that
 * one contact; in selection mode it checks the row, and ADD picks the checked ones. Back leaves selection, then
 * search, then the picker. */
#include "ct-picker.h"
#include "ct-list.h"
#include "ct-page.h"
#include "ct-popup.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <neo.h>

typedef struct {
  GtkWidget *page, *window;
  CtStore *store;
  GSettings *settings;
  GHashTable *exclude;          /* uids not offered */
  gboolean require;             /* only the contacts with a row of need */
  CtKind need;
  gboolean multiple;
  CtPicked picked;
  gpointer data;

  GtkWidget *bar, *search_bar, *search_entry, *search_clear, *selection_bar, *selection_count, *add;
  GtkWidget *search_action, *overflow, *list, *empty, *create_row;
  CtPickerCreate create;
  gpointer create_data;
  gboolean searching, selecting;
  GHashTable *selected;         /* uid → uid */
} Picker;

static void
picker_free (gpointer data)
{
  Picker *picker = data;

  g_signal_handlers_disconnect_by_data (picker->store, picker);
  g_hash_table_unref (picker->exclude);
  g_hash_table_unref (picker->selected);
  g_object_unref (picker->store);
  g_object_unref (picker->settings);
  g_free (picker);
}

/* whether @entry is one the picker leaves out */
static gboolean
left_out (Picker *picker, CtEntry *entry)
{
  g_autoptr(GPtrArray) items = NULL;

  if (g_hash_table_contains (picker->exclude, entry->uid))
    return TRUE;
  if (!picker->require)
    return FALSE;
  items = ct_contact_get_items (entry->contact, picker->need);
  return items->len == 0;
}

/* the contacts shown (Settings' "Contacts to display") less the ones left out, by the search */
static void
refresh (Picker *picker, gboolean to_top)
{
  g_autoptr(GPtrArray) entries = ct_shown_entries (picker->store, picker->settings);
  const char *query = picker->searching ? gtk_editable_get_text (GTK_EDITABLE (picker->search_entry)) : NULL;
  CtRows *rows;

  for (guint i = entries->len; i > 0; i--)
    if (left_out (picker, entries->pdata[i - 1]))
      g_ptr_array_remove_index_fast (entries, i - 1);
  rows = ct_rows_new (entries, ct_display_order (picker->settings), ct_sort_order (picker->settings), query, FALSE);
  ct_list_set_search (CT_LIST (picker->list), query && *query);
  ct_list_set_rows (CT_LIST (picker->list), rows, to_top);
  gtk_widget_set_visible (picker->empty, rows->rows->len == 0);
  /* "Select contacts" only when there is someone to select */
  gtk_widget_set_visible (picker->overflow, picker->multiple && entries->len > 0 && !picker->searching
                                            && !picker->selecting);
  gtk_widget_set_visible (picker->search_action, !picker->searching && !picker->selecting);
  if (picker->create_row)
    gtk_widget_set_visible (picker->create_row, !picker->searching);
}

static void
on_store_changed (CtStore *store, Picker *picker)
{
  refresh (picker, FALSE);
}

static void
update_selection_bar (Picker *picker)
{
  guint count = g_hash_table_size (picker->selected);
  g_autofree char *text = count ? g_strdup_printf ("%u", count) : g_strdup (_("Select contacts"));

  gtk_label_set_label (GTK_LABEL (picker->selection_count), text);
  gtk_widget_set_visible (picker->add, count > 0);
}

static void
show_bars (Picker *picker)
{
  gtk_widget_set_visible (picker->search_bar, picker->searching && !picker->selecting);
  gtk_widget_set_visible (picker->selection_bar, picker->selecting);
}

static void
start_search (Picker *picker)
{
  picker->searching = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (picker->search_entry), "");
  show_bars (picker);
  refresh (picker, TRUE);
  gtk_widget_grab_focus (picker->search_entry);
}

static void
stop_search (Picker *picker)
{
  picker->searching = FALSE;
  show_bars (picker);
  refresh (picker, TRUE);
}

static void
start_selection (Picker *picker)
{
  picker->selecting = TRUE;
  g_hash_table_remove_all (picker->selected);
  ct_list_set_selection (CT_LIST (picker->list), picker->selected);
  update_selection_bar (picker);
  show_bars (picker);
  refresh (picker, FALSE);
}

static void
stop_selection (Picker *picker)
{
  picker->selecting = FALSE;
  g_hash_table_remove_all (picker->selected);
  ct_list_set_selection (CT_LIST (picker->list), NULL);
  show_bars (picker);
  refresh (picker, FALSE);
}

/* the picker goes first, then the caller hears of the pick (it may push a page of its own) */
static void
finish (Picker *picker, GPtrArray *uids)
{
  GtkWidget *window = picker->window;
  CtPicked picked = picker->picked;
  gpointer data = picker->data;

  GtkWidget *page = g_object_ref (picker->page);

  /* the page (and what the caller hung on it) lives until the caller has heard */
  g_ptr_array_ref (uids);
  ct_window_pop (window, page);
  picked (uids, data);
  g_ptr_array_unref (uids);
  g_object_unref (page);
}

static void
on_row_activated (CtList *list, guint index, Picker *picker)
{
  CtEntry *entry = ct_list_get_entry (list, index);
  g_autoptr(GPtrArray) uids = NULL;

  if (!entry)
    return;
  if (picker->selecting)
    {
      if (!g_hash_table_remove (picker->selected, entry->uid))
        g_hash_table_add (picker->selected, g_strdup (entry->uid));
      ct_list_update_selection (list);
      update_selection_bar (picker);
      return;
    }
  uids = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (uids, g_strdup (entry->uid));
  finish (picker, uids);
}

static void
on_add (NeoPressable *button, Picker *picker)
{
  g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  const char *uid;

  g_hash_table_iter_init (&iter, picker->selected);
  while (g_hash_table_iter_next (&iter, (gpointer *) &uid, NULL))
    g_ptr_array_add (uids, g_strdup (uid));
  finish (picker, uids);
}

static void
on_menu_chosen (int index, gpointer data)
{
  start_selection (data);
}

static void
on_overflow (NeoPressable *button, Picker *picker)
{
  const char *labels[] = { _("Select contacts") };

  ct_popup_show (GTK_WIDGET (button), labels, 1, CT_POPUP_OVER_END, 0, 0, 0, on_menu_chosen, picker);
}

static void
on_search_action (NeoPressable *button, Picker *picker)
{
  start_search (picker);
}

static void
on_search_back (NeoPressable *button, Picker *picker)
{
  stop_search (picker);
}

static void
on_search_changed (GtkEditable *entry, Picker *picker)
{
  gtk_widget_set_visible (picker->search_clear, *gtk_editable_get_text (entry) != '\0');
  refresh (picker, TRUE);
}

static void
on_search_clear (NeoPressable *button, Picker *picker)
{
  gtk_editable_set_text (GTK_EDITABLE (picker->search_entry), "");
}

static void
on_selection_close (NeoPressable *button, Picker *picker)
{
  stop_selection (picker);
}

static gboolean
on_back (CtPage *page, gpointer data)
{
  Picker *picker = data;

  if (picker->selecting)
    stop_selection (picker);
  else if (picker->searching)
    stop_search (picker);
  else
    return FALSE;
  return TRUE;
}

static GtkWidget *
make_toolbar (Picker *picker, const char *title)
{
  GtkWidget *frame = gtk_overlay_new ();
  GtkWidget *actions, *back, *end;

  gtk_widget_add_css_class (frame, "ct-people-toolbar");
  picker->bar = ct_page_toolbar (CT_PAGE (picker->page), NULL, "ct-arrow-back-symbolic", _("Back"), title, &actions,
                                 NULL);
  picker->search_action = ct_icon_button ("ct-search-symbolic", _("Search contacts"), 56, 48, CT_RIPPLE_APPCOMPAT);
  picker->overflow = ct_icon_button ("ct-more-vert-symbolic", _("More options"), 40, 48, CT_RIPPLE_APPCOMPAT);
  g_signal_connect (picker->search_action, "clicked", G_CALLBACK (on_search_action), picker);
  g_signal_connect (picker->overflow, "clicked", G_CALLBACK (on_overflow), picker);
  for (GtkWidget *w = picker->search_action; w; w = w == picker->search_action ? picker->overflow : NULL)
    {
      gtk_widget_set_valign (w, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (w, "ct-toolbar-action");
      gtk_box_append (GTK_BOX (actions), w);
    }
  gtk_overlay_set_child (GTK_OVERLAY (frame), picker->bar);

  /* the search box, as the list's */
  picker->search_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (picker->search_bar, "ct-search-bar");
  back = ct_icon_button ("ct-arrow-back-symbolic", _("stop searching"), CT_SEARCH_ICON_DP, CT_TOOLBAR_H_DP,
                         CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (back, "ct-search-back");
  g_signal_connect (back, "clicked", G_CALLBACK (on_search_back), picker);
  gtk_box_append (GTK_BOX (picker->search_bar), back);
  picker->search_entry = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (picker->search_entry), _("Search contacts"));
  gtk_widget_add_css_class (picker->search_entry, "ct-search-entry");
  gtk_widget_set_hexpand (picker->search_entry, TRUE);
  gtk_widget_set_valign (picker->search_entry, GTK_ALIGN_CENTER);
  g_signal_connect (picker->search_entry, "changed", G_CALLBACK (on_search_changed), picker);
  gtk_box_append (GTK_BOX (picker->search_bar), picker->search_entry);
  picker->search_clear = ct_icon_button ("ct-close-symbolic", _("Clear search"), CT_SEARCH_ICON_DP, CT_TOOLBAR_H_DP,
                                         CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (picker->search_clear, "ct-search-clear");
  gtk_widget_set_visible (picker->search_clear, FALSE);
  g_signal_connect (picker->search_clear, "clicked", G_CALLBACK (on_search_clear), picker);
  gtk_box_append (GTK_BOX (picker->search_bar), picker->search_clear);
  gtk_widget_set_visible (picker->search_bar, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), picker->search_bar);

  /* the selection bar: ✕, the count ("Select contacts" at 0), ADD */
  picker->selection_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (picker->selection_bar, "ct-selection-bar");
  back = ct_icon_button ("ct-close-symbolic", _("close"), CT_SELECTION_CLOSE_DP, CT_TOOLBAR_H_DP,
                         CT_RIPPLE_APPCOMPAT);
  gtk_widget_add_css_class (back, "ct-selection-close");
  g_signal_connect (back, "clicked", G_CALLBACK (on_selection_close), picker);
  gtk_box_append (GTK_BOX (picker->selection_bar), back);
  picker->selection_count = ct_label ("", "ct-selection-count", CT_TOOLBAR_TITLE_SP);
  gtk_widget_set_valign (picker->selection_count, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (picker->selection_count, TRUE);
  gtk_box_append (GTK_BOX (picker->selection_bar), picker->selection_count);
  end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  picker->add = ct_text_button (_("Add"), "ct-selection-add");
  gtk_widget_set_valign (picker->add, GTK_ALIGN_CENTER);
  g_signal_connect (picker->add, "clicked", G_CALLBACK (on_add), picker);
  gtk_box_append (GTK_BOX (end), picker->add);
  gtk_box_append (GTK_BOX (picker->selection_bar), end);
  gtk_widget_set_visible (picker->selection_bar, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (frame), picker->selection_bar);
  return frame;
}

static void
on_create (NeoPressable *row, Picker *picker)
{
  GtkWidget *window = picker->window;
  CtPickerCreate create = picker->create;
  gpointer data = picker->create_data;
  GtkWidget *page = g_object_ref (picker->page);

  ct_window_pop (window, page);
  create (data);
  g_object_unref (page);
}

void
ct_picker_offer_new (GtkWidget *page, CtPickerCreate create, gpointer data)
{
  Picker *picker = g_object_get_data (G_OBJECT (page), "ct-picker");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = ct_tinted_icon_new ("ct-person-add-symbolic", 24, CT_C_ACCENT_TEXT);
  GtkWidget *label = ct_label (_("Create new contact"), "ct-picker-create-text", 16);
  GdkRGBA ripple = ct_rgba (CT_RIPPLE_APPCOMPAT);

  picker->create = create;
  picker->create_data = data;
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "ct-picker-create-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), label);
  picker->create_row = neo_pressable_new (box);
  gtk_widget_add_css_class (picker->create_row, "ct-picker-create");
  neo_pressable_set_ripple (NEO_PRESSABLE (picker->create_row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (picker->create_row), &ripple);
  g_signal_connect (picker->create_row, "clicked", G_CALLBACK (on_create), picker);
  /* under the toolbar, over the list */
  gtk_box_insert_child_after (GTK_BOX (page), picker->create_row, gtk_widget_get_first_child (page));
  refresh (picker, FALSE);
}

void
ct_picker_require (GtkWidget *page, CtKind kind)
{
  Picker *picker = g_object_get_data (G_OBJECT (page), "ct-picker");

  picker->require = TRUE;
  picker->need = kind;
  refresh (picker, TRUE);
}

GtkWidget *
ct_picker_new (GtkWidget *window, const char *title, GPtrArray *exclude, gboolean multiple, CtPicked picked,
               gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  GtkWidget *page = ct_page_new (window);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *shadow = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  picker->page = page;
  picker->window = window;
  picker->store = g_object_ref (ct_window_get_store (window));
  picker->settings = g_object_ref (ct_window_get_settings (window));
  picker->multiple = multiple;
  picker->picked = picked;
  picker->data = data;
  picker->exclude = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; exclude && i < exclude->len; i++)
    g_hash_table_add (picker->exclude, g_strdup (exclude->pdata[i]));
  picker->selected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (page), "ct-picker", picker, picker_free);
  gtk_widget_add_css_class (page, "ct-picker");
  ct_page_set_back_handler (CT_PAGE (page), on_back, picker);

  gtk_box_append (GTK_BOX (page), make_toolbar (picker, title));
  picker->list = ct_list_new ();
  gtk_widget_set_vexpand (picker->list, TRUE);
  g_signal_connect (picker->list, "row-activated", G_CALLBACK (on_row_activated), picker);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), picker->list);
  /* nobody left to offer: the list's own "No contacts" */
  picker->empty = ct_label (_("No contacts"), "ct-picker-empty", 18);
  gtk_widget_set_halign (picker->empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (picker->empty, GTK_ALIGN_START);
  gtk_widget_set_can_target (picker->empty, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), picker->empty);
  gtk_widget_add_css_class (shadow, "ct-toolbar-shadow");
  gtk_widget_set_valign (shadow, GTK_ALIGN_START);
  gtk_widget_set_can_target (shadow, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), shadow);
  gtk_widget_set_vexpand (overlay, TRUE);
  gtk_box_append (GTK_BOX (page), overlay);

  g_signal_connect (picker->store, "changed", G_CALLBACK (on_store_changed), picker);
  g_signal_connect (picker->store, "ready", G_CALLBACK (on_store_changed), picker);
  refresh (picker, TRUE);
  return page;
}
