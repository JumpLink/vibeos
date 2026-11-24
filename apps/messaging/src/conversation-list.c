/* The conversation list (conversation-list.h). */
#include "conversation-list.h"
#include "blocked-numbers.h"
#include "msg-add-contact.h"
#include "msg-account.h"
#include "msg-action-bar.h"
#include "msg-avatar.h"
#include "msg-dates.h"
#include "msg-dialog.h"
#include "msg-people.h"
#include "msg-swipe.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define LIST_TOP_PADDING 8
#define LIST_BOTTOM_PADDING 96  /* the last row clear of the FAB */
#define AVATAR 52
#define AVATAR_X 16
#define TEXT_X 16               /* the text's inset after the avatar */
#define TIME_END 24             /* the time's inset from the end */
#define TIME_GAP 12             /* between the name or the snippet and the time's column */
#define ERROR_ICON 16
#define ERROR_GAP 2
#define ERROR_TOP 2             /* the error sign's inset from the snippet's top */
#define ACCOUNT 44              /* the account's picture in its 48 px button */
#define BAR_END 8               /* the last button's inset from the end */
#define FAB_MARGIN 24
#define GO_TOP 44
#define EMPTY_TEXT_TOP 20
#define EMPTY_TEXT_SIDE 60
#define EMPTY_TEXT_PAD 4         /* over and under the text's lines */
#define REFRESH_MS (60 * 1000)  /* "Just now" becomes "1 min" */

typedef struct {
  gint64 id;
  gboolean archived;
  char *destination;            /* the one other participant as stored, for Block; NULL in a group */
  MsgPerson *person;            /* that participant, for Add contact and their avatar; NULL in a group */
  GtkWidget *row, *swipe, *content, *avatar;
} Row;


struct _ConversationList {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  GSettings *settings;          /* vibeos.Messaging: Swipe to delete */
  gboolean archived;
  gboolean forward;             /* Forward message's list: no bars, no FAB, no swipe, no select */

  GtkWidget *box;               /* the page: bar over the content */
  GtkWidget *bar, *select_bar;
  GtkWidget *archive_action, *unarchive_action, *add_contact_action, *block_action;
  GtkWidget *scroll, *list, *empty, *fab, *go_top, *account;
  VibeosSnackbar *snackbar;
  GHashTable *rows;             /* id -> Row */
  GHashTable *selected;         /* ids */
  gint64 current;               /* the conversation open beside the list, or 0 */
  guint refresh_id;
  gboolean at_top;
};

G_DEFINE_FINAL_TYPE (ConversationList, conversation_list, GTK_TYPE_WIDGET)

enum { OPEN_CONVERSATION, START_CHAT, CLOSED, AT_TOP_CHANGED, SHOW_SEARCH, SHOW_ACCOUNT, N_SIGNALS };
static guint signals[N_SIGNALS];

static void reload (ConversationList *self);

static const char *
country (ConversationList *self)
{
  return sms_client_get_country (self->client);
}

/* the one conversation selected, when it is with one person who is not blocked yet: what Block acts on */
static Row *
blockable (ConversationList *self)
{
  GHashTableIter iter;
  gpointer key;
  Row *row;

  if (g_hash_table_size (self->selected) != 1)
    return NULL;
  g_hash_table_iter_init (&iter, self->selected);
  g_hash_table_iter_next (&iter, &key, NULL);
  row = g_hash_table_lookup (self->rows, key);
  if (!row || !row->destination
      || blocked_numbers_contains (blocked_numbers_get_default (), row->destination, country (self)))
    return NULL;
  return row;
}

/* the one conversation selected, when it is with one person who is not a contact: what Add contact acts on
 * (one selected, not a group, no lookup key) */
static Row *
addable (ConversationList *self)
{
  GHashTableIter iter;
  gpointer key;
  Row *row;

  if (g_hash_table_size (self->selected) != 1)
    return NULL;
  g_hash_table_iter_init (&iter, self->selected);
  g_hash_table_iter_next (&iter, &key, NULL);
  row = g_hash_table_lookup (self->rows, key);
  return row && msg_contact_can_add (row->person) ? row : NULL;
}

/* --- select mode */

static gboolean
selecting (ConversationList *self)
{
  return g_hash_table_size (self->selected) > 0;
}

