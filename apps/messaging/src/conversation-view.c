/* A conversation (conversation-view.h). */
#include "conversation-view.h"
#include "app-action.h"
#include "blocked-numbers.h"
#include "msg-action-bar.h"
#include "msg-add-contact.h"
#include "msg-avatar.h"
#include "msg-bubble.h"
#include "msg-dates.h"
#include "msg-dialog.h"
#include "msg-people.h"
#include "msg-sound.h"
#include "msg-thread.h"
#include "phone-number.h"
#include "popup-menu.h"
#include "sms-segments.h"

#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>

/* the top bar */
#define BAR_AVATAR 40
#define BAR_AVATAR_GAP 8        /* from the back arrow to the avatar */
#define BAR_AVATAR_ALONE 16     /* and from the bar's start, with no back arrow */
#define BAR_TITLE_GAP 12        /* from the avatar to the name */
#define MENU_CORNER 20
/* the messages */
#define SIDE 8                  /* the bubbles' inset from the container's sides */
#define BUBBLE_PAD_X 16
#define BUBBLE_PAD_Y 8
#define BUBBLE_MARGIN 70        /* a bubble leaves at least this much of the width free */
#define BUBBLE_MAX 480          /* and is never wider than this */
#define SEPARATOR_H 32
#define SEPARATOR_BEFORE 24     /* from a message to the date line under it */
#define SEPARATOR_AFTER 8       /* from the date line to its first message */
#define META_TOP 4              /* a failed message's line under it */
#define META_END 16             /* that line's inset from the bubble's end */
#define META_ICON 12
#define META_GAP 4
#define LIST_TOP 8
#define LIST_BOTTOM 8
#define REVEAL_DRAW_MS 80       /* the messages drawn this long after the bar */
/* the compose bar */
#define COMPOSE_PAD 8
#define COMPOSE_H 56
#define PLUS_W 48
#define SEND 56
#define SEND_GAP 8
#define FIELD_MAX_LINES 6
/* A window this short is a phone on its side with the keyboard up (about 130 dp of 332): the bar and the compose bar
 * have to fit in it, so the bar is a dense one and the field stays one line high (it scrolls). The bar stays: its
 * back arrow is what the back gesture presses */
#define SHORT_HEIGHT 240
#define SHORT_BAR 48
#define SHORT_FIELD_LINES 1
#define COUNTER_THRESHOLD 10    /* characters left when the counter shows */
#define REVEAL_MS 200           /* the send button coming or going */
#define NOTE_H 96               /* "can't reply": its band */
#define NOTE_SIDE 30
#define REFRESH_MS (60 * 1000)

struct _ConversationView {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  gint64 id;
  SmsConversation *conversation;
  GPtrArray *people;            /* MsgPerson */

  GtkWidget *box, *bar, *select_bar, *call_action, *title_avatar, *title_row;
  GtkWidget *forward_action, *copy_action, *resend_action;
  GtkWidget *container, *messages_area, *scroll, *messages;
  GtkWidget *compose, *note, *field, *field_scroll, *hint, *send, *counter;
  GPtrArray *bubbles;           /* MsgBubble, for their widths */
  int max_width;
  VibeosAnim *reveal;
  VibeosSnackbar *snackbar;
  gint64 selected;              /* the message selected, or 0 */
  gint64 newest;                /* the newest message shown, for "New message" */
  gboolean at_bottom;
  gboolean loading;
  gboolean short_window;        /* under SHORT_HEIGHT: a dense bar, a one-line field */
  guint refresh_id, draw_id, width_id, short_id;
};

G_DEFINE_FINAL_TYPE (ConversationView, conversation_view, GTK_TYPE_WIDGET)

enum { CLOSED, SHOW_OPTIONS, FORWARD, N_SIGNALS };
static guint signals[N_SIGNALS];

static void reload (ConversationView *self);
static void select_message (ConversationView *self, gint64 id);

gint64
conversation_view_get_id (ConversationView *self)
{
  return self->id;
}

static const char *
country (ConversationView *self)
{
  return sms_client_get_country (self->client);
}

static MsgPerson *
person_for (ConversationView *self, const char *destination)
{
  for (guint i = 0; destination && i < self->people->len; i++)
    if (g_strcmp0 (((MsgPerson *) self->people->pdata[i])->destination, destination) == 0)
      return self->people->pdata[i];
  return NULL;
}

static MsgPerson *
only_person (ConversationView *self)
{
  return self->people && self->people->len == 1 ? self->people->pdata[0] : NULL;
}

/* a number that can be called and written to: digits and a +, longer than a short code */
static gboolean
is_dialable (const char *destination)
{
  return destination && *destination && strspn (destination, "+0123456789") == strlen (destination)
         && strlen (destination) > 6;
}

/* one sender who is a name, not a number (a bank, a delivery service): they cannot be answered */
static gboolean
cannot_reply (ConversationView *self)
{
  MsgPerson *only = only_person (self);

  return only && only->destination && *only->destination
         && strspn (only->destination, "+0123456789 ()-.") != strlen (only->destination);
}

static gboolean
is_failed (const SmsMessage *message)
{
  return message->status == MESSAGE_STATUS_OUTGOING_FAILED
         || message->status == MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER;
}

static gint64
message_time (const SmsMessage *message)
{
  return MESSAGE_STATUS_IS_INCOMING (message->status) ? message->received : message->sent;
}

/* --- Check options: what a failed message can do */

typedef struct {
  ConversationView *self;
  gint64 id;
  VibeosSheet *sheet;
} Options;

static void
on_try_again (GtkWidget *row, Options *options)
{
  sms_client_resend (options->self->client, options->id);
  vibeos_sheet_close (options->sheet);
}

static void
on_options_delete (GtkWidget *row, Options *options)
{
  sms_client_delete_message (options->self->client, options->id);
  vibeos_sheet_close (options->sheet);
}

static GtkWidget *
sheet_row (const char *icon_name, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *row;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "msg-sheet-icon");
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "msg-sheet-title");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_size_request (box, -1, 56);
  row = vibeos_pressable_new (box);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return row;
}

