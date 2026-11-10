/* Call details: see call-details.h. The rules are CallDetailsActivityCommon's, CallDetailsHeaderViewHolder's,
 * CallDetailsEntryViewHolder's and CallDetailsFooterViewHolder's (dialer/calldetails); the geometry is
 * dialer_toolbar.xml's, contact_container.xml's, call_details_entry.xml's and call_details_footer.xml's, measured on
 * still:dialer-call-details (spec §5.5). */
#include "call-details.h"
#include "avatar.h"
#include "call-dates.h"
#include "call-log.h"
#include "calls-client.h"
#include "contacts-app.h"
#include "phone.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>

#define BAR_HEIGHT      56      /* ?actionBarSize */
#define NAV_SIZE        56      /* the Toolbar's navigation button */
#define TITLE_START     72      /* contentInsetStartWithNavigation */
/* contact_container.xml */
#define HEADER_TOP      6
#define HEADER_PAD_TOP  12
#define HEADER_PAD_START 12
#define HEADER_PAD_BOTTOM 8
#define HEADER_PAD_END  8
#define BADGE           48      /* call_details_contact_photo_size */
#define BADGE_PAD       4       /* call_details_contact_photo_padding: the picture is 40 dp */
#define TEXT_START      12
#define BUTTON          48      /* call_back_button_size */
/* call_details_entry.xml */
#define ENTRY_PAD_TOP   16      /* call_entry_padding */
#define ENTRY_PAD_BOTTOM 14     /* call_entry_bottom_padding */
#define ENTRY_ICON_START 16
#define ENTRY_ICON      24      /* CallTypeIconsView's large icons */
#define ENTRY_TEXT      72      /* call_entry_text_left_margin */
#define ENTRY_END       16
/* call_details_footer.xml, CallDetailsActionItemStyle */
#define DIVIDER_MARGIN  8
#define ACTION_PAD      28      /* paddingStart/End and drawablePadding */
#define ACTION_PAD_V    16
#define ACTION_ICON     24
#define TEXT_BOX        1.42    /* a Dialer.TextAppearance TextView's height over its text size: Google Sans Flex with
                                 * its font padding (still:dialer-call-details: 68 px at 16 sp, 60 px at 14 sp) */

struct _CallDetails
{
  GtkWidget parent;
  GtkWidget *box;               /* the toolbar, the list */
  GtkWidget *list, *header, *entries, *footer;
  Contacts *contacts;
  NeoSnackbar *snackbar;
  char *number;
  Presentation presentation;
  GArray *ids;                  /* gint64 */
  GCancellable *loading;
  char *uid;                    /* the contact's, or NULL */
};

G_DEFINE_FINAL_TYPE (CallDetails, call_details, GTK_TYPE_WIDGET)

enum { CLOSED, EDIT_NUMBER, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
country (void)
{
  const char *country = calls_client_get_country (calls_client_get ());

  return country && *country ? country : phone_number_locale_country ();
}

static GtkWidget *
text_label (const char *text, const char *css_class, double size)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css_class);
  neo_label_set_font_padding (label, size);
  return label;
}