static void
sync_select (ConversationList *self)
{
  gboolean any_archived = FALSE, any_unarchived = FALSE;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      Row *row = value;
      gboolean on = g_hash_table_contains (self->selected, key);

      msg_avatar_set_checked (MSG_AVATAR (row->avatar), on);
      if (on)
        gtk_widget_add_css_class (row->row, "selected");
      else
        gtk_widget_remove_css_class (row->row, "selected");
      /* no swiping while selecting, nor in the archived list or the forward one */
      msg_swipe_set_enabled (MSG_SWIPE (row->swipe), !self->archived && !self->forward && !selecting (self));
      if (on)
        {
          any_archived |= row->archived;
          any_unarchived |= !row->archived;
        }
    }
  gtk_widget_set_visible (self->select_bar, selecting (self));
  gtk_widget_set_visible (self->bar, !self->forward && !selecting (self));
  gtk_widget_set_visible (self->archive_action, any_unarchived);
  gtk_widget_set_visible (self->unarchive_action, any_archived);
  gtk_widget_set_visible (self->add_contact_action, addable (self) != NULL);
  gtk_widget_set_visible (self->block_action, blockable (self) != NULL);
  if (self->fab)
    gtk_widget_set_visible (self->fab, !selecting (self));
  {
    g_autofree char *count = g_strdup_printf ("%u", g_hash_table_size (self->selected));

    msg_action_bar_set_title (self->select_bar, count);
  }
  g_signal_emit (self, signals[AT_TOP_CHANGED], 0);
}

static void
toggle_selected (ConversationList *self, gint64 id)
{
  if (!g_hash_table_remove (self->selected, &id))
    g_hash_table_add (self->selected, g_memdup2 (&id, sizeof id));
  sync_select (self);
}

gboolean
conversation_list_back (ConversationList *self)
{
  if (!selecting (self))
    return FALSE;
  g_hash_table_remove_all (self->selected);
  sync_select (self);
  return TRUE;
}

/* the selected ids, and select mode left */
static GArray *
take_selected (ConversationList *self)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, self->selected);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_array_append_val (ids, *(gint64 *) key);
  g_hash_table_remove_all (self->selected);
  sync_select (self);
  return ids;
}

typedef struct {
  ConversationList *self;
  GArray *ids;
  gboolean archived;
} Undo;

static void
undo_free (gpointer data)
{
  Undo *undo = data;

  g_array_unref (undo->ids);
  g_object_unref (undo->self);
  g_free (undo);
}

static void
on_undo (gpointer data)
{
  Undo *undo = data;

  for (guint i = 0; i < undo->ids->len; i++)
    sms_client_set_archived (undo->self->client, g_array_index (undo->ids, gint64, i), !undo->archived);
}

/* archives or unarchives them, with the snackbar's Undo: "2 archived" */
static void
set_archived (ConversationList *self, GArray *ids, gboolean archived)
{
  Undo *undo = g_new0 (Undo, 1);
  g_autofree char *text = NULL;

  for (guint i = 0; i < ids->len; i++)
    sms_client_set_archived (self->client, g_array_index (ids, gint64, i), archived);
  text = g_strdup_printf (archived ? ngettext ("%d archived", "%d archived", ids->len)
                                   : ngettext ("%d unarchived", "%d unarchived", ids->len), ids->len);
  undo->self = g_object_ref (self);
  undo->ids = g_array_ref (ids);
  undo->archived = archived;
  /* the undo lives as long as the snackbar might call it */
  g_object_set_data_full (G_OBJECT (self->snackbar), "msg-undo", undo, undo_free);
  msg_snackbar_show (self->snackbar, text, _("Undo"), on_undo, undo);
}

static void
on_archive (ConversationList *self)
{
  g_autoptr(GArray) ids = take_selected (self);

  set_archived (self, ids, TRUE);
}

static void
on_unarchive (ConversationList *self)
{
  g_autoptr(GArray) ids = take_selected (self);

  set_archived (self, ids, FALSE);
}

static void
on_delete_response (VibeosDialog *dialog, int response, gpointer data)
{
  ConversationList *self = data;
  GArray *ids = g_object_get_data (G_OBJECT (dialog), "ids");

  if (response == VIBEOS_RESPONSE_OK)
    for (guint i = 0; i < ids->len; i++)
      sms_client_delete_conversation (self->client, g_array_index (ids, gint64, i));
}

