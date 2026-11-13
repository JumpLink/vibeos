/* The contact picker (contact-picker.h). */
#include "contact-picker.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-image.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

/* a contact's row, and the tabs */
#define ROW_PAD_VERTICAL 8
#define ROW_PAD_START 16
#define HEADER_WIDTH 56         /* the alphabet header's column (All contacts) */
#define AVATAR 42
#define TEXT_PAD_START 12
#define TEXT_PAD_END 24
#define NAME_PAD_BOTTOM 4
#define LABEL_PAD_START 12
#define TAB_HEIGHT 56
#define FREQUENTS 20            /* how many Frequents shows */

struct _ContactPicker {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;

  GtkWidget *box, *bar, *field, *dialpad_action, *clear_action;
  GtkWidget *tabs, *tab_labels[2], *underlines[2];
  GtkWidget *pages, *frequents, *frequents_empty, *all, *results, *results_page;
  guint tab;
  gboolean phone_input;
};

G_DEFINE_FINAL_TYPE (ContactPicker, contact_picker, GTK_TYPE_WIDGET)

enum { PICKED, CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
country (ContactPicker *self)
{
  return sms_client_get_country (self->client);
}

/* --- rows */

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

/* one of a contact's numbers: with the avatar and the name on its first, only the number on the others. @letter:
 * the All contacts list's header column, the letter shown on a letter's first contact ("" for the others), or NULL
 * for a list without the column. @contact NULL: a typed number, the avatar and the number only */
static GtkWidget *
make_row (ContactPicker *self, const Contact *contact, const char *number, const char *label, gboolean first,
          const char *letter)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  g_autofree char *formatted = phone_number_format (number, country (self));
  GtkWidget *detail, *row;

  if (letter)
    {
      GtkWidget *header = gtk_label_new (letter);

      gtk_widget_set_size_request (header, HEADER_WIDTH, -1);
      gtk_widget_set_valign (header, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (header, "msg-picker-letter");
      gtk_box_append (GTK_BOX (box), header);
    }
  if (contact)
    msg_avatar_set_person (MSG_AVATAR (avatar), contact->name_primary, contact->uid, contact->photo);
  else
    msg_avatar_set_person (MSG_AVATAR (avatar), NULL, number, NULL);
  /* a contact's other numbers: its avatar's place kept, empty */
  gtk_widget_set_opacity (avatar, first ? 1 : 0);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), avatar);

  if (contact && first)
    {
      GtkWidget *name = text_label (contact->name, "msg-picker-name");

      gtk_widget_set_margin_bottom (name, NAME_PAD_BOTTOM);
      gtk_box_append (GTK_BOX (column), name);
    }
  /* the number takes the width, so the type sits at the row's end */
  detail = text_label (formatted, "msg-picker-number");
  gtk_widget_set_hexpand (detail, TRUE);
  gtk_box_append (GTK_BOX (line), detail);
  if (label)
    {
      GtkWidget *type = text_label (label, "msg-picker-type");

      gtk_widget_set_margin_start (type, LABEL_PAD_START);
      gtk_box_append (GTK_BOX (line), type);
    }
  gtk_box_append (GTK_BOX (column), line);
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_margin_start (column, TEXT_PAD_START);
  gtk_widget_set_margin_end (column, TEXT_PAD_END);
  gtk_box_append (GTK_BOX (box), column);
  gtk_widget_set_margin_start (box, ROW_PAD_START);
  gtk_widget_set_margin_top (box, ROW_PAD_VERTICAL);
  gtk_widget_set_margin_bottom (box, ROW_PAD_VERTICAL);

  row = neo_pressable_new (box);
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