static void
close_screen (CallDetails *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

/* ---- the header (CallDetailsHeaderViewHolder.updateContactInfo) --------------------------------------------- */

static void
on_dialed (guint id, const GError *error, gpointer data)
{
  CallDetails *self = data;

  if (error)
    neo_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
  else
    phone_start_app (INCALL_APP_ID);
  g_object_unref (self);
}

/* the call-back button: a voice call at once (PreCall, CALL_DETAILS) */
static void
on_call (CallDetails *self)
{
  calls_client_dial (calls_client_get (), self->number, on_dialed, g_object_ref (self));
}

/* QuickContactBadge: a contact's card */
static void
on_photo (CallDetails *self)
{
  contacts_app_view (GTK_WIDGET (self), self->uid, self->snackbar);
}

static GtkWidget *
make_header (CallDetails *self)
{
  Presentation presentation = phone_number_presentation (self->number, self->presentation);
  const char *withheld = phone_number_presentation_name (presentation);
  const Contact *contact = withheld ? NULL : contacts_lookup (self->contacts, self->number, country ());
  g_autofree char *formatted = withheld ? NULL : phone_number_format (self->number, country ());
  gboolean is_contact = contact && contact->name && *contact->name;
  const char *name = is_contact ? contact->name : withheld ? withheld : formatted;
  g_autofree char *secondary = NULL;
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = dialer_avatar_new (BADGE - 2 * BADGE_PAD);
  GtkWidget *badge, *texts, *label;

  g_clear_pointer (&self->uid, g_free);
  if (is_contact)
    self->uid = g_strdup (contact->uid);
  /* the number's kind and the number under a contact's name ("Mobile +49 176 …"); an emergency number is named so,
   * with nothing under it */
  if (!withheld && phone_number_is_emergency (self->number))
    name = _("Emergency number");
  else if (is_contact)
    {
      const char *kind = contact_number_label (contact, self->number, country ());

      secondary = kind && *kind ? g_strdup_printf (C_("call_subject_type_and_number", "%1$s %2$s"), kind, formatted)
                                : g_strdup (formatted);
    }

  gtk_widget_set_margin_top (header, HEADER_TOP + HEADER_PAD_TOP);
  gtk_widget_set_margin_start (header, HEADER_PAD_START);
  gtk_widget_set_margin_bottom (header, HEADER_PAD_BOTTOM);
  gtk_widget_set_margin_end (header, HEADER_PAD_END);

  /* the badge: 48 dp with 4 dp padding, so a 40 dp picture; its own colours (forceDarkAllowed false) */
  dialer_avatar_set (DIALER_AVATAR (avatar), is_contact ? contact->uid : name, is_contact ? contact->name : NULL,
                     is_contact ? contact->photo : NULL);
  gtk_widget_set_margin_start (avatar, BADGE_PAD);
  gtk_widget_set_margin_end (avatar, BADGE_PAD);
  gtk_widget_set_margin_top (avatar, BADGE_PAD);
  gtk_widget_set_margin_bottom (avatar, BADGE_PAD);
  badge = neo_pressable_new (avatar);
  neo_pressable_set_ripple (NEO_PRESSABLE (badge), NEO_RIPPLE_CIRCLE, BADGE / 2.0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (badge), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive (badge, is_contact);
  if (is_contact)
    gtk_accessible_update_property (GTK_ACCESSIBLE (badge), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect_swapped (badge, "clicked", G_CALLBACK (on_photo), self);
  gtk_box_append (GTK_BOX (header), badge);

  /* the name and the number, centred in a column at least as tall as the badge */
  texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (texts, -1, BADGE);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (texts, TEXT_START);
  label = text_label (name, "dialer-primary-text", 16);
  gtk_widget_set_vexpand (label, TRUE);
  gtk_widget_set_valign (label, secondary ? GTK_ALIGN_END : GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (texts), label);
  if (secondary)
    {
      label = text_label (secondary, "dialer-secondary-text", 14);
      gtk_widget_set_vexpand (label, TRUE);
      gtk_widget_set_valign (label, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (texts), label);
    }
  gtk_box_append (GTK_BOX (header), texts);

  /* CallbackAction.VOICE: quantum_ic_call in ?colorIcon; NONE for a number that cannot be called */
  if (!withheld && *self->number)
    {
      GtkWidget *call = neo_icon_button_new ("dialer-call-symbolic");

      gtk_widget_set_size_request (call, BUTTON, BUTTON);
      gtk_widget_set_valign (call, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (call, "dialer-icon");
      neo_pressable_set_ripple_color (NEO_PRESSABLE (call), &(GdkRGBA) { 1, 1, 1, 0.06 });
      gtk_accessible_update_property (GTK_ACCESSIBLE (call), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Call"), -1);
      g_signal_connect_swapped (call, "clicked", G_CALLBACK (on_call), self);
      gtk_box_append (GTK_BOX (header), call);
    }
  return header;
}

/* ---- the calls (CallDetailsEntryViewHolder) ---------------------------------------------------------------- */

/* CallTypeIconsView's large icon and CallTypeHelper's text for a call of this type */
static void
call_type (CallType type, const char **icon, const char **css_class, const char **text)
{
  switch (type)
    {
    case CALL_TYPE_INCOMING:
      *icon = "dialer-call-received-symbolic";
      *css_class = "details-incoming";
      *text = _("Incoming call");
      break;
    case CALL_TYPE_ANSWERED_EXTERNALLY:
      *icon = "dialer-call-received-symbolic";
      *css_class = "details-incoming";
      *text = _("Call answered on another device");
      break;
    case CALL_TYPE_OUTGOING:
      *icon = "dialer-call-made-symbolic";
      *css_class = "details-outgoing";
      *text = _("Outgoing call");
      break;
    case CALL_TYPE_VOICEMAIL:
      *icon = "dialer-voicemail-symbolic";
      *css_class = "dialer-icon";
      *text = _("Voicemail");
      break;
    case CALL_TYPE_REJECTED:
      *icon = "dialer-call-missed-symbolic";
      *css_class = "details-missed";
      *text = _("Declined call");
      break;
    case CALL_TYPE_BLOCKED:
      *icon = "dialer-block-symbolic";
      *css_class = "dialer-icon";
      *text = _("Blocked call");
      break;
    case CALL_TYPE_MISSED:
    default:
      *icon = "dialer-call-missed-symbolic";
      *css_class = "details-missed";
      *text = _("Missed call");
      break;
    }
}

/* a one-line TextView's height, which the font here (Roboto's metrics) falls short of: the room left goes under
 * the text, which keeps its place at the top */
static void
set_text_box (GtkWidget *label, double size)
{
  int box = (int) round (size * TEXT_BOX);

  gtk_widget_set_size_request (label, -1, box - gtk_widget_get_margin_top (label)
                                         - gtk_widget_get_margin_bottom (label));
}

/* CallTypeHelper.isMissedCallType: anything but a call that was talked on (or a voicemail) */
static gboolean
is_missed_type (CallType type)
{
  return type != CALL_TYPE_INCOMING && type != CALL_TYPE_OUTGOING && type != CALL_TYPE_VOICEMAIL
         && type != CALL_TYPE_ANSWERED_EXTERNALLY;
}

static GtkWidget *
make_entry (const CallLogEntry *entry)
{
  const char *icon_name, *icon_class, *type_text;
  g_autofree char *when = call_dates_details (entry->date);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon, *texts, *line, *type, *time;

  call_type (entry->type, &icon_name, &icon_class, &type_text);
  gtk_widget_set_margin_top (row, ENTRY_PAD_TOP);
  gtk_widget_set_margin_bottom (row, ENTRY_PAD_BOTTOM);

  /* one 24 dp arrow, its colour through the colour filter, at the top beside the type */
  icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), ENTRY_ICON);
  gtk_widget_set_valign (icon, GTK_ALIGN_START);
  gtk_widget_set_margin_start (icon, ENTRY_ICON_START);
  gtk_widget_add_css_class (icon, icon_class);
  gtk_box_append (GTK_BOX (row), icon);

  /* call_entry's RelativeLayout: the type up to 16 dp before the duration at the end; the time under the type, as
   * wide as the row less 16 dp, on more lines if it must */
  texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_margin_start (texts, ENTRY_TEXT - ENTRY_ICON_START - ENTRY_ICON);
  gtk_widget_set_margin_end (texts, ENTRY_END);
  line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  type = text_label (type_text, "dialer-primary-text", 16);
  gtk_widget_set_hexpand (type, TRUE);
  set_text_box (type, 16);
  gtk_box_append (GTK_BOX (line), type);
  /* the duration; none for a call that was not talked on */
  if (!is_missed_type (entry->type))
    {
      g_autofree char *duration = call_dates_duration (entry->duration);
      GtkWidget *label = text_label (duration, "dialer-primary-text", 16);

      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_NONE);
      gtk_widget_set_margin_start (label, ENTRY_END);
      set_text_box (label, 16);
      gtk_box_append (GTK_BOX (line), label);
    }
  gtk_box_append (GTK_BOX (texts), line);
  /* the time in textColorSecondary, or dialer_red for a missed call */
  time = text_label (when, "dialer-secondary-text", 14);
  gtk_label_set_single_line_mode (GTK_LABEL (time), FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (time), PANGO_ELLIPSIZE_NONE);
  gtk_label_set_wrap (GTK_LABEL (time), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (time), PANGO_WRAP_WORD_CHAR);
  set_text_box (time, 14);
  if (entry->type == CALL_TYPE_MISSED || (entry->type > CALL_TYPE_ANSWERED_EXTERNALLY))
    gtk_widget_add_css_class (time, "details-missed-text");
  gtk_box_append (GTK_BOX (texts), time);
  gtk_box_append (GTK_BOX (row), texts);
  return row;
}

/* ---- the footer (CallDetailsFooterViewHolder) -------------------------------------------------------------- */

static GtkWidget *
action_row (CallDetails *self, const char *icon_name, const char *text, GCallback callback)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = text_label (text, "dialer-secondary-text", 14);
  GtkWidget *row;

  gtk_widget_set_size_request (box, -1, ACTION_PAD_V + ACTION_ICON + ACTION_PAD_V);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), ACTION_ICON);
  gtk_widget_set_margin_start (icon, ACTION_PAD);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, ACTION_PAD);
  gtk_widget_set_margin_end (label, ACTION_PAD);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_signal_connect_swapped (row, "clicked", callback, self);
  return row;
}