static void
show_check_options (ConversationView *self, gint64 id)
{
  Options *options = g_new0 (Options, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *again = sheet_row ("msg-send-symbolic", _("Try again"));
  GtkWidget *remove = sheet_row ("msg-delete-symbolic", _("Delete"));

  options->self = self;
  options->id = id;
  options->sheet = VIBEOS_SHEET (vibeos_sheet_new (_("Not sent")));
  g_object_set_data_full (G_OBJECT (options->sheet), "msg-options", options, g_free);
  g_signal_connect (again, "clicked", G_CALLBACK (on_try_again), options);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_options_delete), options);
  gtk_box_append (GTK_BOX (content), again);
  gtk_box_append (GTK_BOX (content), remove);
  gtk_widget_set_margin_bottom (content, 16);
  vibeos_sheet_set_content (options->sheet, content);
  vibeos_sheet_present (options->sheet, GTK_WIDGET (self));
}

static void
on_check_options (GtkWidget *button, ConversationView *self)
{
  show_check_options (self, *(gint64 *) g_object_get_data (G_OBJECT (button), "id"));
}

/* --- the messages */

static void
on_bubble_clicked (GtkWidget *pressable, gpointer data)
{
  ConversationView *self = g_object_get_data (G_OBJECT (pressable), "view");
  gint64 id = *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id");
  g_autoptr(SmsMessage) message = NULL;
  SmsStore *store = sms_client_get_store (self->client);

  if (self->selected)
    {
      select_message (self, self->selected == id ? 0 : id);
      return;
    }
  /* a failed one: what can be done with it */
  message = store ? sms_store_get_message (store, id) : NULL;
  if (message && message->status == MESSAGE_STATUS_OUTGOING_FAILED)
    show_check_options (self, id);
}

static void
on_bubble_long_pressed (GtkWidget *pressable, gpointer data)
{
  ConversationView *self = g_object_get_data (G_OBJECT (pressable), "view");

  select_message (self, *(gint64 *) g_object_get_data (G_OBJECT (pressable), "id"));
}

static GtkWidget *
small_label (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, css);
  return label;
}

static GtkWidget *
make_separator (gint64 time, gboolean first)
{
  g_autofree char *text = msg_thread_separator_text (time, g_get_real_time () / 1000, vibeos_clock_uses_24h ());
  GtkWidget *label = small_label (text, "msg-separator");

  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_widget_set_size_request (label, -1, SEPARATOR_H);
  gtk_widget_set_name (label, "text_separator");
  gtk_widget_set_margin_top (label, first ? 0 : SEPARATOR_BEFORE);
  gtk_widget_set_margin_bottom (label, SEPARATOR_AFTER);
  return label;
}

/* under a failed message: the error sign, "Not sent" and "Check options" */
static GtkWidget *
make_failed_line (ConversationView *self, const SmsMessage *message)
{
  GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, META_GAP);
  GtkWidget *icon = gtk_image_new_from_icon_name ("msg-error-filled-symbolic");
  gboolean emergency = message->status == MESSAGE_STATUS_OUTGOING_FAILED_EMERGENCY_NUMBER;
  GtkWidget *text = small_label (emergency ? _("Not sent. Call emergency services instead.") : _("Not sent"),
                                 "msg-failed-text");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), META_ICON);
  gtk_widget_add_css_class (icon, "msg-failed-icon");
  gtk_box_append (GTK_BOX (line), icon);
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_box_append (GTK_BOX (line), text);
  if (!emergency)
    {
      GtkWidget *link = gtk_label_new (_("Check options"));
      GtkWidget *button;

      gtk_widget_add_css_class (link, "msg-failed-link");
      button = vibeos_pressable_new (link);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_NONE, 0);
      g_object_set_data_full (G_OBJECT (button), "id", g_memdup2 (&message->id, sizeof message->id), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_check_options), self);
      gtk_box_append (GTK_BOX (line), button);
    }
  gtk_widget_set_name (line, "metatext");
  gtk_widget_set_halign (line, GTK_ALIGN_END);
  gtk_widget_set_margin_top (line, META_TOP);
  gtk_widget_set_margin_end (line, META_END);
  return line;
}

/* what a message's own line under it says, if anything: sending, or the newest one's time */
static char *
status_text (const SmsMessage *message, gboolean time_under)
{
  switch (message->status)
    {
    case MESSAGE_STATUS_OUTGOING_YET_TO_SEND:
    case MESSAGE_STATUS_OUTGOING_SENDING:
    case MESSAGE_STATUS_OUTGOING_RESENDING:
      return g_strdup (_("Sending…"));
    case MESSAGE_STATUS_OUTGOING_AWAITING_RETRY:
      return g_strdup (_("Not sent. Trying again…"));
    default:
      break;
    }
  if (!time_under)
    return NULL;
  {
    g_autofree char *time = msg_thread_time_text (message_time (message), vibeos_clock_uses_24h ());

    if (message->status == MESSAGE_STATUS_OUTGOING_DELIVERED)
      return g_strdup_printf (C_("time, status", "%s • Delivered"), time);
    return g_steal_pointer (&time);
  }
}

