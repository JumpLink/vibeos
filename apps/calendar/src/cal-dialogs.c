/* The calendar's small dialogs (cal-dialogs.h). */
#include "cal-dialogs.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

typedef struct {
  CalChoiceDone done;
  gpointer data;
  int chosen;
  GPtrArray *radios;            /* the radio rows' marks, for cal_radio_dialog */
  GtkWidget *ok;
} Choice;

static void
choice_free (gpointer data, GClosure *closure)
{
  Choice *choice = data;

  g_clear_pointer (&choice->radios, g_ptr_array_unref);
  g_free (choice);
}

static void
on_response (NeoDialog *dialog, int response, Choice *choice)
{
  if (response == NEO_RESPONSE_CANCEL)
    choice->done (-1, choice->data);
  else if (response == NEO_RESPONSE_OK)
    choice->done (choice->chosen, choice->data);
  else
    choice->done (response - 100, choice->data);
}

static void
on_item (NeoPressable *row, NeoDialog *dialog)
{
  /* a row answers with its index, offset past the standard responses */
  neo_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-index")));
}

static GtkWidget *
item_row (const char *label, int index, GtkWidget **mark)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *row;

  if (mark)
    {
      *mark = gtk_image_new_from_icon_name ("cal-radio-off-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (*mark), 24);
      gtk_widget_add_css_class (*mark, "cal-radio");
      gtk_box_append (GTK_BOX (box), *mark);
    }
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_box_append (GTK_BOX (box), text);
  row = neo_pressable_new (box);
  gtk_widget_add_css_class (row, "cal-dialog-item");
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  g_object_set_data (G_OBJECT (row), "cal-index", GINT_TO_POINTER (index));
  return row;
}

void
cal_choice_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                   CalChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  Choice *choice = g_new0 (Choice, 1);

  choice->done = done;
  choice->data = data;
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *row = item_row (labels[i], i, NULL);

      g_signal_connect (row, "clicked", G_CALLBACK (on_item), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "cal-dialog-list");
  neo_dialog_set_content (dialog, list);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), choice, choice_free, 0);
  neo_dialog_present (dialog, parent);
}

static void
on_radio (NeoPressable *row, Choice *choice)
{
  choice->chosen = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-index"));
  for (guint i = 0; i < choice->radios->len; i++)
    gtk_image_set_from_icon_name (choice->radios->pdata[i], (int) i == choice->chosen ? "cal-radio-on-symbolic"
                                                                                       : "cal-radio-off-symbolic");
  gtk_widget_set_sensitive (choice->ok, TRUE);
}

void
cal_radio_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels,
                  CalChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  Choice *choice = g_new0 (Choice, 1);

  choice->done = done;
  choice->data = data;
  choice->chosen = -1;
  choice->radios = g_ptr_array_new ();
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *mark;
      GtkWidget *row = item_row (labels[i], i, &mark);

      g_ptr_array_add (choice->radios, mark);
      g_signal_connect (row, "clicked", G_CALLBACK (on_radio), choice);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "cal-dialog-list");
  neo_dialog_set_content (dialog, list);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  /* OK waits for a choice (the dialog's positive button disabled until a row is checked) */
  choice->ok = neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  gtk_widget_set_sensitive (choice->ok, FALSE);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), choice, choice_free, 0);
  neo_dialog_present (dialog, parent);
}

void
cal_list_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                 CalChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();
  Choice *choice = g_new0 (Choice, 1);

  choice->done = done;
  choice->data = data;
  for (int i = 0; i < n_labels; i++)
    {
      GtkWidget *mark;
      GtkWidget *row = item_row (labels[i], i, &mark);

      if (i == selected)
        gtk_image_set_from_icon_name (GTK_IMAGE (mark), "cal-radio-on-symbolic");
      g_signal_connect (row, "clicked", G_CALLBACK (on_item), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "cal-dialog-list");
  /* a long list (the reminder times) scrolls inside the dialog */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), list);
  gtk_widget_add_css_class (scroll, "cal-dialog-scroll");
  neo_dialog_set_content (dialog, scroll);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), choice, choice_free, 0);
  neo_dialog_present (dialog, parent);
}

void
cal_confirm_dialog (GtkWidget *parent, const char *message, const char *ok, CalChoiceDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (NULL);
  Choice *choice = g_new0 (Choice, 1);

  choice->done = done;
  choice->data = data;
  choice->chosen = 0;
  neo_dialog_set_message (dialog, message);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, ok, NEO_RESPONSE_OK);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_response), choice, choice_free, 0);
  neo_dialog_present (dialog, parent);
}

typedef struct {
  CalTextDone done;
  gpointer data;
  GtkWidget *entry;
} TextAsk;

static void
text_ask_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

static void
on_text_response (NeoDialog *dialog, int response, TextAsk *ask)
{
  ask->done (response == NEO_RESPONSE_OK ? gtk_editable_get_text (GTK_EDITABLE (ask->entry)) : NULL, ask->data);
}

static void
on_text_activate (GtkEntry *entry, NeoDialog *dialog)
{
  neo_dialog_respond (dialog, NEO_RESPONSE_OK);
}

void
cal_text_dialog (GtkWidget *parent, const char *title, const char *text, CalTextDone done, gpointer data)
{
  NeoDialog *dialog = neo_dialog_new (title);
  TextAsk *ask = g_new0 (TextAsk, 1);

  ask->done = done;
  ask->data = data;
  /* preference_dialog_edittext: the field under the title, 24 dp in, its line in colorControlActivated */
  ask->entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (ask->entry), text ? text : "");
  gtk_widget_add_css_class (ask->entry, "cal-dialog-entry");
  g_signal_connect (ask->entry, "activate", G_CALLBACK (on_text_activate), dialog);
  neo_dialog_set_content (dialog, ask->entry);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("OK"), NEO_RESPONSE_OK);
  g_signal_connect_data (dialog, "response", G_CALLBACK (on_text_response), ask, text_ask_free, 0);
  neo_dialog_present (dialog, parent);
  gtk_widget_grab_focus (ask->entry);
}