/* ClipboardUtils.copyText with its toast */
static void
on_copy (CallDetails *self)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), self->number);
  neo_snackbar_show (self->snackbar, _("Text copied"), NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

/* ACTION_DIAL: the dialpad with the number, in the main screen */
static void
on_edit (CallDetails *self)
{
  g_signal_emit (self, signals[EDIT_NUMBER], 0, self->number);
}

/* every call of the entry, no confirmation; then the screen goes (RESULT_OK) */
static void
on_delete (CallDetails *self)
{
  calls_client_delete_call_log (calls_client_get (), (gint64 *) self->ids->data, self->ids->len);
  close_screen (self);
}

static GtkWidget *
make_footer (CallDetails *self)
{
  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (divider, "recents-divider");
  gtk_widget_set_margin_top (divider, DIVIDER_MARGIN);
  gtk_widget_set_margin_bottom (divider, DIVIDER_MARGIN);
  gtk_box_append (GTK_BOX (footer), divider);
  /* the number rows only with a number; "Report inaccurate number" never shows (no caller-id service) */
  if (*self->number && !phone_number_presentation_name (phone_number_presentation (self->number, self->presentation)))
    {
      gtk_box_append (GTK_BOX (footer), action_row (self, "dialer-copy-symbolic", _("Copy number"),
                                                    G_CALLBACK (on_copy)));
      gtk_box_append (GTK_BOX (footer), action_row (self, "dialer-edit-symbolic", _("Edit number before call"),
                                                    G_CALLBACK (on_edit)));
    }
  gtk_box_append (GTK_BOX (footer), action_row (self, "dialer-delete-symbolic", _("Delete"), G_CALLBACK (on_delete)));
  return footer;
}

/* ---- loading ------------------------------------------------------------------------------------------------ */

static gboolean
has_id (CallDetails *self, gint64 id)
{
  for (guint i = 0; i < self->ids->len; i++)
    if (g_array_index (self->ids, gint64, i) == id)
      return TRUE;
  return FALSE;
}

static void
on_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  CallDetails *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) entries = calls_client_get_call_log_finish (CALLS_CLIENT (source), result, &error);
  GtkWidget *child;
  guint shown = 0;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->loading);
  if (!entries)
    {
      g_warning ("call details: %s", error->message);
      return;
    }
  while ((child = gtk_widget_get_first_child (self->entries)))
    gtk_box_remove (GTK_BOX (self->entries), child);
  /* the log is newest first, as the entry's calls are */
  for (guint i = 0; i < entries->len; i++)
    {
      const CallLogEntry *entry = entries->pdata[i];

      if (!has_id (self, entry->id))
        continue;
      gtk_box_append (GTK_BOX (self->entries), make_entry (entry));
      shown++;
    }
  /* its calls were deleted from elsewhere: nothing is left to show */
  if (!shown)
    close_screen (self);
}