static GtkWidget *
make_message (ConversationView *self, const SmsMessage *message, const MsgThreadPlace *place, int gap)
{
  gboolean incoming = MESSAGE_STATUS_IS_INCOMING (message->status);
  gboolean failed = is_failed (message);
  g_autofree char *status = failed ? NULL : status_text (message, place->time_under);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *text = gtk_label_new (message->text);
  GtkWidget *bubble, *pressable;

  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (text), PANGO_WRAP_WORD_CHAR);
  /* as wide as its text on one line, up to the bubbles' widest, and wrapped there */
  gtk_label_set_natural_wrap_mode (GTK_LABEL (text), GTK_NATURAL_WRAP_NONE);
  gtk_label_set_xalign (GTK_LABEL (text), 0);
  gtk_label_set_selectable (GTK_LABEL (text), FALSE);
  gtk_widget_add_css_class (text, "msg-message-text");
  gtk_widget_add_css_class (text, incoming ? "incoming" : "outgoing");
  gtk_widget_set_name (text, "message_text");
  gtk_widget_set_margin_start (text, BUBBLE_PAD_X);
  gtk_widget_set_margin_end (text, BUBBLE_PAD_X);
  gtk_widget_set_margin_top (text, BUBBLE_PAD_Y);
  gtk_widget_set_margin_bottom (text, BUBBLE_PAD_Y);
  if (message->id == self->selected)
    gtk_widget_add_css_class (text, "selected");

  bubble = msg_bubble_new (text);
  gtk_widget_set_name (bubble, "message_bubble");
  msg_bubble_set_run (MSG_BUBBLE (bubble), place->position, !incoming);
  msg_bubble_set_max_width (MSG_BUBBLE (bubble), self->max_width);
  g_object_set_data (G_OBJECT (bubble), "msg-incoming", GINT_TO_POINTER (incoming));
  g_object_set_data (G_OBJECT (bubble), "msg-failed", GINT_TO_POINTER (failed));
  g_object_set_data (G_OBJECT (bubble), "msg-selected", GINT_TO_POINTER (message->id == self->selected));
  g_ptr_array_add (self->bubbles, bubble);
  pressable = vibeos_pressable_new (bubble);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (pressable), VIBEOS_RIPPLE_NONE, 0);
  g_object_set_data (G_OBJECT (pressable), "view", self);
  {
    g_autofree char *spoken = status || failed ? g_strdup_printf ("%s, %s", message->text ? message->text : "",
                                                                 failed ? _("Not sent") : status)
                                               : g_strdup (message->text ? message->text : "");

    gtk_accessible_update_property (GTK_ACCESSIBLE (pressable), GTK_ACCESSIBLE_PROPERTY_LABEL, spoken, -1);
  }
  g_object_set_data_full (G_OBJECT (pressable), "id", g_memdup2 (&message->id, sizeof message->id), g_free);
  g_signal_connect (pressable, "clicked", G_CALLBACK (on_bubble_clicked), NULL);
  g_signal_connect (pressable, "long-pressed", G_CALLBACK (on_bubble_long_pressed), NULL);
  gtk_widget_set_halign (pressable, incoming ? GTK_ALIGN_START : GTK_ALIGN_END);

  /* in a group, the sender's name over the first of their run */
  if (incoming && self->people->len > 1
      && (place->position == MSG_RUN_SINGLE || place->position == MSG_RUN_FIRST))
    {
      MsgPerson *sender = person_for (self, message->sender);
      GtkWidget *name = small_label (sender ? sender->name : message->sender, "msg-sender");

      gtk_widget_set_margin_start (name, BUBBLE_PAD_X);
      gtk_box_append (GTK_BOX (column), name);
    }
  gtk_box_append (GTK_BOX (column), pressable);
  if (failed)
    gtk_box_append (GTK_BOX (column), make_failed_line (self, message));
  else if (status)
    {
      GtkWidget *line = small_label (status, "msg-time-under");

      gtk_widget_set_halign (line, incoming ? GTK_ALIGN_START : GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (column), line);
    }
  gtk_widget_set_margin_top (column, gap);
  return column;
}

/* the bubbles' colours, from the theme of now: incoming on the container's surface, ours in the primary colour
 * (dimmed when it failed), the one selected in the tertiary container's */
static void
paint_bubbles (ConversationView *self)
{
  for (guint i = 0; i < self->bubbles->len; i++)
    {
      MsgBubble *bubble = self->bubbles->pdata[i];
      GdkRGBA color;

      if (g_object_get_data (G_OBJECT (bubble), "msg-selected"))
        color = vibeos_original_color (VIBEOS_TERTIARY_CONTAINER);
      else if (g_object_get_data (G_OBJECT (bubble), "msg-incoming"))
        color = vibeos_original_color (VIBEOS_SURFACE_CONTAINER);
      else if (g_object_get_data (G_OBJECT (bubble), "msg-failed"))
        {
          GdkRGBA primary = vibeos_original_color (VIBEOS_PRIMARY), surface = vibeos_original_color (VIBEOS_SURFACE);

          color = vibeos_rgba_mix (&surface, &primary, 0.65f);
        }
      else
        color = vibeos_original_color (VIBEOS_PRIMARY);
      msg_bubble_set_color (bubble, &color);
    }
}

static void
on_theme_changed (VibeosTheme *theme, ConversationView *self)
{
  paint_bubbles (self);
}

/* --- select mode */

static SmsMessage *
selected_message (ConversationView *self)
{
  SmsStore *store = sms_client_get_store (self->client);

  return self->selected && store ? sms_store_get_message (store, self->selected) : NULL;
}

static void
select_message (ConversationView *self, gint64 id)
{
  g_autoptr(SmsMessage) message = NULL;

  self->selected = id;
  message = selected_message (self);
  if (!message)
    self->selected = 0;
  gtk_widget_set_visible (self->select_bar, self->selected != 0);
  gtk_widget_set_visible (self->bar, self->selected == 0);
  if (message)
    {
      /* forward: sent, delivered or come whole, with text to pass on (an SMS is all text);
       * copy: text, and sent or come whole; send: one that failed */
      gtk_widget_set_visible (self->forward_action, message->text && *message->text
                                                    && (message->status == MESSAGE_STATUS_OUTGOING_COMPLETE
                                                        || message->status == MESSAGE_STATUS_OUTGOING_DELIVERED
                                                        || message->status == MESSAGE_STATUS_INCOMING_COMPLETE));
      gtk_widget_set_visible (self->copy_action, message->text && *message->text
                                                 && (!MESSAGE_STATUS_IS_INCOMING (message->status)
                                                     || message->status == MESSAGE_STATUS_INCOMING_COMPLETE));
      gtk_widget_set_visible (self->resend_action, message->status == MESSAGE_STATUS_OUTGOING_FAILED);
    }
  reload (self);
}

gboolean
conversation_view_back (ConversationView *self)
{
  if (!self->selected)
    return FALSE;
  select_message (self, 0);
  return TRUE;
}

static void
on_copy (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);

  if (message)
    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), message->text ? message->text : "");
  select_message (self, 0);
}

