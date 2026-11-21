/* The contact picker (contact-picker.h). */
#include "contact-picker.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-card.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define PILL_SIDE 12            /* the To pill's and the cards' margin from the sides */
#define PILL_H 48
#define TO_X 20                 /* "To:" in the pill */
#define FIELD_GAP 18            /* from "To:" to the field */
#define FIELD_END 16
#define LIST_TOP 8
#define HEADER_H 40
#define HEADER_X 2
#define ROW_H 72
#define AVATAR 40
#define AVATAR_X 16             /* in the card */
#define TEXT_GAP 16             /* from the avatar to the text */
#define TEXT_END 16
#define FREQUENTS 8             /* how many of the most written to come first */

struct _ContactPicker {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;

  GtkWidget *box, *bar, *field, *list, *results, *pages, *empty;
};

G_DEFINE_FINAL_TYPE (ContactPicker, contact_picker, GTK_TYPE_WIDGET)

enum { PICKED, CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
country (ContactPicker *self)
{
  return sms_client_get_country (self->client);
}

static void
on_row_clicked (GtkWidget *row, gpointer data)
{
  ContactPicker *self = data;

  g_signal_emit (self, signals[PICKED], 0, g_object_get_data (G_OBJECT (row), "number"));
}

static GtkWidget *
text_label (const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css_class);
  return label;
}

/* one of a contact's numbers, on its card: the avatar, the name, the number and its type. @contact NULL: a typed
 * number, the avatar and the number only */
static GtkWidget *
make_row (ContactPicker *self, const Contact *contact, const char *number, const char *label)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autofree char *formatted = phone_number_format (number, country (self));
  g_autofree char *detail = label ? g_strdup_printf (C_("number, type", "%s • %s"), formatted, label)
                                  : g_strdup (formatted);
  GtkWidget *row;

  if (contact)
    msg_avatar_set_person (MSG_AVATAR (avatar), contact->name_primary, contact->uid, contact->photo);
  else
    msg_avatar_set_person (MSG_AVATAR (avatar), NULL, number, NULL);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (avatar, AVATAR_X);
  gtk_box_append (GTK_BOX (box), avatar);
  if (contact)
    gtk_box_append (GTK_BOX (column), text_label (contact->name, "msg-picker-name"));
  gtk_box_append (GTK_BOX (column), text_label (detail, contact ? "msg-picker-number" : "msg-picker-name"));
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_margin_start (column, TEXT_GAP);
  gtk_widget_set_margin_end (column, TEXT_END);
  gtk_box_append (GTK_BOX (box), column);
  gtk_widget_set_size_request (box, -1, ROW_H);

  row = msg_card_row_new (box);
  gtk_widget_set_name (row, "contact_row");
  g_object_set_data_full (G_OBJECT (row), "number", g_strdup (number), g_free);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  contact ? contact->name : formatted, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_row_clicked), self);
  return row;
}

static void
clear (GtkWidget *list)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (list)))
    gtk_box_remove (GTK_BOX (list), child);
}

/* a header over a group of cards, flush with the list's start */
static GtkWidget *
add_group (GtkWidget *list, const char *header)
{
  GtkWidget *group = msg_card_group_new ();

  if (header)
    {
      GtkWidget *label = text_label (header, "msg-picker-header");

      gtk_widget_set_size_request (label, -1, HEADER_H);
      gtk_widget_set_margin_start (label, HEADER_X);
      gtk_box_append (GTK_BOX (list), label);
    }
  gtk_widget_set_margin_start (group, PILL_SIDE);
  gtk_widget_set_margin_end (group, PILL_SIDE);
  gtk_box_append (GTK_BOX (list), group);
  return group;
}

/* a contact's numbers, one card each */
static void
append_contact (ContactPicker *self, GtkWidget *group, const Contact *contact, const char *only_matching)
{
  for (guint i = 0; i < contact->numbers->len; i++)
    {
      ContactNumber *number = contact->numbers->pdata[i];

      if (only_matching)
        {
          g_autofree char *digits = phone_number_normalize (number->number);

          if (!strstr (digits, only_matching))
            continue;
        }
      msg_card_group_append (group, make_row (self, contact, number->number, number->label));
    }
}

/* --- the lists */

static char *
first_letter (const Contact *contact)
{
  const char *name = contact->sort_name ? contact->sort_name : contact->name;
  gunichar c = name ? g_unichar_toupper (g_utf8_get_char (name)) : 0;
  char letter[8] = "";

  /* the address book index: a letter, else "#" */
  if (!g_unichar_isalpha (c))
    return g_strdup ("#");
  letter[g_unichar_to_utf8 (c, letter)] = '\0';
  return g_strdup (letter);
}