static void
on_delete (ConversationList *self)
{
  GArray *ids = take_selected (self);
  VibeosDialog *dialog = vibeos_dialog_new (ngettext ("Delete this conversation?", "Delete these conversations?",
                                                ids->len));

  g_object_set_data_full (G_OBJECT (dialog), "ids", ids, (GDestroyNotify) g_array_unref);
  msg_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), self);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* Block: the number goes on the blocked list (the Phone's too) and the conversation is archived; the service then
 * keeps what it sends without a notification. Undo takes both back. */

typedef struct {
  ConversationList *self;
  gint64 id;
  char *destination;
} Block;

static void
block_free (gpointer data)
{
  Block *block = data;

  g_object_unref (block->self);
  g_free (block->destination);
  g_free (block);
}

static void
on_block_undo (gpointer data)
{
  Block *block = data;
  g_autoptr(GError) error = NULL;

  if (!blocked_numbers_remove (blocked_numbers_get_default (), block->destination, country (block->self), &error))
    {
      g_warning ("messaging: %s", error->message);
      return;
    }
  sms_client_set_archived (block->self->client, block->id, FALSE);
  msg_snackbar_show (block->self->snackbar, _("Contact unblocked & unarchived"), NULL, NULL, NULL);
}

static void
on_block_response (VibeosDialog *dialog, int response, gpointer data)
{
  ConversationList *self = data;
  Block *block = g_object_steal_data (G_OBJECT (dialog), "block");
  g_autoptr(GError) error = NULL;

  if (response != VIBEOS_RESPONSE_OK)
    {
      block_free (block);
      return;
    }
  if (!blocked_numbers_add (blocked_numbers_get_default (), block->destination, country (self), &error))
    {
      g_warning ("messaging: %s", error->message);
      block_free (block);
      return;
    }
  sms_client_set_archived (self->client, block->id, TRUE);
  /* the undo lives as long as the snackbar might call it */
  g_object_set_data_full (G_OBJECT (self->snackbar), "msg-undo", block, block_free);
  msg_snackbar_show (self->snackbar, _("Contact blocked & archived"), _("Undo"), on_block_undo, block);
}

static void
on_block (ConversationList *self)
{
  Row *row = blockable (self);
  Block *block;
  g_autofree char *title = NULL;
  VibeosDialog *dialog;

  if (!row)
    return;
  block = g_new0 (Block, 1);
  block->self = g_object_ref (self);
  block->id = row->id;
  block->destination = g_strdup (row->destination);
  g_array_unref (take_selected (self));
  /* the title names the number as stored, not the contact */
  title = g_strdup_printf (_("Block %s?"), block->destination);
  dialog = vibeos_dialog_new (title);
  vibeos_dialog_set_message (dialog, _("You'll continue to receive messages from this number but won't be notified "
                                    "anymore. This conversation will be archived."));
  msg_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "block", block);
  g_signal_connect (dialog, "response", G_CALLBACK (on_block_response), self);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* Add contact: "Add to Contacts?" for the person, and select mode left */
static void
on_add_contact (ConversationList *self)
{
  Row *row = addable (self);

  if (!row)
    return;
  msg_contact_confirm_add (GTK_WIDGET (self), row->person);
  g_array_unref (take_selected (self));
}

/* --- rows */

static void
on_row_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");

  if (selecting (self))
    toggle_selected (self, id);
  else
    {
      /* the row that opens a conversation shows its press highlight first (msg-window.c) */
      g_object_set_data (G_OBJECT (self), "msg-pressed-row", pressable);
      g_signal_emit (self, signals[OPEN_CONVERSATION], 0, id);
      g_object_set_data (G_OBJECT (self), "msg-pressed-row", NULL);
    }
}

GtkWidget *
conversation_list_get_pressed_row (ConversationList *self)
{
  return g_object_get_data (G_OBJECT (self), "msg-pressed-row");
}

static void
on_row_long_pressed (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");

  /* in Forward message's list a long press is a tap */
  if (self->forward)
    g_signal_emit (self, signals[OPEN_CONVERSATION], 0, id);
  else
    toggle_selected (self, id);
}

/* the avatar: while selecting, a tap on it is the row's; else the person's card, or "Add to
 * Contacts?"; a group's takes the tap and does nothing. A long press is the row's. */