/* the text goes on as a draft to the conversation picked (a text part for a text part) */
static void
on_forward (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);
  g_autofree char *text = message && message->text ? g_strdup (message->text) : NULL;

  select_message (self, 0);
  if (text)
    g_signal_emit (self, signals[FORWARD], 0, text);
}

static void
on_resend (ConversationView *self)
{
  gint64 id = self->selected;

  select_message (self, 0);
  sms_client_resend (self->client, id);
}

static void
on_details (ConversationView *self)
{
  g_autoptr(SmsMessage) message = selected_message (self);
  g_autoptr(GString) body = g_string_new (NULL);
  VibeosDialog *dialog;

  if (!message)
    return;
  /* Message details: an SMS's type, who, and when */
  g_string_append_printf (body, "%s%s", _("Type: "), _("Text message"));
  if (MESSAGE_STATUS_IS_INCOMING (message->status))
    {
      MsgPerson *sender = person_for (self, message->sender);
      g_autofree char *sent = msg_dates_details (message->sent);
      g_autofree char *received = msg_dates_details (message->received);

      g_string_append_printf (body, "\n%s%s", _("From: "), sender ? sender->name : message->sender);
      g_string_append_printf (body, "\n%s%s", _("Sent: "), sent);
      g_string_append_printf (body, "\n%s%s", _("Received: "), received);
    }
  else
    {
      g_autofree char *to = msg_people_name (self->people);
      g_autofree char *sent = msg_dates_details (message->sent);

      g_string_append_printf (body, "\n%s%s", _("To: "), to);
      g_string_append_printf (body, "\n%s%s", _("Sent: "), sent);
    }
  dialog = vibeos_dialog_new (_("Message details"));
  vibeos_dialog_set_message (dialog, body->str);
  msg_dialog_add_button (dialog, _("OK"), VIBEOS_RESPONSE_OK);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
  select_message (self, 0);
}

static void
on_delete_message_response (VibeosDialog *dialog, int response, gpointer data)
{
  ConversationView *self = data;
  gint64 id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dialog), "message"));

  if (response == VIBEOS_RESPONSE_OK)
    sms_client_delete_message (self->client, id);
}

static void
on_delete_message (ConversationView *self)
{
  VibeosDialog *dialog = vibeos_dialog_new (_("Delete this message?"));

  vibeos_dialog_set_message (dialog, _("This action cannot be undone."));
  msg_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), VIBEOS_RESPONSE_OK);
  g_object_set_data (G_OBJECT (dialog), "message", GINT_TO_POINTER (self->selected));
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_message_response), self);
  select_message (self, 0);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* --- the list */

static gboolean
is_at_bottom (ConversationView *self)
{
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));

  return gtk_adjustment_get_value (adj) >= gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj) - 1;
}

static void
scroll_to_bottom (ConversationView *self)
{
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));

  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

/* a new size (a message came, the keyboard opened): one at the bottom stays there */
static void
on_adjustment_changed (GtkAdjustment *adj, gpointer data)
{
  ConversationView *self = data;

  if (self->at_bottom)
    scroll_to_bottom (self);
}

static void
on_adjustment_value (GtkAdjustment *adj, gpointer data)
{
  ConversationView *self = data;

  self->at_bottom = is_at_bottom (self);
}

static void
on_view_new (gpointer data)
{
  ConversationView *self = data;

  self->at_bottom = TRUE;
  scroll_to_bottom (self);
}

static void
update_title (ConversationView *self)
{
  g_autofree char *name = msg_people_name (self->people);
  MsgPerson *only = only_person (self);

  msg_action_bar_set_title (self->bar, *name ? name : _("Messages"));
  if (only)
    msg_avatar_set_person (MSG_AVATAR (self->title_avatar), only->name, only->identifier, only->photo);
  else
    {
      g_autoptr(GPtrArray) names = g_ptr_array_new (), ids = g_ptr_array_new (), photos = g_ptr_array_new ();

      for (guint i = 0; i < self->people->len; i++)
        {
          MsgPerson *person = self->people->pdata[i];

          g_ptr_array_add (names, person->name);
          g_ptr_array_add (ids, person->identifier);
          g_ptr_array_add (photos, person->photo);
        }
      g_ptr_array_add (names, NULL);
      msg_avatar_set_group (MSG_AVATAR (self->title_avatar), (const char * const *) names->pdata,
                            (const char * const *) ids->pdata, (GBytes * const *) photos->pdata);
    }
  /* Call: one other person with a phone number (a sender's name or a short code cannot be called) */
  gtk_widget_set_visible (self->call_action, only && is_dialable (only->destination));
  /* a sender who cannot be answered: the note in the compose bar's place */
  gtk_widget_set_visible (self->compose, !cannot_reply (self));
  gtk_widget_set_visible (self->note, cannot_reply (self));
}

static void
reload (ConversationView *self)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autoptr(GPtrArray) messages = NULL;
  g_autofree MsgThreadItem *items = NULL;
  g_autofree MsgThreadPlace *places = NULL;
  gint64 newest = self->newest;
  GtkWidget *child;

  g_clear_pointer (&self->conversation, sms_conversation_free);
  self->conversation = store ? sms_store_get (store, self->id) : NULL;
  if (!self->conversation)
    {
      /* deleted (here or elsewhere): the conversation closes */
      if (!self->loading)
        g_signal_emit (self, signals[CLOSED], 0);
      return;
    }
  g_clear_pointer (&self->people, g_ptr_array_unref);
  self->people = msg_people_new (self->contacts, (const char * const *) self->conversation->destinations,
                                 country (self));
  update_title (self);

  messages = sms_store_list_messages (store, self->id);
  items = g_new0 (MsgThreadItem, MAX (messages->len, 1));
  places = g_new0 (MsgThreadPlace, MAX (messages->len, 1));
  for (guint i = 0; i < messages->len; i++)
    {
      SmsMessage *message = messages->pdata[i];

      items[i] = (MsgThreadItem) { message_time (message), !MESSAGE_STATUS_IS_INCOMING (message->status),
                                   message->sender, is_failed (message) };
    }
  msg_thread_layout (items, messages->len, places);
  while ((child = gtk_widget_get_first_child (self->messages)))
    gtk_box_remove (GTK_BOX (self->messages), child);
  g_ptr_array_set_size (self->bubbles, 0);
  for (guint i = 0; i < messages->len; i++)
    {
      if (places[i].separator)
        gtk_box_append (GTK_BOX (self->messages), make_separator (items[i].time, i == 0));
      gtk_box_append (GTK_BOX (self->messages),
                      make_message (self, messages->pdata[i], &places[i], msg_thread_gap_before (places, i)));
    }
  paint_bubbles (self);
  if (messages->len)
    {
      SmsMessage *last = messages->pdata[messages->len - 1];

      self->newest = last->id;
      /* scrolled up when a message came: "New message", View scrolls down to it */
      if (!self->loading && newest && last->id != newest && MESSAGE_STATUS_IS_INCOMING (last->status)
          && !self->at_bottom)
        msg_snackbar_show (self->snackbar, _("New message"), _("View"), on_view_new, self);
    }
}