/* a contact's numbers, one row each */
static void
append_contact (ContactPicker *self, GtkWidget *list, const Contact *contact, const char *letter,
                const char *only_matching)
{
  gboolean first = TRUE;

  for (guint i = 0; i < contact->numbers->len; i++)
    {
      ContactNumber *number = contact->numbers->pdata[i];

      if (only_matching)
        {
          g_autofree char *digits = phone_number_normalize (number->number);

          if (!strstr (digits, only_matching))
            continue;
        }
      gtk_box_append (GTK_BOX (list), make_row (self, contact, number->number, number->label, first,
                                                letter ? (first ? letter : "") : NULL));
      first = FALSE;
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

static void
fill_all (ContactPicker *self)
{
  GPtrArray *contacts = contacts_get_all (self->contacts);
  g_autofree char *previous = NULL;

  clear (self->all);
  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      const Contact *contact = contacts->pdata[i];
      g_autofree char *letter = first_letter (contact);
      gboolean new_letter = g_strcmp0 (letter, previous) != 0;

      append_contact (self, self->all, contact, new_letter ? letter : "", NULL);
      g_free (previous);
      previous = g_steal_pointer (&letter);
    }
}

/* the contacts written with most, counted from the messages */
static void
fill_frequents (ContactPicker *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_auto(GStrv) frequent = store ? sms_store_list_frequent (store, FREQUENTS * 2) : NULL;
  guint shown = 0;

  clear (self->frequents);
  for (guint i = 0; frequent && frequent[i] && shown < FREQUENTS; i++)
    {
      const Contact *contact = contacts_lookup (self->contacts, frequent[i], country (self));

      if (!contact)
        continue;
      gtk_box_append (GTK_BOX (self->frequents),
                      make_row (self, contact, frequent[i], contact_number_label (contact, frequent[i], country (self)),
                                TRUE, NULL));
      shown++;
    }
  gtk_widget_set_visible (self->frequents_empty, shown == 0);
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

  clear (self->results);
  /* the number typed, picked as it is */
  if (numeric)
    gtk_box_append (GTK_BOX (self->results), make_row (self, NULL, digits, NULL, TRUE, NULL));
  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      const Contact *contact = contacts->pdata[i];

      if (contact->name && name_matches (contact->name, words))
        append_contact (self, self->results, contact, NULL, NULL);
      else if (numeric)
        append_contact (self, self->results, contact, NULL, digits);
    }
}

/* --- the tabs */

static void
select_tab (ContactPicker *self, guint tab)
{
  self->tab = tab;
  for (guint i = 0; i < 2; i++)
    {
      if (i == tab)
        gtk_widget_add_css_class (self->tab_labels[i], "selected");
      else
        gtk_widget_remove_css_class (self->tab_labels[i], "selected");
      gtk_widget_set_visible (self->underlines[i], i == tab);
    }
  gtk_stack_set_visible_child_name (GTK_STACK (self->pages), tab == 0 ? "frequents" : "all");
}

static void
on_tab_clicked (GtkWidget *tab, gpointer data)
{
  ContactPicker *self = data;

  select_tab (self, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (tab), "tab")));
}

/* the two tabs, the selected one's underline under it (the strip's indicator, which follows the pager as it is
 * swiped; the pages here only change by a tap) */
static GtkWidget *
make_tabs (ContactPicker *self)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  const char *titles[] = { N_("FREQUENTS"), N_("ALL CONTACTS") };

  gtk_box_set_homogeneous (GTK_BOX (row), TRUE);
  for (guint i = 0; i < 2; i++)
    {
      GtkWidget *label = gtk_label_new (_(titles[i]));
      GtkWidget *cell = gtk_overlay_new ();
      GtkWidget *tab;
      GdkRGBA ripple;

      gtk_widget_add_css_class (label, "msg-picker-tab");
      gtk_widget_set_margin_start (label, 10);
      gtk_widget_set_margin_end (label, 10);
      self->tab_labels[i] = label;
      gtk_overlay_set_child (GTK_OVERLAY (cell), label);
      self->underlines[i] = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_size_request (self->underlines[i], -1, 2);
      gtk_widget_set_valign (self->underlines[i], GTK_ALIGN_END);
      gtk_widget_add_css_class (self->underlines[i], "msg-picker-underline");
      gtk_overlay_add_overlay (GTK_OVERLAY (cell), self->underlines[i]);
      tab = neo_pressable_new (cell);
      gtk_widget_set_size_request (tab, -1, TAB_HEIGHT);
      gdk_rgba_parse (&ripple, "#ddffffff");
      ripple.alpha = 0.24;
      neo_pressable_set_ripple_color (NEO_PRESSABLE (tab), &ripple);
      g_object_set_data (G_OBJECT (tab), "tab", GUINT_TO_POINTER (i));
      gtk_accessible_update_property (GTK_ACCESSIBLE (tab), GTK_ACCESSIBLE_PROPERTY_LABEL, _(titles[i]), -1);
      g_signal_connect (tab, "clicked", G_CALLBACK (on_tab_clicked), self);
      gtk_box_append (GTK_BOX (row), tab);
    }
  gtk_widget_add_css_class (row, "msg-picker-tabs");
  return row;
}

/* --- the field */

static void
update_field_actions (ContactPicker *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->field));
  gboolean typed = *text != '\0';

  /* the dialpad toggle while it is empty, the X once there is text */
  gtk_widget_set_visible (self->dialpad_action, !typed);
  gtk_widget_set_visible (self->clear_action, typed);
}

static void
on_field_changed (GtkEditable *field, gpointer data)
{
  ContactPicker *self = data;
  const char *text = gtk_editable_get_text (field);
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  update_field_actions (self);
  gtk_widget_set_visible (self->tabs, !*stripped);
  gtk_stack_set_visible_child_name (GTK_STACK (self->pages), *stripped ? "results"
                                                            : self->tab == 0 ? "frequents" : "all");
  if (*stripped)
    fill_results (self, stripped);
}