static void
on_avatar_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationList *self = g_object_get_data (G_OBJECT (pressable), "list");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");
  Row *row = g_hash_table_lookup (self->rows, &id);

  if (selecting (self))
    toggle_selected (self, id);
  else if (row && row->person)
    msg_contact_show_or_add (pressable, row->person, self->snackbar);
}

static GtkWidget *
text_label (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css);
  return label;
}

static gboolean
is_failed (const SmsConversation *conversation)
{
  return conversation->latest_status == MESSAGE_STATUS_OUTGOING_FAILED
         || conversation->latest_status == MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER;
}

/* where the time goes: "Draft", "Sending…", or when */
static char *
row_time (const SmsConversation *conversation, gint64 now)
{
  if (conversation->show_draft)
    return g_strdup (_("Draft"));
  switch (conversation->latest_status)
    {
    case MESSAGE_STATUS_OUTGOING_YET_TO_SEND:
    case MESSAGE_STATUS_OUTGOING_SENDING:
    case MESSAGE_STATUS_OUTGOING_RESENDING:
    case MESSAGE_STATUS_OUTGOING_AWAITING_RETRY:
      return g_strdup (_("Sending…"));
    default:
      return msg_dates_conversation (conversation->sort_timestamp, now);
    }
}

/* the newest message's words, "You: " before one of ours */
static char *
row_snippet (const SmsConversation *conversation)
{
  const char *text = conversation->snippet ? conversation->snippet : "";

  if (conversation->show_draft)
    return g_strdup (conversation->draft ? conversation->draft : "");
  if (conversation->latest_status != MESSAGE_STATUS_UNKNOWN && !MESSAGE_STATUS_IS_INCOMING (conversation->latest_status))
    return g_strdup_printf (_("You: %s"), text);
  return g_strdup (text);
}

/* archived, with Undo; or with "Swipe to delete" on, a swipe to the right deletes it, with no Undo and no question */
static void
on_swiped (GtkWidget *swipe, gboolean right, gpointer data)
{
  ConversationList *self = data;
  g_autoptr(GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (swipe), "id");

  if (right && g_settings_get_boolean (self->settings, "swipe-right-deletes"))
    {
      sms_client_delete_conversation (self->client, id);
      msg_snackbar_show (self->snackbar, _("Conversation deleted"), NULL, NULL, NULL);
      return;
    }
  g_array_append_val (ids, id);
  set_archived (self, ids, TRUE);
}

static void row_free (gpointer data);