static void
on_changed (SmsClient *client, gint64 conversation, gpointer data)
{
  ConversationView *self = data;

  if (conversation == 0 || conversation == self->id)
    reload (self);
}

static gboolean
on_refresh (gpointer data)
{
  reload (data);
  return G_SOURCE_CONTINUE;
}

/* the bubbles' widest: the width less a margin, within a cap */
static gboolean
update_widths (gpointer data)
{
  ConversationView *self = data;
  int width = gtk_widget_get_width (self->messages);

  self->width_id = 0;
  self->max_width = width > 0 ? MIN (width - BUBBLE_MARGIN, BUBBLE_MAX) : 0;
  for (guint i = 0; i < self->bubbles->len; i++)
    msg_bubble_set_max_width (self->bubbles->pdata[i], self->max_width);
  return G_SOURCE_REMOVE;
}

/* --- the compose bar */

static char *
field_text (ConversationView *self)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

static void
on_reveal (VibeosAnim *anim, double value, gpointer data)
{
  ConversationView *self = data;

  /* the send button grows from its centre; shrunk away, it is not there to tap */
  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (self->send), value);
  gtk_widget_set_can_target (self->send, value > 0.5);
}

static void
on_text_changed (GtkTextBuffer *buffer, gpointer data)
{
  ConversationView *self = data;
  g_autofree char *text = field_text (self);
  gboolean has_text = *text != '\0';
  SmsLength length = sms_segments_count (text);
  g_autofree char *counter = NULL;

  gtk_widget_set_visible (self->hint, !has_text);
  if (has_text != (vibeos_anim_get_to (self->reveal) > 0.5))
    vibeos_anim_start (self->reveal, vibeos_anim_get_value (self->reveal), has_text ? 1 : 0, REVEAL_MS, 0,
                       SPONGY_FAST_OUT_SLOW_IN);
  /* "62 / 2": once it takes more than one message, or the last 10 are left */
  counter = g_strdup_printf ("%u / %u", length.remaining, length.segments);
  gtk_label_set_text (GTK_LABEL (self->counter), counter);
  gtk_widget_set_visible (self->counter, has_text && (length.segments > 1
                                                      || length.remaining <= COUNTER_THRESHOLD));
}

static void
on_send (ConversationView *self)
{
  g_autofree char *text = field_text (self);
  g_autofree char *stripped = g_strstrip (g_strdup (text));

  if (!*stripped || !self->conversation)
    return;
  msg_sound_play_sent ();
  sms_client_send (self->client, self->id, text);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), "", -1);
  self->at_bottom = TRUE;
}

void
conversation_view_set_text (ConversationView *self, const char *text)
{
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), text ? text : "", -1);
}

void
conversation_view_save_draft (ConversationView *self)
{
  g_autofree char *text = field_text (self);
  const char *draft = self->conversation && self->conversation->show_draft ? self->conversation->draft : NULL;

  if (!self->conversation || g_strcmp0 (*text ? text : NULL, draft) == 0)
    return;
  sms_client_set_draft (self->client, self->id, text);
}

/* the field grows to a few lines of its text, then scrolls: as many lines at the font size in effect (fewer in a
 * short window) */
static void
update_field_height (ConversationView *self)
{
  int lines = self->short_window ? SHORT_FIELD_LINES : FIELD_MAX_LINES;

  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (self->field_scroll),
                                              (int) round (lines * vibeos_sp (24) + 32));
}

static void
update_send_fill (ConversationView *self)
{
  GdkRGBA fill = vibeos_original_color (VIBEOS_TERTIARY_CONTAINER);

  vibeos_pressable_set_fill (VIBEOS_PRESSABLE (self->send), &fill, FALSE);
}