/* the contacts written to most, counted from the messages, then every contact under its letter */
static void
fill_all (ContactPicker *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_auto(GStrv) frequent = store ? sms_store_list_frequent (store, FREQUENTS * 2) : NULL;
  GPtrArray *contacts = contacts_get_all (self->contacts);
  g_autofree char *previous = NULL;
  GtkWidget *group = NULL;
  guint shown = 0;

  clear (self->list);
  for (guint i = 0; frequent && frequent[i] && shown < FREQUENTS; i++)
    {
      const Contact *contact = contacts_lookup (self->contacts, frequent[i], country (self));

      if (!contact)
        continue;
      if (!group)
        group = add_group (self->list, _("Top contacts"));
      msg_card_group_append (group, make_row (self, contact, frequent[i],
                                              contact_number_label (contact, frequent[i], country (self))));
      shown++;
    }
  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      const Contact *contact = contacts->pdata[i];
      g_autofree char *letter = first_letter (contact);

      if (!contact->numbers->len)
        continue;
      if (g_strcmp0 (letter, previous) != 0)
        group = add_group (self->list, letter);
      append_contact (self, group, contact, NULL);
      g_free (previous);
      previous = g_steal_pointer (&letter);
    }
  gtk_widget_set_visible (self->empty, !gtk_widget_get_first_child (self->list));
}

/* the folded words of @text: lower case, no accents */
static char *
fold (const char *text)
{
  g_autofree char *normal = g_utf8_normalize (text, -1, G_NORMALIZE_NFD);
  g_autoptr(GString) folded = g_string_new (NULL);

  for (const char *p = normal; p && *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_ismark (c))
        continue;
      g_string_append_unichar (folded, g_unichar_tolower (c));
    }
  return g_string_free (g_steal_pointer (&folded), FALSE);
}

/* the contacts provider's filter: each word of the query starts a word of the name */
static gboolean
name_matches (const char *name, char **query_words)
{
  g_autofree char *folded = fold (name);
  g_auto(GStrv) words = g_strsplit_set (folded, " -.,", -1);

  for (guint q = 0; query_words[q]; q++)
    {
      gboolean found = FALSE;

      if (!*query_words[q])
        continue;
      for (guint w = 0; words[w] && !found; w++)
        found = g_str_has_prefix (words[w], query_words[q]);
      if (!found)
        return FALSE;
    }
  return TRUE;
}

static gboolean
looks_like_number (const char *text)
{
  guint digits = 0;

  for (const char *p = text; *p; p++)
    if (g_ascii_isdigit (*p))
      digits++;
    else if (!strchr ("+ ()-./", *p))
      return FALSE;
  return digits > 0;
}

static void
fill_results (ContactPicker *self, const char *query)
{
  GPtrArray *contacts = contacts_get_all (self->contacts);
  g_autofree char *folded = fold (query);
  g_auto(GStrv) words = g_strsplit (g_strstrip (folded), " ", -1);
  gboolean numeric = looks_like_number (query);
  g_autofree char *digits = numeric ? phone_number_normalize (query) : NULL;
  GtkWidget *group;

  clear (self->results);
  group = add_group (self->results, NULL);
  /* the number typed, picked as it is */
  if (numeric)
    msg_card_group_append (group, make_row (self, NULL, digits, NULL));
  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      const Contact *contact = contacts->pdata[i];

      if (contact->name && name_matches (contact->name, words))
        append_contact (self, group, contact, NULL);
      else if (numeric)
        append_contact (self, group, contact, digits);
    }
}

/* --- the field */

static void
on_field_changed (GtkEditable *field, gpointer data)
{
  ContactPicker *self = data;
  const char *text = gtk_editable_get_text (field);
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  gtk_stack_set_visible_child_name (GTK_STACK (self->pages), *stripped ? "results" : "all");
  if (*stripped)
    fill_results (self, stripped);
}

/* Enter: the typed number, or the first match */
static void
on_field_activate (GtkText *field, gpointer data)
{
  ContactPicker *self = data;
  GtkWidget *group = gtk_widget_get_first_child (self->results);
  GtkWidget *first = group ? gtk_widget_get_first_child (group) : NULL;

  if (first && g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (self->pages)), "results") == 0)
    g_signal_emit (self, signals[PICKED], 0, g_object_get_data (G_OBJECT (first), "number"));
}