static GtkWidget *
make_row (ConversationList *self, const SmsConversation *conversation, gint64 now)
{
  g_autoptr(GPtrArray) people = msg_people_new (self->contacts, (const char * const *) conversation->destinations,
                                                sms_client_get_country (self->client));
  g_autofree char *name = msg_people_name (people);
  g_autofree char *time = row_time (conversation, now);
  g_autofree char *snippet_text = row_snippet (conversation);
  gboolean unread = !conversation->read;
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (AVATAR);
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, ERROR_GAP);
  GtkWidget *name_label, *snippet, *time_label, *pressable, *avatar_pressable;
  Row *row = g_new0 (Row, 1);

  /* the avatar: the person's, or the group's (up to four) */
  if (people->len == 1)
    {
      MsgPerson *person = people->pdata[0];

      msg_avatar_set_person (MSG_AVATAR (avatar), person->name, person->identifier, person->photo);
    }
  else
    {
      g_autoptr(GPtrArray) names = g_ptr_array_new (), ids = g_ptr_array_new (), photos = g_ptr_array_new ();

      for (guint i = 0; i < people->len; i++)
        {
          MsgPerson *person = people->pdata[i];

          g_ptr_array_add (names, person->name);
          g_ptr_array_add (ids, person->identifier);
          g_ptr_array_add (photos, person->photo);
        }
      g_ptr_array_add (names, NULL);
      msg_avatar_set_group (MSG_AVATAR (avatar), (const char * const *) names->pdata,
                            (const char * const *) ids->pdata, (GBytes * const *) photos->pdata);
    }
  avatar_pressable = vibeos_pressable_new (avatar);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (avatar_pressable), VIBEOS_RIPPLE_NONE, 0);
  gtk_widget_set_valign (avatar_pressable, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (avatar_pressable, AVATAR_X);
  gtk_box_append (GTK_BOX (content), avatar_pressable);

  /* the name and the time over the snippet; the time's column as wide as the time */
  name_label = text_label (name, "msg-row-name");
  time_label = text_label (time, "msg-row-time");
  snippet = text_label (snippet_text, "msg-row-snippet");
  gtk_widget_set_hexpand (name_label, TRUE);
  gtk_widget_set_hexpand (snippet, TRUE);
  gtk_widget_set_valign (time_label, GTK_ALIGN_START);
  if (unread)
    {
      gtk_widget_add_css_class (name_label, "unread");
      gtk_widget_add_css_class (snippet, "unread");
      gtk_widget_add_css_class (time_label, "unread");
    }
  if (conversation->show_draft)
    gtk_widget_add_css_class (time_label, "draft");
  /* a send that failed: the error sign before the snippet */
  if (is_failed (conversation))
    {
      GtkWidget *error = gtk_image_new_from_icon_name ("msg-error-filled-symbolic");

      gtk_image_set_pixel_size (GTK_IMAGE (error), ERROR_ICON);
      /* level with the snippet's first line's letters, a little over its middle */
      gtk_widget_set_valign (error, GTK_ALIGN_START);
      gtk_widget_set_margin_top (error, ERROR_TOP);
      gtk_widget_add_css_class (error, "msg-row-error");
      gtk_box_append (GTK_BOX (line), error);
    }
  gtk_box_append (GTK_BOX (line), snippet);
  gtk_grid_set_column_spacing (GTK_GRID (grid), TIME_GAP);
  gtk_grid_attach (GTK_GRID (grid), name_label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), time_label, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), line, 0, 1, 1, 1);
  gtk_widget_set_hexpand (grid, TRUE);
  gtk_widget_set_valign (grid, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (grid, TEXT_X);
  gtk_widget_set_margin_end (grid, TIME_END);
  gtk_box_append (GTK_BOX (content), grid);
  gtk_widget_add_css_class (content, "msg-row-content");

  pressable = vibeos_pressable_new (content);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (pressable), VIBEOS_RIPPLE_BOUNDED, 16);
  gtk_widget_add_css_class (pressable, "msg-row");
  if (conversation->id == self->current)
    gtk_widget_add_css_class (pressable, "current");
  gtk_widget_set_name (pressable, "conversation_row");
  row->id = conversation->id;
  row->archived = conversation->archived;
  if (g_strv_length (conversation->destinations) == 1)
    row->destination = g_strdup (conversation->destinations[0]);
  if (people->len == 1)
    row->person = msg_person_copy (people->pdata[0]);
  row->row = pressable;
  row->content = content;
  row->avatar = avatar;
  g_object_set_data (G_OBJECT (pressable), "list", self);
  g_object_set_data_full (G_OBJECT (pressable), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_row_clicked), NULL);
  g_signal_connect (pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), NULL);
  g_object_set_data (G_OBJECT (avatar_pressable), "list", self);
  g_object_set_data_full (G_OBJECT (avatar_pressable), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (avatar_pressable, "clicked", G_CALLBACK (on_avatar_clicked), NULL);
  g_signal_connect (avatar_pressable, "long-pressed", G_CALLBACK (on_row_long_pressed), NULL);
  g_hash_table_insert (self->rows, g_memdup2 (&row->id, sizeof row->id), row);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  row->swipe = msg_swipe_new (pressable,
                              g_settings_get_boolean (self->settings, "swipe-right-deletes")
                              ? "msg-delete-symbolic" : "msg-archive-symbolic",
                              "msg-archive-symbolic");
  g_object_set_data_full (G_OBJECT (row->swipe), "id", g_memdup2 (&row->id, sizeof row->id), g_free);
  g_signal_connect (row->swipe, "swiped", G_CALLBACK (on_swiped), self);
  return row->swipe;
}

static void
row_free (gpointer data)
{
  Row *row = data;

  g_free (row->destination);
  msg_person_free (row->person);
  g_free (row);
}

static void
reload (ConversationList *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(GPtrArray) conversations = store ? sms_store_list_conversations (store, self->archived) : NULL;
  gint64 now = g_get_real_time () / 1000;
  GtkWidget *child;
  GHashTableIter iter;
  gpointer key;

  while ((child = gtk_widget_get_first_child (self->list)))
    gtk_box_remove (GTK_BOX (self->list), child);
  g_hash_table_remove_all (self->rows);
  for (guint i = 0; conversations && i < conversations->len; i++)
    gtk_box_append (GTK_BOX (self->list), make_row (self, conversations->pdata[i], now));
  /* what was selected and is gone stays unselected */
  g_hash_table_iter_init (&iter, self->selected);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (self->rows, key))
      g_hash_table_iter_remove (&iter);
  gtk_widget_set_visible (self->empty, g_hash_table_size (self->rows) == 0);
  gtk_widget_set_visible (self->scroll, g_hash_table_size (self->rows) > 0);
  sync_select (self);
}