/* ⊕, the pill with the text, and the round send button */
static GtkWidget *
make_compose (ConversationView *self)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *plus_icon = gtk_image_new_from_icon_name ("msg-add-circle-symbolic");
  GtkWidget *plus;
  GtkWidget *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *field_box = gtk_overlay_new ();
  GtkWidget *field_scroll = gtk_scrolled_window_new ();
  GtkWidget *send_icon = gtk_image_new_from_icon_name ("msg-send-symbolic");

  /* the shortcuts (camera, files, location …): none of them can be sent as an SMS yet, so it is shown but off */
  gtk_image_set_pixel_size (GTK_IMAGE (plus_icon), 24);
  plus = vibeos_pressable_new (plus_icon);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (plus), VIBEOS_RIPPLE_CIRCLE, 20);
  gtk_widget_set_size_request (plus, PLUS_W, COMPOSE_H);
  gtk_widget_set_valign (plus, GTK_ALIGN_END);
  gtk_widget_set_sensitive (plus, FALSE);
  gtk_widget_add_css_class (plus, "msg-compose-icon");
  gtk_widget_set_name (plus, "ComposeRowIcon:Shortcuts");
  gtk_accessible_update_property (GTK_ACCESSIBLE (plus), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Show attach options"), -1);
  gtk_box_append (GTK_BOX (row), plus);

  self->field = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->field), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (self->field), FALSE);
  gtk_text_view_set_input_hints (GTK_TEXT_VIEW (self->field), GTK_INPUT_HINT_UPPERCASE_SENTENCES
                                                              | GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_EMOJI);
  gtk_widget_add_css_class (self->field, "msg-compose-text");
  gtk_widget_set_name (self->field, "compose_message_text");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->field), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Text message"),
                                  -1);
  g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), "changed",
                    G_CALLBACK (on_text_changed), self);
  /* a few lines, then it scrolls */
  self->field_scroll = field_scroll;
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (field_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (field_scroll), TRUE);
  update_field_height (self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (field_scroll), self->field);
  gtk_overlay_set_child (GTK_OVERLAY (field_box), field_scroll);
  self->hint = gtk_label_new (_("Text message"));
  gtk_label_set_xalign (GTK_LABEL (self->hint), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->hint), PANGO_ELLIPSIZE_END);
  gtk_widget_set_can_target (self->hint, FALSE);
  gtk_widget_add_css_class (self->hint, "msg-compose-hint");
  gtk_overlay_add_overlay (GTK_OVERLAY (field_box), self->hint);
  gtk_widget_set_hexpand (field_box, TRUE);
  gtk_widget_set_valign (field_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (pill), field_box);
  /* the counter at the pill's end, when it says something */
  self->counter = small_label ("", "msg-counter");
  gtk_widget_set_valign (self->counter, GTK_ALIGN_END);
  gtk_widget_set_margin_end (self->counter, 16);
  gtk_widget_set_margin_bottom (self->counter, 8);
  gtk_widget_set_visible (self->counter, FALSE);
  gtk_box_append (GTK_BOX (pill), self->counter);
  gtk_widget_add_css_class (pill, "msg-compose-field");
  gtk_widget_set_size_request (pill, -1, COMPOSE_H);
  gtk_widget_set_hexpand (pill, TRUE);
  gtk_box_append (GTK_BOX (row), pill);

  /* the round send button, there while there is text */
  gtk_image_set_pixel_size (GTK_IMAGE (send_icon), 24);
  self->send = vibeos_pressable_new (send_icon);
  gtk_widget_set_size_request (self->send, SEND, SEND);
  gtk_widget_set_valign (self->send, GTK_ALIGN_END);
  gtk_widget_set_margin_start (self->send, SEND_GAP);
  update_send_fill (self);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->send), VIBEOS_RIPPLE_CIRCLE, SEND / 2.0);
  vibeos_pressable_set_scale (VIBEOS_PRESSABLE (self->send), 0);
  gtk_widget_set_can_target (self->send, FALSE);
  gtk_widget_add_css_class (self->send, "msg-send");
  gtk_widget_set_name (self->send, "Compose:Draft:Send");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->send), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Send SMS"), -1);
  g_signal_connect_swapped (self->send, "clicked", G_CALLBACK (on_send), self);
  gtk_box_append (GTK_BOX (row), self->send);
  self->reveal = vibeos_anim_new (self->send, on_reveal, self);

  gtk_widget_set_margin_start (row, COMPOSE_PAD);
  gtk_widget_set_margin_end (row, COMPOSE_PAD);
  gtk_widget_set_margin_top (row, COMPOSE_PAD);
  gtk_widget_set_margin_bottom (row, COMPOSE_PAD);
  gtk_widget_add_css_class (row, "msg-compose");
  return row;
}

static GtkWidget *
make_note (void)
{
  GtkWidget *label = gtk_label_new (_("Sender can't accept replies. Contact them directly."));

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign (GTK_LABEL (label), 0.5);
  gtk_widget_set_margin_start (label, NOTE_SIDE);
  gtk_widget_set_margin_end (label, NOTE_SIDE);
  gtk_widget_set_size_request (label, -1, NOTE_H);
  gtk_widget_add_css_class (label, "msg-no-reply");
  gtk_widget_set_name (label, "Compose:Disabled");
  return label;
}

/* --- the top bar */

