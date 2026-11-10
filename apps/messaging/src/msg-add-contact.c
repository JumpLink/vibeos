/* Adding someone to the contacts (msg-add-contact.h). */
#include "msg-add-contact.h"
#include "contacts-app.h"
#include "msg-avatar.h"
#include "msg-dialog.h"

#include <glib/gi18n.h>

/* add_contacts_confirmation_dialog_body.xml: 20 dp around; the 56 dp avatar; the name 12 dp after it, 24 dp before
 * the end (AddContactConfirmationTextStyle) */
#define BODY_PAD 20
#define AVATAR 56
#define NAME_PAD_START 12
#define NAME_PAD_END 24

gboolean
msg_contact_can_add (const MsgPerson *person)
{
  return person && !person->is_contact && person->destination && *person->destination && contacts_app_installed ();
}

static void
on_response (NeoDialog *dialog, int response, gpointer data)
{
  if (response == NEO_RESPONSE_OK)
    contacts_app_insert_or_edit (GTK_WIDGET (dialog), g_object_get_data (G_OBJECT (dialog), "destination"));
}

void
msg_contact_confirm_add (GtkWidget *from, const MsgPerson *person)
{
  NeoDialog *dialog = neo_dialog_new (_("Add to Contacts?"));
  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *name = gtk_label_new (person->destination);

  msg_avatar_set_person (MSG_AVATAR (avatar), person->name, person->identifier, person->photo);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (body), avatar);
  /* the destination as it is stored (getNormalizedDestination), on one line */
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (name), TRUE);
  gtk_widget_set_valign (name, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_set_margin_start (name, NAME_PAD_START);
  gtk_widget_set_margin_end (name, NAME_PAD_END);
  gtk_widget_add_css_class (name, "msg-add-contact-name");
  gtk_box_append (GTK_BOX (body), name);
  gtk_widget_set_margin_start (body, BODY_PAD);
  gtk_widget_set_margin_end (body, BODY_PAD);
  gtk_widget_set_margin_top (body, BODY_PAD);
  gtk_widget_set_margin_bottom (body, BODY_PAD);
  neo_dialog_set_content (dialog, body);
  msg_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Add Contact"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "destination", g_strdup (person->destination), g_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_response), NULL);
  neo_dialog_present (dialog, from);
}

void
msg_contact_show_or_add (GtkWidget *from, const MsgPerson *person, NeoSnackbar *snackbar)
{
  if (!person)
    return;
  if (person->is_contact)
    contacts_app_view (from, person->identifier, snackbar);
  else if (msg_contact_can_add (person))
    msg_contact_confirm_add (from, person);
}
