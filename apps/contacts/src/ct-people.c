/* The contact list, the Contacts tab (ct-people.h).
 *
 * Its parts: the list (ct-list.h) with its head, which scrolls away with it: the search pill with the account
 * picture at its end, the filter row ("All contacts ▾" choosing the address book, View labels, the filter toggle)
 * with its chips under it when the toggle is on, and the "Your info" card. Over the list, the FAB in the bottom end
 * corner and, in selection mode, the contextual bar at the top.
 *
 * A label's members are this screen too: the head is then a bar with the label's name, Back, "Add contacts" and the
 * label's menu. Search covers the window (VibeosSearchView) with its own list of results. */
#include "ct-people.h"
#include "ct-avatar.h"
#include "ct-dialogs.h"
#include "ct-list.h"
#include "ct-page.h"
#include "ct-picker.h"
#include "ct-popup.h"
#include "ct-settings.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>
#include <vibeos.h>

typedef enum {
  VIEW_ALL,                     /* everyone, or one address book's people */
  VIEW_LABEL,                   /* a label's members */
} View;

enum {
  FILTER_PHONE = 1 << 0,        /* the chips: only the contacts with a number */
  FILTER_EMAIL = 1 << 1,        /* only those with an email address */
};

typedef struct {
  GtkWidget *root, *window;
  CtStore *store;
  GSettings *settings;
  View view;
  char *label;                  /* VIEW_LABEL: its name */
  char *account;                /* the address book chosen with "All contacts ▾", NULL for all */
  guint filters;
  gboolean filters_shown;

  GtkWidget *list, *head;
  GtkWidget *search_bar;
  GtkWidget *filter_row, *account_button, *account_text, *filter_button, *chips_row, *phone_chip, *email_chip;
  GtkWidget *label_bar, *label_count;
  GtkWidget *info_card, *info_avatar, *info_name;
  GtkWidget *empty, *empty_text, *empty_button;
  GtkWidget *fab;
  VibeosAnim *fab_anim;
  gboolean fab_shown, fab_wanted;
  GtkWidget *selection_bar, *share, *delete, *selection_more;
  gboolean selecting, editing;
  GHashTable *selected;         /* uid set */
  GtkWidget *back_marker;
  gboolean animated_load;

  /* search */
  GtkWidget *search_view, *search_list, *search_none, *recents, *recents_cells;
  gboolean searching;

  /* the label last deleted and its members, for the snackbar's UNDO (kept until the next deletion) */
  char *undo_label;
  GPtrArray *undo_members;
} People;

static void refresh (People *people, gboolean to_top);
static void update_head (People *people);
static void stop_selection (People *people);
static void refresh_search (People *people);

static People *
people_of (GtkWidget *root)
{
  return g_object_get_data (G_OBJECT (root), "ct-people");
}

/* ===== the contacts shown ====================================================================================== */

static gboolean
has_kind (CtEntry *entry, CtKind kind)
{
  g_autoptr(GPtrArray) items = ct_contact_get_items (entry->contact, kind);

  for (guint i = 0; i < items->len; i++)
    if (!ct_item_is_empty (items->pdata[i]))
      return TRUE;
  return FALSE;
}

/* the contacts this view is for: those "Contacts to display" keeps, in the chosen address book, with what the chips
 * ask for; or a label's members */
static GPtrArray *
view_entries (People *people)
{
  GPtrArray *entries = ct_shown_entries (people->store, people->settings);

  for (guint i = entries->len; i > 0; i--)
    {
      CtEntry *entry = entries->pdata[i - 1];
      gboolean keep = TRUE;

      if (people->view == VIEW_LABEL)
        keep = ct_contact_has_label (entry->contact, people->label);
      else
        {
          if (people->account && g_strcmp0 (entry->book_uid, people->account) != 0)
            keep = FALSE;
          if (keep && (people->filters & FILTER_PHONE) && !entry->has_phone)
            keep = FALSE;
          if (keep && (people->filters & FILTER_EMAIL) && !has_kind (entry, CT_KIND_EMAIL))
            keep = FALSE;
        }
      if (!keep)
        g_ptr_array_remove_index_fast (entries, i - 1);
    }
  return entries;
}

/* ===== the FAB ================================================================================================= */

static void
on_fab_anim (VibeosAnim *anim, double value, gpointer data)
{
  People *people = data;

  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (people->fab), value);
}

static void
show_fab (People *people, gboolean show)
{
  double scale = vibeos_pressable_get_scale (VIBEOS_PRESSABLE (people->fab));

  if (people->fab_shown == show)
    return;
  people->fab_shown = show;
  if (show)
    vibeos_anim_start (people->fab_anim, scale, 1, CT_FAB_SCALE_IN_MS, 0, SPONGY_LINEAR_OUT_SLOW_IN);
  else
    vibeos_anim_start (people->fab_anim, scale, 0, CT_FAB_SCALE_OUT_MS, 0, SPONGY_FAST_OUT_LINEAR_IN);
}

/* the FAB shows over the whole list, not a label's or in selection mode; on a tablet it is the rail's instead */
static void
update_fab (People *people)
{
  gtk_widget_set_visible (people->fab, people->fab_wanted);
  show_fab (people, people->view == VIEW_ALL && !people->selecting);
}

GtkWidget *
ct_fab_new (void)
{
  GtkWidget *plus = gtk_image_new_from_icon_name ("ct-add-symbolic");
  GtkWidget *fab = vibeos_pressable_new (plus);
  GdkRGBA fill = vibeos_color (VIBEOS_PRIMARY_CONTAINER);

  gtk_image_set_pixel_size (GTK_IMAGE (plus), 24);
  gtk_widget_add_css_class (fab, "ct-fab");
  gtk_widget_set_size_request (fab, (int) CT_FAB_DP, (int) CT_FAB_DP);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (fab), &fill, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (fab), VIBEOS_RIPPLE_BOUNDED, CT_FAB_RADIUS_DP);
  gtk_accessible_update_property (GTK_ACCESSIBLE (fab), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Create contact"), -1);
  return fab;
}

void
ct_fab_update (GtkWidget *fab)
{
  GdkRGBA fill = vibeos_color (VIBEOS_PRIMARY_CONTAINER);

  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (fab), &fill, TRUE);
}

/* whether Back means something here: the shell's back gesture leaves the app unless it finds a Back button or a
 * dialog, so while Back would leave a mode or a view, a dialog-role marker says so */
static void
update_back_marker (People *people)
{
  gtk_widget_set_visible (people->back_marker, people->selecting || people->view != VIEW_ALL || people->account
                                                || people->filters);
}

/* ===== the list ================================================================================================ */

static void
refresh (People *people, gboolean to_top)
{
  g_autoptr(GPtrArray) entries = view_entries (people);
  CtRows *rows = ct_rows_new (entries, ct_display_order (people->settings), ct_sort_order (people->settings), NULL,
                              people->view == VIEW_ALL && !people->filters);
  guint n = rows->rows->len;
  gboolean ready = ct_store_is_ready (people->store);

  /* a contact that went is not selected any more */
  if (people->selected)
    {
      GHashTableIter iter;
      gpointer uid;

      g_hash_table_iter_init (&iter, people->selected);
      while (g_hash_table_iter_next (&iter, &uid, NULL))
        if (!ct_store_lookup (people->store, uid))
          g_hash_table_iter_remove (&iter);
    }
  ct_list_set_rows (CT_LIST (people->list), rows, to_top);

  /* the empty states: nobody at all, nobody in this book or with these filters, nobody with this label. Before the
   * address books have answered, the list is not empty but unknown, so it says nothing. */
  gtk_widget_set_visible (people->empty, n == 0 && ready);
  if (people->view == VIEW_LABEL)
    {
      gtk_label_set_label (GTK_LABEL (people->empty_text), _("No contacts with this label"));
      gtk_label_set_label (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (people->empty_button))),
                           _("Add contacts"));
    }
  else
    {
      gtk_label_set_label (GTK_LABEL (people->empty_text), people->account || people->filters
                                                             ? _("No contacts here") : _("No contacts yet"));
      gtk_label_set_label (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (people->empty_button))),
                           _("Create contact"));
    }
  if (people->view == VIEW_LABEL)
    {
      g_autofree char *count = ct_contacts_count (n, NULL);

      gtk_label_set_label (GTK_LABEL (people->label_count), count);
    }

  /* the rows' fade-in: once, when the list first has rows */
  if (!people->animated_load && n > 0)
    {
      people->animated_load = TRUE;
      ct_list_animate_load (CT_LIST (people->list));
    }
  if (people->selecting)
    {
      if (g_hash_table_size (people->selected) == 0 && !people->editing)
        stop_selection (people);
      else
        {
          g_autofree char *title = g_strdup_printf ("%u", g_hash_table_size (people->selected));

          vibeos_top_bar_set_selection (VIBEOS_TOP_BAR (people->selection_bar),
                                        people->editing ? _("Remove contacts") : title);
        }
    }
  if (people->searching)
    refresh_search (people);
}

