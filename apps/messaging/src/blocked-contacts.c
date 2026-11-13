/* Blocked contacts (blocked-contacts.h). */
#include "blocked-contacts.h"
#include "blocked-numbers.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-people.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* a blocked contact's row */
#define ROW_PADDING 16
#define AVATAR 42
#define TEXT_PADDING 16

struct _BlockedContacts {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  BlockedNumbers *blocked;

  GtkWidget *box, *list;
};

G_DEFINE_FINAL_TYPE (BlockedContacts, blocked_contacts, GTK_TYPE_WIDGET)

enum { CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
country (BlockedContacts *self)
{
  return sms_client_get_country (self->client);
}

static void
on_row_clicked (GtkWidget *row, gpointer data)
{
  BlockedContacts *self = data;
  g_autoptr(GError) error = NULL;

  /* the list's "changed" takes the row away */
  if (!blocked_numbers_remove (self->blocked, g_object_get_data (G_OBJECT (row), "number"), country (self), &error))
    g_warning ("messaging: %s", error->message);
}

static GtkWidget *
make_row (BlockedContacts *self, const char *number)
{
  g_autoptr(MsgPerson) person = msg_person_new (self->contacts, number, country (self));
  g_autofree char *caps = g_utf8_strup (_("Unblock"), -1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *name = gtk_label_new (person->name);
  GtkWidget *unblock = gtk_label_new (caps);
  GtkWidget *row;

  msg_avatar_set_person (MSG_AVATAR (avatar), person->name, person->identifier, person->photo);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), avatar);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (name, TRUE);
  /* both texts are padded all round: the padding, not a fixed height, makes the row (and its touch area) tall */
  gtk_widget_set_margin_start (name, TEXT_PADDING);
  gtk_widget_set_margin_end (name, TEXT_PADDING);
  gtk_widget_set_margin_top (name, TEXT_PADDING);
  gtk_widget_set_margin_bottom (name, TEXT_PADDING);
  gtk_widget_add_css_class (name, "msg-blocked-name");
  gtk_box_append (GTK_BOX (content), name);
  gtk_widget_add_css_class (unblock, "msg-blocked-unblock");
  gtk_widget_set_margin_start (unblock, TEXT_PADDING);
  gtk_widget_set_margin_end (unblock, TEXT_PADDING);
  gtk_widget_set_valign (unblock, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), unblock);
  gtk_widget_set_margin_start (content, ROW_PADDING);
  gtk_widget_set_margin_end (content, ROW_PADDING);
  gtk_widget_set_margin_top (content, ROW_PADDING);
  gtk_widget_set_margin_bottom (content, ROW_PADDING);

  row = neo_pressable_new (content);
  g_object_set_data_full (G_OBJECT (row), "number", g_strdup (number), g_free);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, person->name, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), self);
  return row;
}

/* every row again, on a change to the list (from here or another app), the contacts, or the network's country */
static void
reload (BlockedContacts *self)
{
  g_auto(GStrv) numbers = blocked_numbers_list (self->blocked);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  for (guint i = 0; numbers[i]; i++)
    gtk_box_append (GTK_BOX (self->list), make_row (self, numbers[i]));
}

static void
on_up (BlockedContacts *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
blocked_contacts_dispose (GObject *object)
{
  BlockedContacts *self = MSG_BLOCKED_CONTACTS (object);

  if (self->blocked)
    g_signal_handlers_disconnect_by_data (self->blocked, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  self->blocked = NULL;
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (blocked_contacts_parent_class)->dispose (object);
}

static void
blocked_contacts_class_init (BlockedContactsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = blocked_contacts_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-blocked-contacts");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
blocked_contacts_init (BlockedContacts *self)
{
}

GtkWidget *
blocked_contacts_new (SmsClient *client, Contacts *contacts)
{
  BlockedContacts *self = g_object_new (MSG_TYPE_BLOCKED_CONTACTS, NULL);
  GtkWidget *bar = msg_action_bar_new (TRUE, _("Blocked contacts"));
  GtkWidget *scroll = spongy_scroll_view_new ();

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->blocked = blocked_numbers_get_default ();
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");
  g_signal_connect_swapped (msg_action_bar_get_up (bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_append (GTK_BOX (self->box), bar);

  /* a list with no dividers */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), self->list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (self->box), scroll);

  g_signal_connect_swapped (self->blocked, "changed", G_CALLBACK (reload), self);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (reload), self);
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  reload (self);
  return GTK_WIDGET (self);
}
