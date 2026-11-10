/* The Settings list dialog: see clock-dialogs.h. */
#include "clock-dialogs.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef struct
{
  ClockChoiceDone done;
  gpointer data;
} Choice;

static void
choice_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

static void
on_response (NeoDialog *dialog, int response, Choice *choice)
{
  /* a row answers with its index past the standard responses; anything else is a cancel */
  choice->done (response >= 100 ? response - 100 : -1, choice->data);
}

static void
on_item (NeoPressable *row, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "clock-index")));
}

/* select_dialog_singlechoice_material: 48 dp tall, 20 dp in; the radio (listChoiceIndicatorSingle, its 20 dp ring
 * in a 32 dp drawable), 20 dp, then the text, 24 dp from the end (measured on still:camera-settings-list-dialog, the
 * same AlertDialog list in another LineageOS app: ring at x 153…212 px, text from 292, from a card edge at 75) */
static GtkWidget *
item_row (const char *label, int index, gboolean checked)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *mark = gtk_image_new_from_icon_name (checked ? "clock-radio-on-symbolic" : "clock-radio-off-symbolic");
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *row;

  gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
  gtk_widget_set_size_request (mark, 32, 32);
  gtk_widget_add_css_class (mark, "clock-radio");
  if (checked)
    gtk_widget_add_css_class (mark, "checked");
  gtk_box_append (GTK_BOX (box), mark);
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (box), text);
  row = neo_pressable_new (box);
  gtk_widget_add_css_class (row, "clock-dialog-item");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_state (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_STATE_CHECKED,
                               checked ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
  g_object_set_data (G_OBJECT (row), "clock-index", GINT_TO_POINTER (index));
  return row;
}

void
clock_list_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                   ClockChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();
  Choice *choice = g_new0 (Choice, 1);

  choice->done = done;
  choice->data = data;
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *row = item_row (labels[i], i, i == selected);

      g_signal_connect (row, "clicked", G_CALLBACK (on_item), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "clock-dialog-list");
  /* a long list (the snooze lengths, the time zones) scrolls inside the dialog */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_add_css_class (scroll, "clock-dialog-scroll");
  neo_dialog_set_content (dialog, scroll);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), choice, choice_free, 0);
  neo_dialog_present (dialog, parent);
}