static void
on_store_changed (CtStore *store, People *people)
{
  /* a label that is gone (renamed or deleted elsewhere) is left; so is a book that went */
  if (people->view == VIEW_LABEL && !ct_store_has_label (store, people->label))
    {
      people->view = VIEW_ALL;
      g_clear_pointer (&people->label, g_free);
    }
  if (people->account && !ct_store_get_book (store, people->account))
    g_clear_pointer (&people->account, g_free);
  update_head (people);
  refresh (people, FALSE);
}

static void
on_settings_changed (GSettings *settings, const char *key, People *people)
{
  if (g_str_equal (key, "recent-contacts"))
    {
      if (people->searching)
        refresh_search (people);
      return;
    }
  update_head (people);
  refresh (people, FALSE);
}

/* ===== the head: the search pill, the filter row, the chips, "Your info" ======================================= */

/* the profile's picture and name, for "Your info" and the pill's account disc */
static GBytes *
profile_photo (EContact *profile)
{
  EContactPhoto *photo = profile ? e_contact_get (profile, E_CONTACT_PHOTO) : NULL;
  GBytes *bytes = NULL;

  if (photo && photo->type == E_CONTACT_PHOTO_TYPE_INLINED && photo->data.inlined.length)
    bytes = g_bytes_new (photo->data.inlined.data, photo->data.inlined.length);
  if (photo)
    e_contact_photo_free (photo);
  return bytes;
}

static void
update_head (People *people)
{
  EContact *profile = ct_store_get_profile (people->store);
  g_autofree char *name = profile ? ct_contact_display_name (profile, ct_display_order (people->settings)) : NULL;
  g_autoptr(GBytes) photo = profile_photo (profile);
  gboolean label = people->view == VIEW_LABEL;
  CtBook *book = people->account ? ct_store_get_book (people->store, people->account) : NULL;

  gtk_widget_set_visible (people->search_bar, !label);
  gtk_widget_set_visible (people->filter_row, !label);
  gtk_widget_set_visible (people->chips_row, !label && people->filters_shown);
  gtk_widget_set_visible (people->label_bar, label);
  /* the filter row's bottom margin goes under the chips when they show */
  gtk_widget_set_margin_bottom (people->filter_row, people->filters_shown ? 0 : (int) CT_HEAD_GAP_DP);
  if (label)
    vibeos_top_bar_set_title (VIBEOS_TOP_BAR (gtk_widget_get_first_child (people->label_bar)), people->label);
  gtk_label_set_label (GTK_LABEL (people->account_text), book ? ct_book_label (book) : _("All contacts"));

  /* "Your info": the profile, over the whole list; without it the rows keep the same gap below the head */
  gtk_widget_set_visible (people->info_card, !label && !people->account && !people->filters
                                             && ct_store_can_have_profile (people->store));
  gtk_widget_set_margin_bottom (people->head, gtk_widget_get_visible (people->info_card) ? 0 : (int) CT_HEAD_GAP_DP);
  gtk_label_set_label (GTK_LABEL (people->info_name), !profile ? _("Set up your profile")
                                                     : *name ? name : _("(No name)"));
  ct_avatar_view_set (people->info_avatar, CT_PROFILE_UID, name, photo);

  /* the toggle and the chips show what is on */
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (people->filter_button),
                             &(GdkRGBA) { 0, 0, 0, 0 }, FALSE);
  if (people->filters_shown)
    {
      GdkRGBA on = vibeos_color (VIBEOS_SECONDARY_CONTAINER);

      vibeos_pressable_set_fill (VIBEOS_PRESSABLE (people->filter_button), &on, FALSE);
    }
  for (int i = 0; i < 2; i++)
    {
      GtkWidget *chip = i == 0 ? people->phone_chip : people->email_chip;
      gboolean on = people->filters & (i == 0 ? FILTER_PHONE : FILTER_EMAIL);
      GtkWidget *icon = gtk_widget_get_first_child (vibeos_pressable_get_child (VIBEOS_PRESSABLE (chip)));

      if (on)
        gtk_widget_add_css_class (chip, "on");
      else
        gtk_widget_remove_css_class (chip, "on");
      gtk_image_set_from_icon_name (GTK_IMAGE (icon), on ? "ct-check-symbolic"
                                                         : i == 0 ? "ct-phone-symbolic" : "ct-email-symbolic");
    }
  update_back_marker (people);
}

static void on_search_activated (VibeosSearchBar *bar, People *people);
static void open_search (People *people, const char *text);

/* --- the account sheet, from the pill's picture: the profile, Settings, Import, Export and the address books */

typedef struct {
  People *people;
  VibeosDialog *dialog;
} Sheet;

static void
sheet_close (Sheet *sheet)
{
  vibeos_dialog_respond (sheet->dialog, VIBEOS_RESPONSE_CANCEL);
}

static void
on_sheet_profile (VibeosPressable *row, Sheet *sheet)
{
  People *people = sheet->people;

  sheet_close (sheet);
  if (ct_store_get_profile (people->store))
    ct_window_show_contact (people->window, CT_PROFILE_UID);
  else
    ct_window_edit_contact (people->window, CT_PROFILE_UID);
}

static void
on_sheet_settings (VibeosPressable *row, Sheet *sheet)
{
  People *people = sheet->people;

  sheet_close (sheet);
  ct_window_push (people->window, ct_settings_new (people->window));
}

static void
on_sheet_import (VibeosPressable *row, Sheet *sheet)
{
  People *people = sheet->people;

  sheet_close (sheet);
  ct_import_contacts (people->window, people->root);
}

static void
on_sheet_export (VibeosPressable *row, Sheet *sheet)
{
  People *people = sheet->people;

  sheet_close (sheet);
  ct_export_all (people->window, people->root);
}

static void
show_account (People *people, const char *book_uid)
{
  stop_selection (people);
  g_free (people->account);
  people->account = g_strdup (book_uid);
  people->animated_load = FALSE;
  update_head (people);
  refresh (people, TRUE);
}

static void
on_sheet_book (VibeosPressable *row, Sheet *sheet)
{
  People *people = sheet->people;
  g_autofree char *uid = g_strdup (g_object_get_data (G_OBJECT (row), "ct-book"));

  sheet_close (sheet);
  show_account (people, uid);
}

static GtkWidget *
sheet_row (const char *icon, const char *title, const char *summary, GCallback clicked, Sheet *sheet)
{
  GtkWidget *row = vibeos_list_row_new (title, summary);

  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), icon);
  gtk_widget_add_css_class (row, "ct-sheet-row");
  g_signal_connect (row, "clicked", clicked, sheet);
  return row;
}

static void
on_sheet_response (VibeosDialog *dialog, int response, Sheet *sheet)
{
  g_free (sheet);
}