static void
on_up (ConversationView *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
show_options (gpointer data)
{
  g_signal_emit (data, signals[SHOW_OPTIONS], 0);
}

/* Call: the Phone app calls the number at once (its "call" action); another phone's app gets the tel: link. An
 * emergency number is not handed over (the snackbar says so instead). */
static void
on_call (ConversationView *self)
{
  MsgPerson *only = only_person (self);
  g_autofree char *uri = NULL;
  g_autoptr(GError) error = NULL;

  if (!only)
    return;
  if (phone_number_is_emergency (only->destination))
    {
      msg_snackbar_show (self->snackbar, _("Can't make a voice call to emergency services here."), NULL, NULL, NULL);
      return;
    }
  if (app_action_activate (GTK_WIDGET (self), "vibeos.Dialer", "call", g_variant_new_string (only->destination)))
    return;
  uri = g_strconcat ("tel:", only->destination, NULL);
  if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
    g_warning ("cannot call: %s", error->message);
}

static void
archive (gpointer data)
{
  ConversationView *self = data;

  sms_client_set_archived (self->client, self->id, !self->conversation->archived);
  /* archiving leaves the conversation */
  if (!self->conversation->archived)
    g_signal_emit (self, signals[CLOSED], 0);
}

static void
on_delete_response (VibeosDialog *dialog, int response, gpointer data)
{
  ConversationView *self = data;

  if (response == VIBEOS_RESPONSE_OK)
    sms_client_delete_conversation (self->client, self->id);
}

static void
delete_conversation (gpointer data)
{
  ConversationView *self = data;
  VibeosDialog *dialog = vibeos_dialog_new (ngettext ("Delete this conversation?", "Delete these conversations?", 1));

  msg_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Delete"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), self);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* the one other person, when they can be blocked (one number, not blocked yet) */
static MsgPerson *
blockable (ConversationView *self)
{
  MsgPerson *only = only_person (self);

  if (!only || !only->destination || !*only->destination
      || blocked_numbers_contains (blocked_numbers_get_default (), only->destination, country (self)))
    return NULL;
  return only;
}

static void
on_block_response (VibeosDialog *dialog, int response, gpointer data)
{
  ConversationView *self = data;
  MsgPerson *only = blockable (self);
  g_autoptr(GError) error = NULL;

  if (response != VIBEOS_RESPONSE_OK || !only)
    return;
  if (!blocked_numbers_add (blocked_numbers_get_default (), only->destination, country (self), &error))
    {
      g_warning ("messaging: %s", error->message);
      return;
    }
  /* blocked: archived, and the conversation closes */
  sms_client_set_archived (self->client, self->id, TRUE);
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
block (gpointer data)
{
  ConversationView *self = data;
  MsgPerson *only = blockable (self);
  g_autofree char *title = NULL;
  VibeosDialog *dialog;

  if (!only)
    return;
  title = g_strdup_printf (_("Block %s?"), only->destination);
  dialog = vibeos_dialog_new (title);
  vibeos_dialog_set_message (dialog, _("You'll continue to receive messages from this number but won't be notified "
                                    "anymore. This conversation will be archived."));
  msg_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  msg_dialog_add_button (dialog, _("Block"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_block_response), self);
  vibeos_dialog_present (dialog, GTK_WIDGET (self));
}

/* the rows that do something here, in the reference app's order */
static void
on_overflow (GtkWidget *button, gpointer data)
{
  ConversationView *self = data;
  DialerPopupMenu *menu;

  if (!self->conversation)
    return;
  menu = dialer_popup_menu_new ();
  dialer_popup_menu_append (menu, _("Details"), show_options, self);
  dialer_popup_menu_append (menu, self->conversation->archived ? _("Unarchive") : _("Archive"), archive, self);
  dialer_popup_menu_append (menu, _("Delete"), delete_conversation, self);
  if (blockable (self))
    dialer_popup_menu_append (menu, _("Block"), block, self);
  dialer_popup_menu_set_corner (menu, MENU_CORNER);
  dialer_popup_menu_present (menu, button, POPUP_MENU_OVERFLOW);
}

/* the avatar and the name, after the back arrow: a tap opens the details */
static GtkWidget *
make_title_row (ConversationView *self)
{
  GtkWidget *title = msg_action_bar_get_title (self->bar);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *row;

  self->title_avatar = msg_avatar_new (BAR_AVATAR);
  gtk_widget_set_valign (self->title_avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_name (self->title_avatar, "monogram");
  gtk_box_append (GTK_BOX (box), self->title_avatar);
  g_object_ref (title);
  gtk_box_remove (GTK_BOX (gtk_widget_get_parent (title)), title);
  gtk_widget_set_margin_start (title, BAR_TITLE_GAP);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (box), title);
  g_object_unref (title);
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_NONE, 0);
  gtk_widget_set_hexpand (row, TRUE);
  gtk_widget_set_margin_start (row, BAR_AVATAR_GAP);
  gtk_widget_set_name (row, "top_app_bar_title_row");
  g_signal_connect_swapped (row, "clicked", G_CALLBACK (show_options), self);
  return row;
}

void
conversation_view_set_back_shown (ConversationView *self, gboolean shown)
{
  g_return_if_fail (MSG_IS_CONVERSATION_VIEW (self));

  gtk_widget_set_visible (msg_action_bar_get_up (self->bar), shown);
  gtk_widget_set_margin_start (self->title_row, shown ? BAR_AVATAR_GAP : BAR_AVATAR_ALONE);
}

/* --- the page */

/* the messages come a moment after the bar, as the page draws */
static gboolean
on_draw_messages (gpointer data)
{
  ConversationView *self = data;

  self->draw_id = 0;
  gtk_widget_set_opacity (self->messages_area, 1);
  return G_SOURCE_REMOVE;
}

static void
on_map (GtkWidget *widget, gpointer data)
{
  ConversationView *self = MSG_CONVERSATION_VIEW (widget);

  if (gtk_widget_get_opacity (self->messages_area) < 1 && !self->draw_id)
    self->draw_id = g_timeout_add (REVEAL_DRAW_MS, on_draw_messages, self);
}

/* a bar SHORT_BAR high, or as high as it was made */
static void
set_dense (GtkWidget *bar, gboolean dense)
{
  int height = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (bar), "msg-bar-height"));

  if (!height)
    {
      gtk_widget_get_size_request (bar, NULL, &height);
      g_object_set_data (G_OBJECT (bar), "msg-bar-height", GINT_TO_POINTER (height));
    }
  gtk_widget_set_size_request (bar, -1, dense ? SHORT_BAR : height);
}

/* a short window or a tall one again: the bars dense or not (not while allocating) */
static gboolean
update_short (gpointer data)
{
  ConversationView *self = data;
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  gboolean short_window = height > 0 && height < SHORT_HEIGHT;

  self->short_id = 0;
  if (short_window == self->short_window)
    return G_SOURCE_REMOVE;
  self->short_window = short_window;
  set_dense (self->bar, short_window);
  set_dense (self->select_bar, short_window);
  update_field_height (self);
  return G_SOURCE_REMOVE;
}

static void
conversation_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  ConversationView *self = MSG_CONVERSATION_VIEW (widget);
  int messages_width = width - 2 * SIDE;

  gtk_widget_allocate (self->box, width, height, baseline, NULL);
  if (MIN (messages_width - BUBBLE_MARGIN, BUBBLE_MAX) != self->max_width && !self->width_id)
    self->width_id = g_idle_add (update_widths, self);
  if ((height < SHORT_HEIGHT) != self->short_window && !self->short_id)
    self->short_id = g_idle_add (update_short, self);
}

static void
conversation_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                           int *min_baseline, int *nat_baseline)
{
  gtk_widget_measure (MSG_CONVERSATION_VIEW (widget)->box, orientation, for_size, min, nat, min_baseline,
                      nat_baseline);
}

static void
conversation_view_dispose (GObject *object)
{
  ConversationView *self = MSG_CONVERSATION_VIEW (object);

  g_clear_handle_id (&self->refresh_id, g_source_remove);
  g_clear_handle_id (&self->draw_id, g_source_remove);
  g_clear_handle_id (&self->width_id, g_source_remove);
  g_clear_handle_id (&self->short_id, g_source_remove);
  if (self->client)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), self);
  g_signal_handlers_disconnect_by_data (vibeos_text_get_default (), self);
  g_clear_pointer (&self->reveal, vibeos_anim_free);
  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->conversation, sms_conversation_free);
  g_clear_pointer (&self->people, g_ptr_array_unref);
  g_clear_pointer (&self->bubbles, g_ptr_array_unref);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (conversation_view_parent_class)->dispose (object);
}