/* Enter: the typed number, or the first match */
static void
on_field_activate (GtkText *field, gpointer data)
{
  ContactPicker *self = data;
  GtkWidget *first = gtk_widget_get_first_child (self->results);

  if (first && gtk_widget_get_visible (self->results_page))
    g_signal_emit (self, signals[PICKED], 0, g_object_get_data (G_OBJECT (first), "number"));
}

static void
on_dialpad_toggle (ContactPicker *self)
{
  /* the on-screen keyboard between letters and the phone pad */
  self->phone_input = !self->phone_input;
  gtk_text_set_input_purpose (GTK_TEXT (self->field), self->phone_input ? GTK_INPUT_PURPOSE_PHONE
                                                                        : GTK_INPUT_PURPOSE_FREE_FORM);
  gtk_widget_grab_focus (self->field);
}

static void
on_clear (ContactPicker *self)
{
  gtk_editable_set_text (GTK_EDITABLE (self->field), "");
  gtk_widget_grab_focus (self->field);
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

  fill_frequents (self);
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

  gtk_box_append (GTK_BOX (inner), list);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), inner);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

static GtkWidget *
make_frequents_empty (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* the picture at a third of its pixel size (msg-image.h), with the text under it as the conversation list's
   * empty view has it */
  GtkWidget *image = msg_image_new ("/neo/Messaging/images/ic_oobe_freq_list.png", 169, 104);
  GtkWidget *text = gtk_label_new (_("Enter a contact name or phone number to start a new message"));

  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), image);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  /* a gap under the picture, plus 4 over and under the text's lines, as the list's */
  gtk_widget_set_margin_top (text, 20 + 4);
  gtk_widget_set_margin_bottom (text, 4);
  gtk_widget_set_margin_start (text, 60);
  gtk_widget_set_margin_end (text, 60);
  gtk_widget_add_css_class (text, "msg-empty-text");
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (box, TRUE);
  gtk_widget_set_can_target (box, FALSE);
  return box;
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
  GtkWidget *frequents_page = gtk_overlay_new ();

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-page");

  /* the action bar: the back arrow, "To", the dialpad toggle or the X */
  self->bar = msg_action_bar_new (TRUE, "");
  g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_back), self);
  self->field = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (self->field), _("To"));
  gtk_text_set_input_hints (GTK_TEXT (self->field), GTK_INPUT_HINT_NO_SPELLCHECK);
  gtk_widget_set_valign (self->field, GTK_ALIGN_CENTER);
  /* the text lined up with the titles' inset: 56 for the arrow, 12 here and the field's own 4 */
  gtk_widget_set_margin_start (self->field, 12);
  gtk_widget_add_css_class (self->field, "msg-picker-field");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->field), GTK_ACCESSIBLE_PROPERTY_LABEL, _("To"), -1);
  g_signal_connect (self->field, "changed", G_CALLBACK (on_field_changed), self);
  g_signal_connect (self->field, "activate", G_CALLBACK (on_field_activate), self);
  msg_action_bar_set_custom (self->bar, self->field);
  self->dialpad_action = msg_action_bar_add_action (self->bar, "messaging-dialpad-symbolic",
                                                    _("Switch between entering text and numbers"));
  g_signal_connect_swapped (self->dialpad_action, "clicked", G_CALLBACK (on_dialpad_toggle), self);
  self->clear_action = msg_action_bar_add_action (self->bar, "messaging-cancel-symbolic", _("Clear text"));
  g_signal_connect_swapped (self->clear_action, "clicked", G_CALLBACK (on_clear), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);

  self->tabs = make_tabs (self);
  gtk_box_append (GTK_BOX (self->box), self->tabs);

  self->pages = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->pages), GTK_STACK_TRANSITION_TYPE_NONE);
  self->frequents = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_overlay_set_child (GTK_OVERLAY (frequents_page), scrolled (self->frequents));
  self->frequents_empty = make_frequents_empty ();
  gtk_overlay_add_overlay (GTK_OVERLAY (frequents_page), self->frequents_empty);
  gtk_stack_add_named (GTK_STACK (self->pages), frequents_page, "frequents");
  self->all = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named (GTK_STACK (self->pages), scrolled (self->all), "all");
  self->results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  self->results_page = scrolled (self->results);
  gtk_stack_add_named (GTK_STACK (self->pages), self->results_page, "results");
  gtk_widget_set_vexpand (self->pages, TRUE);
  gtk_box_append (GTK_BOX (self->box), self->pages);

  g_signal_connect (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  fill_frequents (self);
  fill_all (self);
  select_tab (self, 0);
  update_field_actions (self);
  return GTK_WIDGET (self);
}