static void
on_account_disc (VibeosPressable *button, People *people)
{
  Sheet *sheet = g_new0 (Sheet, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *avatar = ct_avatar_view_new (CT_SHEET_AVATAR_DP);
  GtkWidget *cards = vibeos_card_list_new ();
  GtkWidget *profile_row;
  EContact *profile = ct_store_get_profile (people->store);
  g_autofree char *name = profile ? ct_contact_display_name (profile, ct_display_order (people->settings)) : NULL;
  g_autoptr(GBytes) photo = profile_photo (profile);
  g_autoptr(GPtrArray) entries = ct_store_dup_entries (people->store);
  GPtrArray *books = ct_store_get_books (people->store);
  GtkWidget *label;

  sheet->people = people;
  sheet->dialog = vibeos_dialog_new (NULL);
  vibeos_dialog_set_flush (sheet->dialog, TRUE);
  gtk_widget_add_css_class (box, "ct-sheet");

  /* the profile: the picture, the name, a tap opens it */
  ct_avatar_view_set (avatar, CT_PROFILE_UID, name, photo);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (header), avatar);
  label = ct_text (profile && *name ? name : _("Your info"), "ct-sheet-name");
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (lines), label);
  label = ct_text (profile ? _("Your info") : _("Set up your profile"), "ct-sheet-summary");
  gtk_box_append (GTK_BOX (lines), label);
  gtk_widget_set_valign (lines, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (lines, TRUE);
  gtk_box_append (GTK_BOX (header), lines);
  gtk_widget_add_css_class (header, "ct-sheet-header");
  profile_row = vibeos_pressable_new (header);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (profile_row), VIBEOS_RIPPLE_BOUNDED, 0);
  g_signal_connect (profile_row, "clicked", G_CALLBACK (on_sheet_profile), sheet);
  gtk_widget_set_visible (profile_row, ct_store_can_have_profile (people->store));
  gtk_box_append (GTK_BOX (box), profile_row);

  /* on cards under it: Settings, Import and Export; then the address books */
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 0, 8);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), sheet_row ("ct-settings-symbolic", _("Contacts settings"), NULL,
                                                                G_CALLBACK (on_sheet_settings), sheet));
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), sheet_row ("ct-download-symbolic", _("Import"),
                                                                _("From a .vcf file"),
                                                                G_CALLBACK (on_sheet_import), sheet));
  if (ct_store_count (people->store) > 0)
    vibeos_card_list_append (VIBEOS_CARD_LIST (cards), sheet_row ("ct-upload-symbolic", _("Export"),
                                                                  _("To a .vcf file"),
                                                                  G_CALLBACK (on_sheet_export), sheet));

  /* the address books, each with how many it has; a tap shows only its people */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Address books"));
  for (guint i = 0; i < books->len; i++)
    {
      CtBook *book = books->pdata[i];
      guint count = 0;
      g_autofree char *summary = NULL;
      GtkWidget *row;

      for (guint j = 0; j < entries->len; j++)
        if (g_strcmp0 (((CtEntry *) entries->pdata[j])->book_uid, book->uid) == 0)
          count++;
      summary = ct_contacts_count (count, NULL);
      row = sheet_row (book->is_local ? "ct-smartphone-symbolic" : "ct-account-circle-symbolic", ct_book_label (book),
                       summary, G_CALLBACK (on_sheet_book), sheet);
      g_object_set_data_full (G_OBJECT (row), "ct-book", g_strdup (book->uid), g_free);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
    }
  gtk_box_append (GTK_BOX (box), cards);

  vibeos_dialog_set_content (sheet->dialog, box);
  g_signal_connect (sheet->dialog, "response", G_CALLBACK (on_sheet_response), sheet);
  vibeos_dialog_present (sheet->dialog, people->root);
}

/* --- "All contacts ▾": the address book the list shows */

static void
on_account_chosen (int response, gpointer data)
{
  People *people = data;
  GPtrArray *books = ct_store_get_books (people->store);

  if (response == 0)
    show_account (people, NULL);
  else if (response > 0 && (guint) response <= books->len)
    show_account (people, ((CtBook *) books->pdata[response - 1])->uid);
}

static void
on_account_button (VibeosPressable *button, People *people)
{
  GPtrArray *books = ct_store_get_books (people->store);
  const char *labels[33];
  guint n = MIN (books->len, 32);

  labels[0] = _("All contacts");
  for (guint i = 0; i < n; i++)
    labels[i + 1] = ct_book_label (books->pdata[i]);
  ct_list_dialog (people->root, CT_DIALOG_APPCOMPAT, _("Contacts to show"), labels, (int) n + 1, FALSE,
                  on_account_chosen, people);
}

/* --- View labels, the filter toggle and its chips */

static void
on_labels_button (VibeosPressable *button, People *people)
{
  ct_people_show_labels (people->root);
}

static void
on_filter_button (VibeosPressable *button, People *people)
{
  people->filters_shown = !people->filters_shown;
  /* hiding the chips drops what they filtered */
  if (!people->filters_shown && people->filters)
    {
      people->filters = 0;
      refresh (people, TRUE);
    }
  update_head (people);
}

static void
on_chip (VibeosPressable *chip, People *people)
{
  people->filters ^= GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (chip), "ct-filter"));
  people->animated_load = FALSE;
  update_head (people);
  refresh (people, TRUE);
}

static GtkWidget *
make_chip (People *people, const char *icon_name, const char *text, guint filter)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *chip;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), (int) CT_CHIP_ICON_DP);
  gtk_widget_add_css_class (icon, "ct-chip-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), ct_text (text, "ct-chip-text"));
  gtk_widget_add_css_class (box, "ct-chip-box");
  chip = vibeos_pressable_new (box);
  gtk_widget_add_css_class (chip, "ct-chip");
  gtk_widget_set_valign (chip, GTK_ALIGN_CENTER);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (chip), VIBEOS_RIPPLE_BOUNDED, CT_CHIP_RADIUS_DP);
  g_object_set_data (G_OBJECT (chip), "ct-filter", GUINT_TO_POINTER (filter));
  g_signal_connect (chip, "clicked", G_CALLBACK (on_chip), people);
  return chip;
}

/* --- "Your info" */

static void
on_info_card (VibeosPressable *card, People *people)
{
  if (ct_store_get_profile (people->store))
    ct_window_show_contact (people->window, CT_PROFILE_UID);
  else
    ct_window_edit_contact (people->window, CT_PROFILE_UID);
}

static void
on_info_share (VibeosPressable *button, People *people)
{
  g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);

  if (!ct_store_get_profile (people->store))
    {
      ct_window_edit_contact (people->window, CT_PROFILE_UID);
      return;
    }
  g_ptr_array_add (uids, g_strdup (CT_PROFILE_UID));
  ct_export_contacts (people->window, uids, g_dngettext (NULL, "Share contact via", "Share contacts via", 1));
}