static void
conversation_view_class_init (ConversationViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = conversation_view_dispose;
  widget_class->size_allocate = conversation_view_size_allocate;
  widget_class->measure = conversation_view_measure;
  gtk_widget_class_set_css_name (widget_class, "msg-conversation");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SHOW_OPTIONS] = g_signal_new ("show-options", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
  signals[FORWARD] = g_signal_new ("forward", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
conversation_view_init (ConversationView *self)
{
  self->at_bottom = TRUE;
  self->bubbles = g_ptr_array_new ();
  /* it opens with the keyboard down: the page has the focus, not the field */
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
}

GtkWidget *
conversation_view_new (SmsClient *client, Contacts *contacts, gint64 conversation)
{
  ConversationView *self = g_object_new (MSG_TYPE_CONVERSATION_VIEW, NULL);
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *cancel, *end;
  GtkAdjustment *adj;

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->id = conversation;
  self->people = g_ptr_array_new ();
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-conversation-page");

  /* back, the avatar and the name, Call, ⋮ */
  self->bar = msg_action_bar_new (TRUE, _("Messages"));
  gtk_widget_add_css_class (self->bar, "msg-conversation-bar");
  gtk_widget_set_name (self->bar, "top_app_bar");
  g_signal_connect_swapped (msg_action_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  self->title_row = make_title_row (self);
  end = msg_action_bar_get_end (self->bar);
  gtk_box_insert_child_after (GTK_BOX (self->bar), self->title_row, msg_action_bar_get_up (self->bar));
  gtk_widget_set_hexpand (end, FALSE);
  self->call_action = msg_action_bar_add_action (self->bar, "msg-call-symbolic", _("Call"));
  g_signal_connect_swapped (self->call_action, "clicked", G_CALLBACK (on_call), self);
  g_signal_connect (msg_action_bar_add_overflow (self->bar), "clicked", G_CALLBACK (on_overflow), self);
  gtk_box_append (GTK_BOX (self->box), self->bar);

  self->select_bar = msg_action_bar_new (FALSE, "");
  gtk_widget_add_css_class (self->select_bar, "msg-conversation-bar");
  cancel = msg_action_bar_add_close (self->select_bar, _("Done"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (conversation_view_back), self);
  self->forward_action = msg_action_bar_add_action (self->select_bar, "msg-forward-symbolic", _("Forward"));
  g_signal_connect_swapped (self->forward_action, "clicked", G_CALLBACK (on_forward), self);
  self->copy_action = msg_action_bar_add_action (self->select_bar, "msg-content-copy-symbolic", _("Copy text"));
  g_signal_connect_swapped (self->copy_action, "clicked", G_CALLBACK (on_copy), self);
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "msg-info-symbolic", _("View details")),
                            "clicked", G_CALLBACK (on_details), self);
  self->resend_action = msg_action_bar_add_action (self->select_bar, "msg-send-symbolic", _("Send"));
  g_signal_connect_swapped (self->resend_action, "clicked", G_CALLBACK (on_resend), self);
  g_signal_connect_swapped (msg_action_bar_add_action (self->select_bar, "msg-delete-symbolic", _("Delete")),
                            "clicked", G_CALLBACK (on_delete_message), self);
  gtk_widget_set_visible (self->select_bar, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->select_bar);

  /* the messages on their container, at the bottom while they are fewer than a screen */
  self->messages = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name (self->messages, "message_list");
  gtk_widget_set_valign (self->messages, GTK_ALIGN_END);
  gtk_widget_set_vexpand (self->messages, TRUE);
  gtk_widget_set_margin_start (self->messages, SIDE);
  gtk_widget_set_margin_end (self->messages, SIDE);
  gtk_widget_set_margin_top (self->messages, LIST_TOP);
  gtk_widget_set_margin_bottom (self->messages, LIST_BOTTOM);
  gtk_box_append (GTK_BOX (inner), self->messages);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), inner);
  gtk_widget_set_vexpand (self->scroll, TRUE);
  adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroll));
  g_signal_connect (adj, "changed", G_CALLBACK (on_adjustment_changed), self);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adjustment_value), self);
  self->messages_area = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (self->messages_area), self->scroll);
  self->snackbar = msg_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (self->messages_area), GTK_WIDGET (self->snackbar));
  gtk_widget_set_vexpand (self->messages_area, TRUE);
  gtk_widget_set_opacity (self->messages_area, 0);
  self->container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->container, "msg-conversation-container");
  gtk_widget_set_overflow (self->container, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_vexpand (self->container, TRUE);
  gtk_box_append (GTK_BOX (self->container), self->messages_area);
  self->compose = make_compose (self);
  gtk_box_append (GTK_BOX (self->container), self->compose);
  gtk_box_append (GTK_BOX (self->box), self->container);
  self->note = make_note ();
  gtk_widget_set_visible (self->note, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->note);

  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), self);
  g_signal_connect_swapped (vibeos_theme_get_default (), "changed", G_CALLBACK (update_send_fill), self);
  g_signal_connect_swapped (vibeos_text_get_default (), "changed", G_CALLBACK (update_field_height), self);
  g_signal_connect (client, "changed", G_CALLBACK (on_changed), self);
  /* the network's country and the address book can come in after the conversation opens (a notification's tap) */
  g_signal_connect_swapped (client, "notify::country", G_CALLBACK (reload), self);
  g_signal_connect_swapped (self->contacts, "changed", G_CALLBACK (reload), self);
  self->refresh_id = g_timeout_add (REFRESH_MS, on_refresh, self);
  self->loading = TRUE;
  reload (self);
  self->loading = FALSE;
  /* the draft comes back into the field */
  if (self->conversation && self->conversation->show_draft && self->conversation->draft)
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->field)), self->conversation->draft,
                              -1);
  return GTK_WIDGET (self);
}