static void
load (CallDetails *self)
{
  if (self->loading)
    g_cancellable_cancel (self->loading);
  g_clear_object (&self->loading);
  self->loading = g_cancellable_new ();
  calls_client_get_call_log (calls_client_get (), self->loading, on_loaded, self);
}

/* a contact added or changed (in the Contacts app, say): the header again */
static void
on_contacts_changed (CallDetails *self)
{
  GtkWidget *header = make_header (self);

  gtk_box_insert_child_after (GTK_BOX (self->list), header, self->header);
  gtk_box_remove (GTK_BOX (self->list), self->header);
  self->header = header;
}

/* ---- the screen --------------------------------------------------------------------------------------------- */

static gboolean
on_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, CallDetails *self)
{
  if (keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back)
    return FALSE;
  close_screen (self);
  return TRUE;
}

/* DialerToolbar: ✕ (finish) and the title, colorPrimary, no elevation */
static GtkWidget *
make_bar (CallDetails *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *close = neo_icon_button_new ("dialer-close-symbolic");
  GtkWidget *title = gtk_label_new (_("Call details"));

  gtk_widget_add_css_class (bar, "dialer-history-bar");
  gtk_widget_set_size_request (bar, -1, BAR_HEIGHT);
  gtk_widget_set_size_request (close, NAV_SIZE, NAV_SIZE);
  gtk_widget_set_valign (close, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (close, "dialer-icon");
  /* Android names it "Close"; here its name is the one the shell's back gesture clicks */
  gtk_accessible_update_property (GTK_ACCESSIBLE (close), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Close"), -1);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (close_screen), self);
  gtk_box_append (GTK_BOX (bar), close);

  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (title), TRUE);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_widget_set_margin_start (title, TITLE_START - NAV_SIZE);
  gtk_widget_add_css_class (title, "dialer-toolbar-title");
  gtk_box_append (GTK_BOX (bar), title);
  return bar;
}

static void
call_details_dispose (GObject *object)
{
  CallDetails *self = CALL_DETAILS (object);

  if (self->loading)
    g_cancellable_cancel (self->loading);
  g_clear_object (&self->loading);
  g_signal_handlers_disconnect_by_data (calls_client_get (), self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->number, g_free);
  g_clear_pointer (&self->uid, g_free);
  g_clear_pointer (&self->ids, g_array_unref);
  G_OBJECT_CLASS (call_details_parent_class)->dispose (object);
}

static void
call_details_class_init (CallDetailsClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = call_details_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "call-details");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[EDIT_NUMBER] = g_signal_new ("edit-number", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
call_details_init (CallDetails *self)
{
}

GtkWidget *
call_details_new (Contacts *contacts, NeoSnackbar *snackbar, const char *number, Presentation presentation,
                  const gint64 *ids, guint n_ids)
{
  CallDetails *self = g_object_new (CALL_TYPE_DETAILS, NULL);
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkWidget *scroller;

  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  self->number = g_strdup (number ? number : "");
  self->presentation = presentation;
  self->ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_array_append_vals (self->ids, ids, n_ids);

  gtk_widget_add_css_class (GTK_WIDGET (self), "dialer-history");
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_box_append (GTK_BOX (self->box), make_bar (self));

  /* the RecyclerView: the header, the calls, the footer; no dividers between the calls */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->header = make_header (self);
  gtk_box_append (GTK_BOX (self->list), self->header);
  self->entries = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (self->list), self->entries);
  self->footer = make_footer (self);
  gtk_box_append (GTK_BOX (self->list), self->footer);
  scroller = spongy_scroll_view_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroller), self->list);
  gtk_box_append (GTK_BOX (self->box), scroller);

  g_signal_connect_swapped (calls_client_get (), "call-log-changed", G_CALLBACK (load), self);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  load (self);
  return GTK_WIDGET (self);
}