static GtkWidget *
make_info_card (People *people)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *lines = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *share = ct_icon_button ("ct-share-symbolic", _("Share"), 48, 48);
  GtkWidget *press;

  people->info_avatar = ct_avatar_view_new (CT_ROW_PHOTO_DP);
  gtk_widget_set_valign (people->info_avatar, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (people->info_avatar, "ct-info-avatar");
  gtk_box_append (GTK_BOX (card), people->info_avatar);
  gtk_box_append (GTK_BOX (lines), ct_text (_("Your info"), "ct-info-title"));
  people->info_name = ct_text ("", "ct-info-name");
  gtk_label_set_ellipsize (GTK_LABEL (people->info_name), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (lines), people->info_name);
  gtk_widget_set_valign (lines, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (lines, TRUE);
  gtk_widget_add_css_class (lines, "ct-info-lines");
  gtk_box_append (GTK_BOX (card), lines);
  gtk_widget_set_valign (share, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (share, "ct-info-share");
  g_signal_connect (share, "clicked", G_CALLBACK (on_info_share), people);
  gtk_box_append (GTK_BOX (card), share);
  gtk_widget_add_css_class (card, "ct-info-box");
  press = vibeos_pressable_new (card);
  gtk_widget_add_css_class (press, "ct-info-card");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (press), VIBEOS_RIPPLE_BOUNDED, CT_CARD_OUTER_RADIUS_DP);
  gtk_accessible_update_property (GTK_ACCESSIBLE (press), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Your info"), -1);
  g_signal_connect (press, "clicked", G_CALLBACK (on_info_card), people);
  return press;
}

/* --- the label view's bar */

static void on_label_more (VibeosPressable *button, People *people);
static void add_members (People *people);

static void
on_label_back (VibeosTopBar *bar, People *people)
{
  ct_people_back (people->root);
}

static void
on_label_add (VibeosPressable *button, People *people)
{
  add_members (people);
}

static GtkWidget *
make_label_bar (People *people)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new ("");
  GtkWidget *add, *more;

  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_label_back), people);
  add = vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), "ct-person-add-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (add), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Add contacts"), -1);
  g_signal_connect (add, "clicked", G_CALLBACK (on_label_add), people);
  more = vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), "ct-more-vert-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (more), GTK_ACCESSIBLE_PROPERTY_LABEL, _("More options"), -1);
  g_signal_connect (more, "clicked", G_CALLBACK (on_label_more), people);
  gtk_box_append (GTK_BOX (box), bar);
  people->label_count = ct_text ("", "ct-label-count");
  gtk_box_append (GTK_BOX (box), people->label_count);
  return box;
}

/* the pill's picture follows the profile's */
static void
update_disc (GtkWidget *avatar, People *people)
{
  EContact *profile = ct_store_get_profile (people->store);
  g_autofree char *name = profile ? ct_contact_display_name (profile, ct_display_order (people->settings)) : NULL;
  g_autoptr(GBytes) photo = profile_photo (profile);

  ct_avatar_view_set (avatar, CT_PROFILE_UID, name, photo);
}

static void
on_disc_profile (CtStore *store, GtkWidget *avatar)
{
  update_disc (avatar, people_of (g_object_get_data (G_OBJECT (avatar), "ct-people")));
}

static void
on_pill_activated (VibeosSearchBar *bar, GtkWidget *root)
{
  on_search_activated (bar, people_of (root));
}

static void
on_pill_disc (VibeosPressable *button, GtkWidget *root)
{
  on_account_disc (button, people_of (root));
}

static void
on_pill_theme (VibeosTheme *theme, GtkWidget *bar)
{
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);

  vibeos_search_bar_set_fill (VIBEOS_SEARCH_BAR (bar), &fill);
}

/* the search pill, the account picture at its end: a tap on the pill opens search, one on the picture the sheet */
static GtkWidget *
make_pill (People *people)
{
  GtkWidget *bar = vibeos_search_bar_new (_("Search contacts"));
  GtkWidget *avatar = ct_avatar_view_new (CT_PILL_DISC_DP);
  GtkWidget *disc = vibeos_pressable_new (avatar);

  vibeos_search_bar_set_geometry (VIBEOS_SEARCH_BAR (bar), (int) CT_PILL_H_DP, (int) CT_LIST_SIDE_DP,
                                  (int) CT_PILL_ICON_X_DP, (int) CT_PILL_TEXT_X_DP);
  gtk_widget_set_margin_top (bar, (int) CT_HEAD_GAP_DP);
  gtk_widget_add_css_class (bar, "ct-pill");
  g_signal_connect (bar, "activated", G_CALLBACK (on_pill_activated), people->root);
  gtk_widget_add_css_class (disc, "ct-pill-disc");
  gtk_widget_set_valign (disc, GTK_ALIGN_CENTER);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (disc), VIBEOS_RIPPLE_CIRCLE, CT_PILL_DISC_DP / 2);
  gtk_accessible_update_property (GTK_ACCESSIBLE (disc), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Account and settings"),
                                  -1);
  g_signal_connect (disc, "clicked", G_CALLBACK (on_pill_disc), people->root);
  vibeos_search_bar_set_end (VIBEOS_SEARCH_BAR (bar), disc);
  on_pill_theme (NULL, bar);
  g_signal_connect_object (vibeos_theme_get_default (), "changed", G_CALLBACK (on_pill_theme), bar, 0);
  g_object_set_data (G_OBJECT (avatar), "ct-people", people->root);
  g_signal_connect_object (people->store, "profile-changed", G_CALLBACK (on_disc_profile), avatar, 0);
  g_signal_connect_object (people->store, "ready", G_CALLBACK (on_disc_profile), avatar, 0);
  update_disc (avatar, people);
  return bar;
}

GtkWidget *
ct_people_new_pill (GtkWidget *root)
{
  return make_pill (people_of (root));
}

void
ct_people_open_search (GtkWidget *root)
{
  open_search (people_of (root), NULL);
}

static GtkWidget *
make_head (People *people)
{
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *account = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon, *labels, *chips, *scroll;

  gtk_widget_add_css_class (head, "ct-head");
  people->search_bar = make_pill (people);
  gtk_box_append (GTK_BOX (head), people->search_bar);

  /* the filter row: the address book's button at the start, View labels and the filter toggle at the end */
  gtk_widget_add_css_class (row, "ct-list-filters");
  gtk_widget_set_margin_top (row, (int) CT_HEAD_GAP_DP);
  icon = gtk_image_new_from_icon_name ("ct-group-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), (int) CT_ACCOUNT_ICON_DP);
  gtk_box_append (GTK_BOX (account), icon);
  people->account_text = ct_text (_("All contacts"), "ct-account-text");
  gtk_label_set_ellipsize (GTK_LABEL (people->account_text), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (account), people->account_text);
  icon = gtk_image_new_from_icon_name ("ct-drop-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), (int) CT_ACCOUNT_ICON_DP);
  gtk_box_append (GTK_BOX (account), icon);
  gtk_widget_add_css_class (account, "ct-account-box");
  people->account_button = vibeos_pressable_new (account);
  gtk_widget_add_css_class (people->account_button, "ct-account-button");
  gtk_widget_set_valign (people->account_button, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (people->account_button, FALSE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (people->account_button), VIBEOS_RIPPLE_BOUNDED,
                               CT_ACCOUNT_BUTTON_H_DP / 2);
  g_signal_connect (people->account_button, "clicked", G_CALLBACK (on_account_button), people);
  gtk_box_append (GTK_BOX (row), people->account_button);
  gtk_box_append (GTK_BOX (row), g_object_new (GTK_TYPE_BOX, "hexpand", TRUE, NULL));
  labels = ct_icon_button ("ct-label-symbolic", _("View labels"), 48, 48);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (labels), VIBEOS_RIPPLE_CIRCLE, 20);
  g_signal_connect (labels, "clicked", G_CALLBACK (on_labels_button), people);
  gtk_box_append (GTK_BOX (row), labels);
  people->filter_button = ct_icon_button ("ct-filter-list-symbolic", _("Show list filters"), 48, 48);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (people->filter_button), VIBEOS_RIPPLE_CIRCLE, 20);
  g_signal_connect (people->filter_button, "clicked", G_CALLBACK (on_filter_button), people);
  gtk_box_append (GTK_BOX (row), people->filter_button);
  people->filter_row = row;
  gtk_box_append (GTK_BOX (head), row);

  /* the chips, scrolling sideways */
  chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, (int) CT_CHIP_GAP_DP);
  gtk_widget_add_css_class (chips, "ct-chips");
  people->phone_chip = make_chip (people, "ct-phone-symbolic", _("Phone contacts"), FILTER_PHONE);
  gtk_box_append (GTK_BOX (chips), people->phone_chip);
  people->email_chip = make_chip (people, "ct-email-symbolic", _("Email contacts"), FILTER_EMAIL);
  gtk_box_append (GTK_BOX (chips), people->email_chip);
  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), chips);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_widget_add_css_class (scroll, "ct-chips-row");
  people->chips_row = scroll;
  gtk_box_append (GTK_BOX (head), scroll);

  /* a label's bar, in place of the pill and the filter row */
  people->label_bar = make_label_bar (people);
  gtk_box_append (GTK_BOX (head), people->label_bar);

  people->info_card = make_info_card (people);
  gtk_widget_set_margin_top (people->info_card, (int) CT_HEAD_GAP_DP);
  gtk_box_append (GTK_BOX (head), people->info_card);
  return head;
}

