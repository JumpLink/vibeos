/* The Organize tab (ct-tabs.h): the address books, Import and Export, and the labels. */
#include "ct-tabs.h"
#include "ct-people.h"
#include "ct-settings.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

typedef struct {
  GtkWidget *root, *window, *list;
  CtStore *store;
  GSettings *settings;
} Organize;

static void
on_book (VibeosListRow *row, Organize *o)
{
  g_autofree char *uid = g_strdup (g_object_get_data (G_OBJECT (row), "ct-book"));

  ct_window_select_tab (o->window, 0);
  ct_people_show_account (ct_window_get_people (o->window), uid);
}

static void
on_import (VibeosListRow *row, Organize *o)
{
  ct_import_contacts (o->window, o->root);
}

static void
on_export (VibeosListRow *row, Organize *o)
{
  ct_export_all (o->window, o->root);
}

static void
on_labels (VibeosListRow *row, Organize *o)
{
  ct_people_show_labels (ct_window_get_people (o->window));
}

static void
on_settings (VibeosListRow *row, Organize *o)
{
  ct_window_push (o->window, ct_settings_new (o->window));
}

static GtkWidget *
action_row (Organize *o, const char *icon, const char *title, const char *summary, GCallback clicked)
{
  GtkWidget *row = vibeos_list_row_new (title, summary);

  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), icon);
  g_signal_connect (row, "clicked", clicked, o);
  return row;
}

static void
refresh (Organize *o)
{
  VibeosCardList *list = VIBEOS_CARD_LIST (o->list);
  GPtrArray *books = ct_store_get_books (o->store);
  g_autoptr(GPtrArray) entries = ct_store_dup_entries (o->store);
  g_autoptr(GPtrArray) labels = ct_store_dup_labels (o->store);
  g_autofree char *label_count = NULL;

  vibeos_card_list_remove_all (list);

  /* the address books, each with how many contacts it has; a tap shows only those */
  vibeos_card_list_add_group (list, _("Address books"));
  for (guint i = 0; i < books->len; i++)
    {
      CtBook *book = books->pdata[i];
      g_autofree char *count = NULL;
      guint n = 0;
      GtkWidget *row;

      for (guint j = 0; j < entries->len; j++)
        if (g_strcmp0 (((CtEntry *) entries->pdata[j])->book_uid, book->uid) == 0)
          n++;
      count = ct_contacts_count (n, NULL);
      row = action_row (o, book->is_local ? "ct-smartphone-symbolic" : "ct-account-circle-symbolic",
                        ct_book_label (book), count, G_CALLBACK (on_book));
      vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
      g_object_set_data_full (G_OBJECT (row), "ct-book", g_strdup (book->uid), g_free);
      vibeos_card_list_append (list, row);
    }

  /* what can be done with them */
  vibeos_card_list_add_group (list, _("Manage contacts"));
  vibeos_card_list_append (list, action_row (o, "ct-download-symbolic", _("Import from file"),
                                             _("Add contacts from a .vcf file"), G_CALLBACK (on_import)));
  if (entries->len)
    vibeos_card_list_append (list, action_row (o, "ct-upload-symbolic", _("Export to file"),
                                               _("Save your contacts as a .vcf file"), G_CALLBACK (on_export)));
  label_count = g_strdup_printf (g_dngettext (NULL, "%u label", "%u labels", labels->len), labels->len);
  vibeos_card_list_append (list, action_row (o, "ct-label-symbolic", _("Labels"), label_count,
                                             G_CALLBACK (on_labels)));
  vibeos_card_list_append (list, action_row (o, "ct-settings-symbolic", _("Contacts settings"), NULL,
                                             G_CALLBACK (on_settings)));
}

static void
on_store_changed (CtStore *store, Organize *o)
{
  refresh (o);
}

static void
organize_free (gpointer data)
{
  Organize *o = data;

  g_signal_handlers_disconnect_by_data (o->store, o);
  g_object_unref (o->store);
  g_object_unref (o->settings);
  g_free (o);
}

GtkWidget *
ct_organize_new (GtkWidget *window)
{
  Organize *o = g_new0 (Organize, 1);
  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  o->root = root;
  o->window = window;
  o->store = g_object_ref (ct_window_get_store (window));
  o->settings = g_object_ref (ct_window_get_settings (window));
  g_object_set_data_full (G_OBJECT (root), "ct-organize", o, organize_free);
  gtk_widget_add_css_class (root, "ct-organize");

  gtk_box_append (GTK_BOX (root), ct_people_new_pill (ct_window_get_people (window)));
  o->list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (o->list), (int) CT_HEAD_GAP_DP, (int) CT_TAB_PAD_BOTTOM_DP);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), o->list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (root), scroll);

  g_signal_connect (o->store, "changed", G_CALLBACK (on_store_changed), o);
  g_signal_connect (o->store, "books-changed", G_CALLBACK (on_store_changed), o);
  g_signal_connect (o->store, "ready", G_CALLBACK (on_store_changed), o);
  refresh (o);
  return root;
}