static void
on_changed (SmsClient *client, gint64 conversation, gpointer data)
{
  reload (data);
}

static gboolean
on_refresh (gpointer data)
{
  reload (data);
  return G_SOURCE_CONTINUE;
}

void
conversation_list_set_current (ConversationList *self, gint64 id)
{
  GHashTableIter iter;
  gpointer value;

  self->current = id;
  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      Row *row = value;

      if (row->id == id)
        gtk_widget_add_css_class (row->row, "current");
      else
        gtk_widget_remove_css_class (row->row, "current");
    }
}

/* --- the page */

guint
conversation_list_get_n_conversations (ConversationList *self)
{
  return g_hash_table_size (self->rows);
}

gboolean
conversation_list_is_at_top (ConversationList *self)
{
  return !selecting (self) && self->at_top;
}

/* away from the top: the FAB down to its icon, and the button back to the newest */
static void
on_scrolled (GObject *scroll, GParamSpec *pspec, gpointer data)
{
  ConversationList *self = data;
  gboolean at_top = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (scroll)) <= 0;

  if (at_top == self->at_top)
    return;
  self->at_top = at_top;
  if (self->fab)
    vibeos_extended_fab_set_extended (VIBEOS_EXTENDED_FAB (self->fab), at_top);
  if (self->go_top)
    gtk_widget_set_visible (self->go_top, !at_top);
  g_signal_emit (self, signals[AT_TOP_CHANGED], 0);
}

static void
on_go_top (ConversationList *self)
{
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), 0);
}

static void
on_search (ConversationList *self)
{
  g_signal_emit (self, signals[SHOW_SEARCH], 0);
}

static void
on_account (ConversationList *self)
{
  g_signal_emit (self, signals[SHOW_ACCOUNT], 0);
}

static void
update_account (ConversationList *self)
{
  MsgAccount *account = msg_account_get_default ();
  GtkWidget *avatar = vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->account));

  msg_avatar_set_person (MSG_AVATAR (avatar), msg_account_get_name (account), msg_account_get_login (account),
                         msg_account_get_picture (account));
}

/* the FAB and the button rise over the snackbar while it shows */
static void
on_snackbar_lift (GObject *snackbar, GParamSpec *pspec, gpointer data)
{
  ConversationList *self = data;
  int lift = (int) round (vibeos_snackbar_get_lift (VIBEOS_SNACKBAR (snackbar)));

  gtk_widget_set_margin_bottom (self->fab, FAB_MARGIN + lift);
  gtk_widget_set_margin_bottom (self->go_top, FAB_MARGIN + lift);
}

static void
on_fab (ConversationList *self)
{
  g_signal_emit (self, signals[START_CHAT], 0);
}

static void
on_up (ConversationList *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static GtkWidget *
make_go_top (void)
{
  GtkWidget *icon = gtk_image_new_from_icon_name ("msg-arrow-upward-symbolic");
  GtkWidget *button;
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  button = vibeos_pressable_new (icon);
  gtk_widget_add_css_class (button, "msg-go-top");
  gtk_widget_set_name (button, "go_to_top");
  gtk_widget_set_size_request (button, GO_TOP, GO_TOP);
  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (button), &fill, TRUE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_CIRCLE, GO_TOP / 2.0);
  gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (button, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (button, FAB_MARGIN);
  gtk_widget_set_visible (button, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("View latest"), -1);
  return button;
}

/* the button's circle in the theme's colour of now */
static void
on_theme_changed (VibeosTheme *theme, ConversationList *self)
{
  GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);

  if (self->go_top)
    vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->go_top), &fill, TRUE);
}

static GtkWidget *
make_empty (gboolean archived)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *text = gtk_label_new (archived ? _("Archived conversations appear here")
                                            : _("Once you start a new conversation, you’ll see it listed here"));

  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (text, EMPTY_TEXT_TOP + EMPTY_TEXT_PAD);
  gtk_widget_set_margin_bottom (text, EMPTY_TEXT_PAD);
  gtk_widget_set_margin_start (text, EMPTY_TEXT_SIDE);
  gtk_widget_set_margin_end (text, EMPTY_TEXT_SIDE);
  gtk_widget_add_css_class (text, "msg-empty-text");
  gtk_box_append (GTK_BOX (box), text);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (box, TRUE);
  return box;
}