/* ===== search ================================================================================================== */

static void
on_recent_clicked (VibeosPressable *cell, People *people)
{
  g_autofree char *uid = g_strdup (g_object_get_data (G_OBJECT (cell), "ct-uid"));

  vibeos_search_view_close (VIBEOS_SEARCH_VIEW (people->search_view));
  ct_window_show_contact (people->window, uid);
}

static void
on_recents_clear (VibeosPressable *button, People *people)
{
  g_settings_set_strv (people->settings, "recent-contacts", NULL);
}

/* the empty query's "Recent contacts": the last opened, a picture and a first name each */
static void
fill_recents (People *people)
{
  g_auto(GStrv) recent = g_settings_get_strv (people->settings, "recent-contacts");
  GtkWidget *child;
  guint shown = 0;

  while ((child = gtk_widget_get_first_child (people->recents_cells)))
    gtk_box_remove (GTK_BOX (people->recents_cells), child);
  for (guint i = 0; recent[i] && shown < CT_RECENT_CELLS; i++)
    {
      CtEntry *entry = ct_store_lookup (people->store, recent[i]);
      const char *name;
      GtkWidget *cell, *box, *avatar, *label;

      if (!entry)
        continue;
      name = entry->name[ct_display_order (people->settings)];
      box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      avatar = ct_avatar_view_new (CT_RECENT_AVATAR_DP);
      ct_avatar_view_set (avatar, entry->uid, name, entry->photo);
      gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), avatar);
      label = ct_text (name, "ct-recent-name");
      gtk_label_set_xalign (GTK_LABEL (label), 0.5);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (box), label);
      cell = vibeos_pressable_new (box);
      gtk_widget_add_css_class (cell, "ct-recent-cell");
      gtk_widget_set_size_request (cell, (int) CT_RECENT_CELL_W_DP, -1);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (cell), VIBEOS_RIPPLE_BOUNDED, 12);
      g_object_set_data_full (G_OBJECT (cell), "ct-uid", g_strdup (entry->uid), g_free);
      g_signal_connect (cell, "clicked", G_CALLBACK (on_recent_clicked), people);
      gtk_box_append (GTK_BOX (people->recents_cells), cell);
      shown++;
    }
  gtk_widget_set_visible (people->recents, shown > 0);
}

static void
refresh_search (People *people)
{
  const char *query = vibeos_search_view_get_text (VIBEOS_SEARCH_VIEW (people->search_view));
  g_autoptr(GPtrArray) entries = ct_shown_entries (people->store, people->settings);
  gboolean empty = !query || !*query;
  CtRows *rows = ct_rows_new (entries, ct_display_order (people->settings), ct_sort_order (people->settings),
                              empty ? NULL : query, FALSE);

  if (empty)
    {
      /* nothing typed: no results yet, only the recents */
      g_array_set_size (rows->rows, 0);
      g_array_set_size (rows->sections, 0);
      fill_recents (people);
    }
  else
    gtk_widget_set_visible (people->recents, FALSE);
  gtk_widget_set_visible (people->search_none, !empty && rows->rows->len == 0);
  ct_list_set_rows (CT_LIST (people->search_list), rows, TRUE);
}

static void
on_search_changed (VibeosSearchView *view, const char *text, People *people)
{
  refresh_search (people);
}

static void
on_search_closed (VibeosSearchView *view, People *people)
{
  people->searching = FALSE;
}

static void
on_search_row (CtList *list, guint index, People *people)
{
  CtEntry *entry = ct_list_get_entry (list, index);
  g_autofree char *uid = entry ? g_strdup (entry->uid) : NULL;

  if (!uid)
    return;
  vibeos_search_view_close (VIBEOS_SEARCH_VIEW (people->search_view));
  ct_window_show_contact (people->window, uid);
}

static void
open_search (People *people, const char *text)
{
  if (people->searching)
    return;
  stop_selection (people);
  people->searching = TRUE;
  vibeos_search_view_set_text (VIBEOS_SEARCH_VIEW (people->search_view), text ? text : "");
  refresh_search (people);
  vibeos_search_view_open (VIBEOS_SEARCH_VIEW (people->search_view), people->root);
}

static void
on_search_activated (VibeosSearchBar *bar, People *people)
{
  open_search (people, NULL);
}

static GtkWidget *
make_search (People *people)
{
  GtkWidget *view = vibeos_search_view_new (_("Search contacts"));
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *clear = vibeos_text_button_new (_("Clear all"));

  g_object_ref_sink (view);
  /* the recents over the results, which are empty while they show */
  people->recents = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (people->recents, "ct-recents");
  gtk_box_append (GTK_BOX (title_row), ct_text (_("Recent contacts"), "ct-recents-title"));
  gtk_widget_set_hexpand (gtk_widget_get_first_child (title_row), TRUE);
  gtk_widget_set_valign (gtk_widget_get_first_child (title_row), GTK_ALIGN_CENTER);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_recents_clear), people);
  gtk_box_append (GTK_BOX (title_row), clear);
  gtk_box_append (GTK_BOX (people->recents), title_row);
  people->recents_cells = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (people->recents_cells, "ct-recents-cells");
  gtk_box_append (GTK_BOX (people->recents), people->recents_cells);
  gtk_box_append (GTK_BOX (column), people->recents);

  people->search_list = ct_list_new ();
  ct_list_set_search (CT_LIST (people->search_list), TRUE);
  gtk_widget_set_vexpand (people->search_list, TRUE);
  g_signal_connect (people->search_list, "row-activated", G_CALLBACK (on_search_row), people);
  gtk_box_append (GTK_BOX (column), people->search_list);
  gtk_overlay_set_child (GTK_OVERLAY (content), column);
  people->search_none = ct_text (_("No contacts found"), "ct-no-results");
  gtk_widget_set_halign (people->search_none, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (people->search_none, GTK_ALIGN_START);
  gtk_widget_set_can_target (people->search_none, FALSE);
  gtk_widget_set_visible (people->search_none, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (content), people->search_none);
  gtk_widget_add_css_class (content, "ct-search-content");
  vibeos_search_view_set_content (VIBEOS_SEARCH_VIEW (view), content);
  g_signal_connect (view, "search-changed", G_CALLBACK (on_search_changed), people);
  g_signal_connect (view, "closed", G_CALLBACK (on_search_closed), people);
  return view;
}

/* typing on the list opens search with what was typed */
static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, People *people)
{
  gunichar c = gdk_keyval_to_unicode (keyval);
  char text[8] = { 0 };

  if (people->searching || people->selecting || !gtk_widget_is_drawable (people->root)
      || ct_window_top (people->window) != ct_window_home (people->window)
      || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) || !c || !g_unichar_isprint (c) || g_unichar_isspace (c))
    return FALSE;
  g_unichar_to_utf8 (c, text);
  open_search (people, text);
  return TRUE;
}

/* ===== selection mode ========================================================================================== */

static void
start_selection (People *people, const char *uid, gboolean editing)
{
  g_autofree char *title = NULL;

  people->selecting = TRUE;
  people->editing = editing;
  g_hash_table_remove_all (people->selected);
  if (uid)
    g_hash_table_add (people->selected, g_strdup (uid));
  ct_list_set_selection (CT_LIST (people->list), people->selected);
  title = g_strdup_printf ("%u", g_hash_table_size (people->selected));
  vibeos_top_bar_set_selection (VIBEOS_TOP_BAR (people->selection_bar), editing ? _("Remove contacts") : title);
  gtk_widget_set_visible (people->share, people->view == VIEW_ALL);
  gtk_widget_set_visible (people->delete, people->view == VIEW_ALL);
  gtk_widget_set_visible (people->selection_more, people->view == VIEW_LABEL && !editing);
  gtk_widget_set_visible (people->selection_bar, TRUE);
  update_fab (people);
  update_back_marker (people);
}