static void
on_back (ContactPicker *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
on_contacts_changed (Contacts *contacts, gpointer data)
{
  ContactPicker *self = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->field));
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  fill_all (self);
  if (*stripped)
    fill_results (self, stripped);
}

/* --- the page */

static GtkWidget *
scrolled (GtkWidget *list)
{
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  gtk_widget_set_margin_top (list, LIST_TOP);
  gtk_widget_set_margin_bottom (list, LIST_TOP);
  gtk_box_append (GTK_BOX (inner), list);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), inner);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

static GtkWidget *
make_empty (void)
{
  GtkWidget *text = gtk_label_new (_("Type a name or a phone number to start a chat"));

  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (text, 60);
  gtk_widget_set_margin_end (text, 60);
  gtk_widget_add_css_class (text, "msg-empty-text");
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (text, TRUE);
  gtk_widget_set_can_target (text, FALSE);
  return text;
}

/* "To:" and the field, in a pill */
static GtkWidget *
make_to (ContactPicker *self)
{
  GtkWidget *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *to = gtk_label_new (_("To:"));

  gtk_widget_add_css_class (to, "msg-picker-to");
  gtk_widget_set_margin_start (to, TO_X);
  gtk_box_append (GTK_BOX (pill), to);
  self->field = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (self->field), _("Type name, phone number, or email"));
  gtk_text_set_input_hints (GTK_TEXT (self->field), GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_valign (self->field, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->field, TRUE);
  gtk_widget_set_margin_start (self->field, FIELD_GAP);
  gtk_widget_set_margin_end (self->field, FIELD_END);
  gtk_widget_add_css_class (self->field, "msg-picker-field");
  gtk_widget_set_name (self->field, "ContactSearchField");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->field), GTK_ACCESSIBLE_PROPERTY_LABEL, _("To"), -1);
  g_signal_connect (self->field, "changed", G_CALLBACK (on_field_changed), self);
  g_signal_connect (self->field, "activate", G_CALLBACK (on_field_activate), self);
  gtk_box_append (GTK_BOX (pill), self->field);
  gtk_widget_add_css_class (pill, "msg-picker-pill");
  gtk_widget_set_size_request (pill, -1, PILL_H);
  gtk_widget_set_margin_start (pill, PILL_SIDE);
  gtk_widget_set_margin_end (pill, PILL_SIDE);
  return pill;
}

static void
contact_picker_dispose (GObject *object)
{
  ContactPicker *self = MSG_CONTACT_PICKER (object);

  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (contact_picker_parent_class)->dispose (object);
}

static gboolean
contact_picker_grab_focus (GtkWidget *widget)
{
  return gtk_widget_grab_focus (MSG_CONTACT_PICKER (widget)->field);
}

static void
contact_picker_class_init (ContactPickerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = contact_picker_dispose;
  widget_class->grab_focus = contact_picker_grab_focus;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-picker");
  signals[PICKED] = g_signal_new ("picked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
contact_picker_init (ContactPicker *self)
{
}

GtkWidget *
contact_picker_new (SmsClient *client, Contacts *contacts)
{
  ContactPicker *self = g_object_new (MSG_TYPE_CONTACT_PICKER, NULL);
  GtkWidget *all_page = gtk_overlay_new ();
  GtkWidget *content;

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-picker-page");

  self->bar = msg_action_bar_new (TRUE, _("New chat"));
  gtk_widget_add_css_class (self->bar, "msg-page-bar");
  g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_back), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);
  /* To: and the contacts under the bar: beside the list on a tablet they are a pane of their own (messaging.css) */
  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (content, "msg-picker-content");
  gtk_widget_set_overflow (content, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (self->box), content);
  gtk_box_append (GTK_BOX (content), make_to (self));

  self->pages = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->pages), GTK_STACK_TRANSITION_TYPE_NONE);
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name (self->list, "contact_list");
  gtk_overlay_set_child (GTK_OVERLAY (all_page), scrolled (self->list));
  self->empty = make_empty ();
  gtk_overlay_add_overlay (GTK_OVERLAY (all_page), self->empty);
  gtk_stack_add_named (GTK_STACK (self->pages), all_page, "all");
  self->results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (self->pages), scrolled (self->results), "results");
  gtk_widget_set_vexpand (self->pages, TRUE);
  gtk_box_append (GTK_BOX (content), self->pages);

  g_signal_connect (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  fill_all (self);
  return GTK_WIDGET (self);
}