static void
conversation_list_dispose (GObject *object)
{
  ConversationList *self = MSG_CONVERSATION_LIST (object);

  if (self->refresh_id)
    g_source_remove (self->refresh_id);
  self->refresh_id = 0;
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_signal_handlers_disconnect_by_data (blocked_numbers_get_default (), self);
  g_signal_handlers_disconnect_by_data (msg_account_get_default (), self);
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_clear_pointer (&self->selected, g_hash_table_unref);
  G_OBJECT_CLASS (conversation_list_parent_class)->dispose (object);
}

static void
conversation_list_class_init (ConversationListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = conversation_list_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-conversation-list");
  signals[OPEN_CONVERSATION] = g_signal_new ("open-conversation", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[START_CHAT] = g_signal_new ("start-chat", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                      NULL, G_TYPE_NONE, 0);
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[AT_TOP_CHANGED] = g_signal_new ("at-top-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                          NULL, NULL, G_TYPE_NONE, 0);
  signals[SHOW_SEARCH] = g_signal_new ("show-search", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 0);
  signals[SHOW_ACCOUNT] = g_signal_new ("show-account", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
}

static void
conversation_list_init (ConversationList *self)
{
  self->rows = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, row_free);
  self->selected = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  self->at_top = TRUE;
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

/* the bar's search button and the account's picture */
static void
add_home_buttons (ConversationList *self)
{
  GtkWidget *end = msg_action_bar_get_end (self->bar);
  GtkWidget *search = msg_action_bar_add_action (self->bar, "msg-search-symbolic", _("Search"));
  GtkWidget *avatar = msg_avatar_new (ACCOUNT);

  g_signal_connect_swapped (search, "clicked", G_CALLBACK (on_search), self);
  gtk_widget_set_halign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  self->account = vibeos_pressable_new (avatar);
  gtk_widget_set_size_request (self->account, 48, 48);
  gtk_widget_set_valign (self->account, GTK_ALIGN_CENTER);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->account), VIBEOS_RIPPLE_CIRCLE, 24);
  gtk_widget_set_name (self->account, "account_button");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->account), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Account and settings"), -1);
  g_signal_connect_swapped (self->account, "clicked", G_CALLBACK (on_account), self);
  gtk_box_append (GTK_BOX (end), self->account);
  gtk_widget_set_margin_end (end, BAR_END);
  g_signal_connect_swapped (msg_account_get_default (), "changed", G_CALLBACK (update_account), self);
  update_account (self);
}