static void
stop_selection (People *people)
{
  if (!people->selecting)
    return;
  people->selecting = FALSE;
  people->editing = FALSE;
  g_hash_table_remove_all (people->selected);
  ct_list_set_selection (CT_LIST (people->list), NULL);
  vibeos_top_bar_set_selection (VIBEOS_TOP_BAR (people->selection_bar), NULL);
  gtk_widget_set_visible (people->selection_bar, FALSE);
  update_fab (people);
  update_back_marker (people);
}

static GPtrArray *
selected_uids (People *people)
{
  GPtrArray *uids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  gpointer uid;

  g_hash_table_iter_init (&iter, people->selected);
  while (g_hash_table_iter_next (&iter, &uid, NULL))
    g_ptr_array_add (uids, g_strdup (uid));
  return uids;
}

static void
on_row_activated (CtList *list, guint index, People *people)
{
  CtEntry *entry = ct_list_get_entry (list, index);

  if (!entry)
    return;
  if (people->selecting)
    {
      g_autofree char *title = NULL;

      /* "Remove contacts" (edit mode) has no checkboxes to toggle: a tap on a row opens it */
      if (people->editing)
        {
          ct_window_show_contact (people->window, entry->uid);
          return;
        }
      if (!g_hash_table_remove (people->selected, entry->uid))
        g_hash_table_add (people->selected, g_strdup (entry->uid));
      /* unchecking the last row ends the mode */
      if (g_hash_table_size (people->selected) == 0)
        {
          stop_selection (people);
          return;
        }
      ct_list_update_selection (list);
      title = g_strdup_printf ("%u", g_hash_table_size (people->selected));
      vibeos_top_bar_set_selection (VIBEOS_TOP_BAR (people->selection_bar), title);
      return;
    }
  ct_window_show_contact (people->window, entry->uid);
}

static void
on_row_long_pressed (CtList *list, guint index, People *people)
{
  CtEntry *entry = ct_list_get_entry (list, index);

  /* a long press in edit mode is ignored */
  if (!entry || people->editing)
    return;
  if (people->selecting)
    {
      on_row_activated (list, index, people);
      return;
    }
  start_selection (people, entry->uid, FALSE);
}

static void
on_selection_closed (VibeosTopBar *bar, People *people)
{
  stop_selection (people);
}

/* Share: there is no share sheet to hand a vCard of the selection to another app, so the vCard is written where the
 * person chooses (the file chooser), from where it can go anywhere. */
static void
on_share (VibeosPressable *button, People *people)
{
  g_autoptr(GPtrArray) uids = selected_uids (people);

  ct_export_contacts (people->window, uids,
                      g_dngettext (NULL, "Share contact via", "Share contacts via", uids->len));
  stop_selection (people);
}

typedef struct {
  People *people;
  GPtrArray *uids;
} Deletion;

static void
on_deleted (const char *uid, const GError *error, gpointer data)
{
  Deletion *deletion = data;

  if (error)
    g_warning ("delete: %s", error->message);
  else
    ct_window_toast (deletion->people->window, g_dngettext (NULL, "Contact deleted", "Contacts deleted",
                                                            deletion->uids->len));
  g_ptr_array_unref (deletion->uids);
  g_free (deletion);
}

static void
on_delete_confirmed (int response, gpointer data)
{
  Deletion *deletion = data;

  if (response != CT_DIALOG_OK)
    {
      g_ptr_array_unref (deletion->uids);
      g_free (deletion);
      return;
    }
  stop_selection (deletion->people);
  ct_store_delete (deletion->people->store, deletion->uids, on_deleted, deletion);
}

static void
on_delete (VibeosPressable *button, People *people)
{
  Deletion *deletion = g_new0 (Deletion, 1);

  deletion->people = people;
  deletion->uids = selected_uids (people);
  ct_confirm_dialog (people->root, NULL, _("Delete selected contacts?"), _("Delete"), on_delete_confirmed, deletion);
}

/* ===== the label view ========================================================================================= */

static void
show_view (People *people, View view, const char *name)
{
  stop_selection (people);
  people->view = view;
  g_free (people->label);
  people->label = view == VIEW_LABEL ? g_strdup (name) : NULL;
  people->animated_load = FALSE;
  update_head (people);
  update_fab (people);
  update_back_marker (people);
  refresh (people, TRUE);
}

static void
on_members_added (GPtrArray *uids, gpointer data)
{
  People *people = data;

  if (!uids->len || !people->label)
    return;
  ct_store_add_to_label (people->store, uids, people->label);
  ct_window_toast (people->window, _("Added to label"));
}

/* "Add contacts": the picker, without the label's members */
static void
add_members (People *people)
{
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);

  ct_window_push (people->window, ct_picker_new (people->window, _("Select"), members, TRUE, on_members_added,
                                                 people));
}

static void
on_empty_button (VibeosPressable *button, People *people)
{
  if (people->view == VIEW_LABEL)
    add_members (people);
  else
    ct_window_new_contact (people->window, NULL);
}

/* the members' (or the selection's) first email address or number each, to one mailto: or smsto: link. For a
 * person with several it is the default, else the first. */
static void
send_to (People *people, CtKind kind)
{
  g_autoptr(GPtrArray) uids = people->selecting ? selected_uids (people)
                                                : ct_store_label_members (people->store, people->label);
  g_autoptr(GString) to = g_string_new (NULL);
  g_autofree char *uri = NULL;
  guint missing = 0;

  for (guint i = 0; i < uids->len; i++)
    {
      CtEntry *entry = ct_store_lookup (people->store, uids->pdata[i]);
      g_autoptr(GPtrArray) items = entry ? ct_contact_get_items (entry->contact, kind) : NULL;
      CtItem *chosen = NULL;

      for (guint j = 0; items && j < items->len; j++)
        if (!chosen || ((CtItem *) items->pdata[j])->primary)
          chosen = chosen && chosen->primary ? chosen : items->pdata[j];
      if (!chosen)
        {
          missing++;
          continue;
        }
      if (to->len)
        g_string_append_c (to, ',');
      g_string_append_uri_escaped (to, chosen->value, "+@", TRUE);
    }
  if (missing)
    ct_window_toast (people->window, kind == CT_KIND_EMAIL ? _("Some contacts do not have emails.")
                                                           : _("Some contacts do not have phone numbers."));
  if (!to->len)
    return;
  uri = g_strconcat (kind == CT_KIND_EMAIL ? "mailto:" : "smsto:", to->str, NULL);
  if (!ct_open_uri (people->window, uri))
    ct_window_toast (people->window, _("No app was found to handle this action."));
  if (people->selecting)
    stop_selection (people);
}

static const char *
label_exists (const char *text, gpointer data)
{
  People *people = data;

  /* the name it has already is not "another" label (a rename that changes nothing just closes) */
  if (people->label && g_str_equal (text, people->label))
    return NULL;
  return ct_store_has_label (people->store, text) ? _("That label already exists") : NULL;
}

static void
on_renamed (const char *text, gpointer data)
{
  People *people = data;

  if (!text || !people->label || g_str_equal (text, people->label))
    return;
  ct_store_rename_label (people->store, people->label, text);
  g_free (people->label);
  people->label = g_strdup (text);
  update_head (people);
  ct_window_toast (people->window, _("Label updated"));
}

static void
forget_undo (People *people)
{
  g_clear_pointer (&people->undo_label, g_free);
  g_clear_pointer (&people->undo_members, g_ptr_array_unref);
}

static void
on_undo_delete (gpointer data)
{
  People *people = data;

  /* the label comes back, with its members */
  if (!people->undo_label)
    return;
  ct_store_create_label (people->store, people->undo_label);
  if (people->undo_members && people->undo_members->len)
    ct_store_add_to_label (people->store, people->undo_members, people->undo_label);
  forget_undo (people);
}