static GtkWidget *
build (SmsClient *client, Contacts *contacts, gboolean archived, gboolean forward)
{
  ConversationList *self = g_object_new (MSG_TYPE_CONVERSATION_LIST, NULL);
  GtkWidget *content = gtk_overlay_new ();
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sheet = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *cancel;

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->settings = g_settings_new ("vibeos.Messaging");
  self->archived = archived;
  self->forward = forward;
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, forward ? "msg-forward-page" : "msg-list-page");

  /* "Messages", with search and the account; the archived list: "Archived" with the back arrow */
  self->bar = msg_action_bar_new (archived, archived ? _("Archived") : _("Messages"));
  gtk_widget_add_css_class (self->bar, "msg-list-bar");
  if (archived)
    g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  else if (!forward)
    add_home_buttons (self);
  gtk_box_append (GTK_BOX (self->box), self->bar);
  /* select mode: the ✕, how many, Archive or Unarchive, Delete, Add contact and Block for one person */
  self->select_bar = msg_action_bar_new (FALSE, "");
  gtk_widget_add_css_class (self->select_bar, "msg-list-bar");
  cancel = msg_action_bar_add_close (self->select_bar, _("Done"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (conversation_list_back), self);
  self->archive_action = msg_action_bar_add_action (self->select_bar, "msg-archive-symbolic", _("Archive"));
  self->unarchive_action = msg_action_bar_add_action (self->select_bar, "msg-unarchive-symbolic", _("Unarchive"));
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "msg-delete-symbolic", _("Delete")),
                            "clicked", G_CALLBACK (on_delete), self);
  self->add_contact_action = msg_action_bar_add_action (self->select_bar, "messaging-people-add-symbolic",
                                                        _("Add contact"));
  g_signal_connect_swapped (self->add_contact_action, "clicked", G_CALLBACK (on_add_contact), self);
  self->block_action = msg_action_bar_add_action (self->select_bar, "msg-block-symbolic", _("Block"));
  g_signal_connect_swapped (self->archive_action, "clicked", G_CALLBACK (on_archive), self);
  g_signal_connect_swapped (self->unarchive_action, "clicked", G_CALLBACK (on_unarchive), self);
  g_signal_connect_swapped (self->block_action, "clicked", G_CALLBACK (on_block), self);
  /* blocked here or in the Phone: Block goes away for that person */
  g_signal_connect_swapped (blocked_numbers_get_default (), "changed", G_CALLBACK (sync_select), self);
  gtk_widget_set_visible (self->select_bar, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->select_bar);

  /* the list on its sheet, the sheet's top corners round */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name (self->list, "conversation_list");
  gtk_widget_set_margin_top (self->list, LIST_TOP_PADDING);
  gtk_widget_set_margin_bottom (self->list, forward ? 0 : LIST_BOTTOM_PADDING);
  gtk_box_append (GTK_BOX (inner), self->list);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), inner);
  gtk_widget_set_vexpand (self->scroll, TRUE);
  g_signal_connect (self->scroll, "notify::offset", G_CALLBACK (on_scrolled), self);
  self->empty = make_empty (archived);
  gtk_widget_add_css_class (sheet, "msg-list-sheet");
  gtk_widget_set_overflow (sheet, GTK_OVERFLOW_HIDDEN);
  gtk_box_append (GTK_BOX (sheet), self->scroll);
  gtk_box_append (GTK_BOX (sheet), self->empty);
  gtk_overlay_set_child (GTK_OVERLAY (content), sheet);
  if (!archived && !forward)
    {
      self->fab = vibeos_extended_fab_new ("msg-chat-symbolic", _("Start chat"));
      gtk_widget_set_name (self->fab, "start_chat_fab");
      gtk_widget_set_margin_end (self->fab, FAB_MARGIN);
      gtk_widget_set_margin_bottom (self->fab, FAB_MARGIN);
      g_signal_connect_swapped (self->fab, "clicked", G_CALLBACK (on_fab), self);
      gtk_overlay_add_overlay (GTK_OVERLAY (content), self->fab);
      self->go_top = make_go_top ();
      g_signal_connect_swapped (self->go_top, "clicked", G_CALLBACK (on_go_top), self);
      gtk_overlay_add_overlay (GTK_OVERLAY (content), self->go_top);
      g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), self);
    }
  self->snackbar = msg_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (content), GTK_WIDGET (self->snackbar));
  if (self->fab)
    g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_snackbar_lift), self);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (self->box), content);
  if (forward)
    gtk_widget_set_visible (self->bar, FALSE);

  g_signal_connect (client, "changed", G_CALLBACK (on_changed), self);
  /* the network's country and the address book come in after the window opens: numbers and names as they then read
   * (before the country, a number is formatted for the locale's, and its avatar colour changes with it) */
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  g_signal_connect_swapped (self->contacts, "changed", G_CALLBACK (reload), self);
  self->refresh_id = g_timeout_add (REFRESH_MS, on_refresh, self);
  reload (self);
  return GTK_WIDGET (self);
}

GtkWidget *
conversation_list_new (SmsClient *client, Contacts *contacts, gboolean archived)
{
  return build (client, contacts, archived, FALSE);
}

GtkWidget *
conversation_list_new_forward (SmsClient *client, Contacts *contacts)
{
  return build (client, contacts, FALSE, TRUE);
}

/* Mark all as read: every conversation not read yet, in the list and archived */
void
conversation_list_mark_all_read (SmsClient *client)
{
  SmsStore *store = sms_client_get_store (client);

  for (int archived = 0; store && archived < 2; archived++)
    {
      g_autoptr (GPtrArray) conversations = sms_store_list_conversations (store, archived);

      for (guint i = 0; i < conversations->len; i++)
        {
          SmsConversation *conversation = conversations->pdata[i];

          if (!conversation->read)
            sms_client_mark_read (client, conversation->id);
        }
    }
}