/* the label's deletion, then the snackbar with UNDO. The snackbar cannot say when it has gone without its action
 * being taken, so the undo is kept here until the next deletion replaces it. */
static void
delete_label (People *people)
{
  forget_undo (people);
  people->undo_label = g_strdup (people->label);
  people->undo_members = ct_store_delete_label (people->store, people->label);
  show_view (people, VIEW_ALL, NULL);
  ct_window_snackbar (people->window, _("Label deleted"), _("Undo"), on_undo_delete, people);
}

static void
on_delete_label_confirmed (int response, gpointer data)
{
  if (response == CT_DIALOG_OK)
    delete_label (data);
}

static void
on_delete_label (People *people)
{
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);
  g_autofree char *message = NULL;

  /* an empty label goes at once */
  if (!members->len)
    {
      g_autofree char *gone = g_strdup (people->label);

      ct_store_delete_label (people->store, gone);
      show_view (people, VIEW_ALL, NULL);
      return;
    }
  message = g_strdup_printf (_("Delete the label \"%s\"? (Contacts themselves will not be deleted.)"),
                             people->label);
  ct_confirm_dialog (people->root, NULL, message, _("OK"), on_delete_label_confirmed, people);
}

static void
on_removed_from_label (People *people)
{
  g_autoptr(GPtrArray) uids = selected_uids (people);

  ct_store_remove_from_label (people->store, uids, people->label);
  stop_selection (people);
  ct_window_toast (people->window, _("Removed from label"));
}

typedef enum {
  MENU_SEND_EMAIL,
  MENU_SEND_MESSAGE,
  MENU_REMOVE_CONTACTS,
  MENU_RENAME,
  MENU_DELETE,
  MENU_REMOVE_FROM_LABEL,
} MenuItem;

typedef struct {
  People *people;
  MenuItem items[6];
  int n;
} Menu;

static void
on_menu_chosen (int index, gpointer data)
{
  Menu *menu = data;
  People *people = menu->people;

  switch (menu->items[index])
    {
    case MENU_SEND_EMAIL:
      send_to (people, CT_KIND_EMAIL);
      break;
    case MENU_SEND_MESSAGE:
      send_to (people, CT_KIND_PHONE);
      break;
    case MENU_REMOVE_CONTACTS:
      start_selection (people, NULL, TRUE);
      break;
    case MENU_RENAME:
      ct_text_dialog (people->root, _("Rename label"), people->label, _("Label name"), 40, label_exists, on_renamed,
                      people);
      break;
    case MENU_DELETE:
      on_delete_label (people);
      break;
    case MENU_REMOVE_FROM_LABEL:
      on_removed_from_label (people);
      break;
    }
}

/* a label's menu. Send email shows only when something takes mailto: links. */
static void
on_label_more (VibeosPressable *button, People *people)
{
  static Menu menu;
  const char *labels[6];
  g_autoptr(GPtrArray) members = ct_store_label_members (people->store, people->label);
  gboolean has_members = members->len > 0;

  menu.people = people;
  menu.n = 0;
  if (!people->editing && (has_members || people->selecting))
    {
      if (ct_can_open_scheme ("mailto"))
        {
          labels[menu.n] = _("Send email");
          menu.items[menu.n++] = MENU_SEND_EMAIL;
        }
      labels[menu.n] = _("Send message");
      menu.items[menu.n++] = MENU_SEND_MESSAGE;
    }
  if (!people->editing && !people->selecting && has_members)
    {
      labels[menu.n] = _("Remove contacts");
      menu.items[menu.n++] = MENU_REMOVE_CONTACTS;
    }
  if (!people->selecting)
    {
      labels[menu.n] = _("Rename label");
      menu.items[menu.n++] = MENU_RENAME;
      labels[menu.n] = _("Delete label");
      menu.items[menu.n++] = MENU_DELETE;
    }
  if (people->selecting && !people->editing)
    {
      labels[menu.n] = _("Remove from label");
      menu.items[menu.n++] = MENU_REMOVE_FROM_LABEL;
    }
  if (menu.n)
    ct_popup_show (GTK_WIDGET (button), labels, menu.n, CT_POPUP_OVER_END, 0, 0, 0, on_menu_chosen, &menu);
}

/* ===== the labels page: every label with its count, and "Create label" ========================================= */

typedef struct {
  GtkWidget *page, *window, *people, *list;
} LabelsPage;

static void labels_page_fill (LabelsPage *lp);

static void
on_labels_page_row (VibeosListRow *row, LabelsPage *lp)
{
  g_autofree char *label = g_strdup (g_object_get_data (G_OBJECT (row), "ct-label"));
  GtkWidget *people = lp->people;

  ct_window_pop (lp->window, lp->page);
  ct_window_select_tab (lp->window, 0);
  ct_people_show_label (people, label);
}

static void
on_label_created (const char *text, gpointer data)
{
  LabelsPage *lp = data;
  People *people = people_of (lp->people);

  if (!text)
    return;
  ct_store_create_label (people->store, text);
  ct_window_toast (lp->window, _("Label created"));
  labels_page_fill (lp);
}

static void
on_labels_page_create (VibeosListRow *row, LabelsPage *lp)
{
  ct_text_dialog (lp->page, _("Create label"), NULL, _("Label name"), 40, label_exists, on_label_created,
                  people_of (lp->people));
  /* the text dialog's callback takes the People: Create label adds it, the page refills on the store's change */
}

static void
labels_page_fill (LabelsPage *lp)
{
  People *people = people_of (lp->people);
  g_autoptr(GPtrArray) labels = ct_store_dup_labels (people->store);
  GtkWidget *row;

  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (lp->list));
  for (guint i = 0; i < labels->len; i++)
    {
      g_autoptr(GPtrArray) members = ct_store_label_members (people->store, labels->pdata[i]);
      g_autofree char *count = ct_contacts_count (members->len, NULL);

      row = vibeos_list_row_new (labels->pdata[i], count);
      vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "ct-label-symbolic");
      g_object_set_data_full (G_OBJECT (row), "ct-label", g_strdup (labels->pdata[i]), g_free);
      g_signal_connect (row, "clicked", G_CALLBACK (on_labels_page_row), lp);
      vibeos_card_list_append (VIBEOS_CARD_LIST (lp->list), row);
    }
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (lp->list), NULL);
  row = vibeos_list_row_new (_("Create label"), NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "ct-add-symbolic");
  g_signal_connect (row, "clicked", G_CALLBACK (on_labels_page_create), lp);
  vibeos_card_list_append (VIBEOS_CARD_LIST (lp->list), row);
}

static void
on_labels_store_changed (CtStore *store, LabelsPage *lp)
{
  labels_page_fill (lp);
}

static void
on_labels_page_back (VibeosTopBar *bar, LabelsPage *lp)
{
  ct_page_back (CT_PAGE (lp->page));
}

static void
labels_page_free (gpointer data)
{
  LabelsPage *lp = data;
  People *people = people_of (lp->people);

  g_signal_handlers_disconnect_by_data (people->store, lp);
  g_free (lp);
}

void
ct_people_show_labels (GtkWidget *root)
{
  People *people = people_of (root);
  LabelsPage *lp = g_new0 (LabelsPage, 1);
  GtkWidget *bar = vibeos_top_bar_new (_("Labels"));
  GtkWidget *scroll = spongy_scroll_view_new ();

  lp->window = people->window;
  lp->people = root;
  lp->page = ct_page_new (people->window);
  gtk_widget_add_css_class (lp->page, "ct-labels");
  g_object_set_data_full (G_OBJECT (lp->page), "ct-labels", lp, labels_page_free);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_labels_page_back), lp);
  gtk_box_append (GTK_BOX (lp->page), bar);
  lp->list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (lp->list), 8, 24);
  /* on a tablet the labels are a column in the middle */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (lp->list, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (lp->page), scroll);
  labels_page_fill (lp);
  g_signal_connect (people->store, "changed", G_CALLBACK (on_labels_store_changed), lp);
  ct_window_push (people->window, lp->page);
}

/* ===== the tab ================================================================================================= */

void
ct_people_show_label (GtkWidget *root, const char *label)
{
  show_view (people_of (root), VIEW_LABEL, label);
}

void
ct_people_show_account (GtkWidget *root, const char *book_uid)
{
  People *people = people_of (root);

  if (people->view != VIEW_ALL)
    show_view (people, VIEW_ALL, NULL);
  show_account (people, book_uid);
}

gboolean
ct_people_back (GtkWidget *root)
{
  People *people = people_of (root);

  if (people->selecting)
    stop_selection (people);
  else if (people->view != VIEW_ALL)
    show_view (people, VIEW_ALL, NULL);
  else if (people->filters || people->filters_shown)
    {
      people->filters = 0;
      people->filters_shown = FALSE;
      update_head (people);
      refresh (people, TRUE);
    }
  else if (people->account)
    show_account (people, NULL);
  else
    return FALSE;
  return TRUE;
}

void
ct_people_reset (GtkWidget *root)
{
  People *people = people_of (root);

  if (people->searching)
    vibeos_search_view_close (VIBEOS_SEARCH_VIEW (people->search_view));
  while (ct_people_back (root))
    ;
}

void
ct_people_set_fab_shown (GtkWidget *root, gboolean shown)
{
  People *people = people_of (root);

  people->fab_wanted = shown;
  update_fab (people);
  /* the list's bottom keeps the last row clear of the FAB */
  ct_list_set_bottom_padding (CT_LIST (people->list), shown ? CT_LIST_PAD_BOTTOM_FAB_DP : CT_LIST_PAD_BOTTOM_DP);
}

void
ct_people_set_current (GtkWidget *root, const char *uid)
{
  ct_list_set_current (CT_LIST (people_of (root)->list), uid);
}

static void
on_fab (VibeosPressable *fab, People *people)
{
  ct_window_new_contact (people->window, NULL);
}

static void
on_theme_changed (VibeosTheme *theme, People *people)
{
  GdkRGBA bar = vibeos_color (VIBEOS_SURFACE_CONTAINER);

  ct_fab_update (people->fab);
  vibeos_top_bar_set_fill (VIBEOS_TOP_BAR (people->selection_bar), &bar);
  update_head (people);
}

static void
people_free (gpointer data)
{
  People *people = data;

  g_signal_handlers_disconnect_by_data (people->store, people);
  g_signal_handlers_disconnect_by_data (people->settings, people);
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), people);
  g_clear_pointer (&people->fab_anim, vibeos_anim_free);
  g_clear_pointer (&people->selected, g_hash_table_unref);
  if (people->search_view)
    {
      g_signal_handlers_disconnect_by_data (people->search_view, people);
      g_object_unref (people->search_view);
    }
  forget_undo (people);
  g_free (people->label);
  g_free (people->account);
  g_object_unref (people->store);
  g_object_unref (people->settings);
  g_free (people);
}

GtkWidget *
ct_people_new (GtkWidget *window)
{
  People *people = g_new0 (People, 1);
  GtkWidget *root = gtk_overlay_new ();
  GtkWidget *empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkEventController *keys = gtk_event_controller_key_new ();

  people->root = root;
  people->window = window;
  people->store = g_object_ref (ct_window_get_store (window));
  people->settings = g_object_ref (ct_window_get_settings (window));
  people->selected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  people->fab_wanted = TRUE;
  g_object_set_data_full (G_OBJECT (root), "ct-people", people, people_free);
  gtk_widget_add_css_class (root, "ct-people");

  people->list = ct_list_new ();
  people->head = make_head (people);
  ct_list_set_head (CT_LIST (people->list), people->head);
  ct_list_set_bottom_padding (CT_LIST (people->list), CT_LIST_PAD_BOTTOM_FAB_DP);
  g_signal_connect (people->list, "row-activated", G_CALLBACK (on_row_activated), people);
  g_signal_connect (people->list, "row-long-pressed", G_CALLBACK (on_row_long_pressed), people);
  gtk_overlay_set_child (GTK_OVERLAY (root), people->list);

  /* the empty states: a line and a button, in the middle */
  gtk_widget_add_css_class (empty, "ct-empty");
  people->empty_text = ct_text ("", "ct-empty-text");
  gtk_label_set_xalign (GTK_LABEL (people->empty_text), 0.5);
  gtk_label_set_wrap (GTK_LABEL (people->empty_text), TRUE);
  gtk_widget_set_halign (people->empty_text, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (empty), people->empty_text);
  people->empty_button = vibeos_text_button_new (_("Create contact"));
  gtk_widget_set_halign (people->empty_button, GTK_ALIGN_CENTER);
  g_signal_connect (people->empty_button, "clicked", G_CALLBACK (on_empty_button), people);
  gtk_box_append (GTK_BOX (empty), people->empty_button);
  gtk_widget_set_halign (empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (empty, FALSE);
  people->empty = empty;
  gtk_overlay_add_overlay (GTK_OVERLAY (root), empty);

  /* the FAB: the plus in the bottom end corner */
  people->fab = ct_fab_new ();
  gtk_widget_set_halign (people->fab, GTK_ALIGN_END);
  gtk_widget_set_valign (people->fab, GTK_ALIGN_END);
  gtk_widget_set_margin_end (people->fab, (int) CT_FAB_MARGIN_END_DP);
  gtk_widget_set_margin_bottom (people->fab, (int) CT_FAB_MARGIN_BOTTOM_DP);
  g_signal_connect (people->fab, "clicked", G_CALLBACK (on_fab), people);
  people->fab_anim = vibeos_anim_new (people->fab, on_fab_anim, people);
  people->fab_shown = TRUE;
  gtk_overlay_add_overlay (GTK_OVERLAY (root), people->fab);

  /* selection mode's bar, over the top of the list */
  people->selection_bar = vibeos_top_bar_new ("");
  {
    GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER);

    vibeos_top_bar_set_fill (VIBEOS_TOP_BAR (people->selection_bar), &fill);
  }
  people->share = vibeos_top_bar_add_selection_action (VIBEOS_TOP_BAR (people->selection_bar), "ct-share-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->share), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Share"), -1);
  g_signal_connect (people->share, "clicked", G_CALLBACK (on_share), people);
  people->delete = vibeos_top_bar_add_selection_action (VIBEOS_TOP_BAR (people->selection_bar), "ct-delete-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->delete), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Delete"), -1);
  g_signal_connect (people->delete, "clicked", G_CALLBACK (on_delete), people);
  people->selection_more = vibeos_top_bar_add_selection_action (VIBEOS_TOP_BAR (people->selection_bar),
                                                                "ct-more-vert-symbolic");
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->selection_more), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("More options"), -1);
  g_signal_connect (people->selection_more, "clicked", G_CALLBACK (on_label_more), people);
  g_signal_connect (people->selection_bar, "selection-closed", G_CALLBACK (on_selection_closed), people);
  gtk_widget_set_valign (people->selection_bar, GTK_ALIGN_START);
  gtk_widget_set_visible (people->selection_bar, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), people->selection_bar);

  /* the marker the back gesture sees while Back has something to leave */
  people->back_marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
  gtk_accessible_update_property (GTK_ACCESSIBLE (people->back_marker), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Contacts"), -1);
  gtk_widget_set_can_target (people->back_marker, FALSE);
  gtk_widget_set_visible (people->back_marker, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (root), people->back_marker);

  people->search_view = make_search (people);

  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), people);
  gtk_widget_add_controller (root, keys);
  g_signal_connect (people->store, "changed", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "books-changed", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "ready", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->store, "profile-changed", G_CALLBACK (on_store_changed), people);
  g_signal_connect (people->settings, "changed", G_CALLBACK (on_settings_changed), people);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), people);
  update_head (people);
  refresh (people, TRUE);
  return root;
}
