/* The Recents tab: see recents-page.h. Calls are grouped and described by the rules of Lineage's call log. */
#include "recents-page.h"
#include "avatar.h"
#include "blocked-numbers.h"
#include "call-dates.h"
#include "call-log.h"
#include "call-types.h"
#include "calls-client.h"
#include "contacts-app.h"
#include "empty-view.h"
#include "phone.h"
#include "phone-number.h"
#include "popup-menu.h"
#include "text.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define LIST_PAD_X      8
#define LIST_PAD_BOTTOM 88      /* room at the foot, so the dialpad button covers no row */
#define HEADER_START    8
#define HEADER_PAD_TOP  15      /* above a day's header (Today, Yesterday) */
#define HEADER_PAD_BOTTOM 9
#define CARD_MARGIN     4
#define CARD_RADIUS     28
#define ROW_PAD         8       /* round a row's contents */
#define PHOTO           48
#define PHOTO_PAD_TOP   2       /* the picture is PHOTO less this, this far down */
#define INFO_START      16
#define NAME_GAP        2       /* under the name */
#define ICON_MARGIN     2       /* after each call-type arrow */
#define TYPE_ICON       12
#define BUTTON          48      /* the call button */
#define ACTION_HEIGHT   48      /* a row of the expanded actions */
#define ACTION_ICON     24
#define ACTION_ICON_START 16
#define ACTION_TEXT_PAD 24
#define EXPAND_MS       200     /* the actions opening and closing */
#define MAX_TYPE_ICONS  3       /* call-type arrows on a line; more calls add a count */
#define FIRST_ROWS      20      /* rows made at once; the rest come in chunks while idle */
#define CHUNK_ROWS      30

/* the expanded row's card and the call-type arrows */
#define CARD_EXPANDED   0x242727
#define TYPE_INCOMING   0x007fc6
#define TYPE_OUTGOING   0x009740
#define TYPE_MISSED     0x922b20
#define TYPE_PLAIN      0xf6f7f8

#define HEADER_STYLE    TEXT_OVERLINE
#define NAME_STYLE(bold)  ((TextStyle) { TEXT_ROBOTO, 16, (bold) ? 900 : 700 })  /* bold: an unseen missed call */
#define DATE_STYLE(bold)  ((TextStyle) { TEXT_ROBOTO, 14, (bold) ? 900 : 700 })

/* ---- an entry: calls in a row with the same number --------------------------------------------------------- */

typedef struct
{
  GArray *ids;                  /* gint64, the group's calls, newest first */
  CallType types[MAX_TYPE_ICONS];
  guint n_types;
  guint count;
  char *number;                 /* as dialled or received; "" withheld */
  Presentation presentation;
  gint64 date;                  /* the newest call's */
  DayGroup day;
  gboolean header;              /* the first entry of its day */
  gboolean unread;              /* a missed call not yet seen: bold */
  /* who, against the contacts */
  char *name;
  char *detail;                 /* the label or place before the date ("Mobile"), or NULL */
  char *uid, *identifier;
  gboolean is_contact;
  GBytes *photo;
  gboolean callable;
} Group;

static void
group_free (Group *group)
{
  if (!group)
    return;
  g_array_unref (group->ids);
  g_free (group->number);
  g_free (group->name);
  g_free (group->detail);
  g_free (group->uid);
  g_free (group->identifier);
  g_clear_pointer (&group->photo, g_bytes_unref);
  g_free (group);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Group, group_free)

static gint64
group_id (const Group *group)
{
  return g_array_index (group->ids, gint64, 0);
}

static gboolean
same_line (const CallLogEntry *a, const CallLogEntry *b, const char *country)
{
  /* numbers with * # , ; only group when they are the same characters */
  if (strpbrk (a->number, "*#,;") || strpbrk (b->number, "*#,;"))
    return g_str_equal (a->number, b->number);
  if (!*a->number || !*b->number)
    return FALSE;
  return phone_number_equal (a->number, b->number, country);
}

/* the conditions under which a call joins the entry above it: the same number, never voicemail,
 * blocked calls only with blocked ones */
static gboolean
joins (const CallLogEntry *group, const CallLogEntry *call, const char *country)
{
  if (group->type == CALL_TYPE_VOICEMAIL || call->type == CALL_TYPE_VOICEMAIL)
    return FALSE;
  if ((group->type == CALL_TYPE_BLOCKED) != (call->type == CALL_TYPE_BLOCKED))
    return FALSE;
  return same_line (group, call, country);
}

/* the line under the name: for a contact, the kind of the number (or the
 * number, when it has no kind); for anyone else, where the number is from, which needs a geocoder this phone does
 * not have (libphonenumber's offline one), so nothing */
static void
group_resolve (Group *group, Contacts *contacts, const char *country)
{
  Presentation presentation = phone_number_presentation (group->number, group->presentation);
  const char *withheld = phone_number_presentation_name (presentation);
  const Contact *contact = withheld ? NULL : contacts_lookup (contacts, group->number, country);
  g_autofree char *formatted = withheld ? NULL : phone_number_format (group->number, country);

  group->callable = !withheld && *group->number;
  if (group->n_types && group->types[0] == CALL_TYPE_BLOCKED)
    group->detail = g_strdup (_("Blocked"));
  if (contact && contact->name && *contact->name)
    {
      const char *label = contact_number_label (contact, group->number, country);

      group->is_contact = TRUE;
      group->name = g_strdup (contact->name);
      group->uid = g_strdup (contact->uid);
      group->identifier = g_strdup (contact->uid);
      group->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
      if (!group->detail)
        group->detail = g_strdup (label && *label ? label : formatted);
    }
  else
    {
      group->name = g_strdup (withheld ? withheld : formatted);
      /* not a contact: the tile's colour from what is shown, and the person glyph (no letter) */
      group->identifier = g_strdup (group->name);
    }
}

/* the entries: calls in a row with the same number grouped, over the log, newest first */
static GPtrArray *
make_groups (GPtrArray *entries, Contacts *contacts, const char *country)
{
  GPtrArray *groups = g_ptr_array_new_with_free_func ((GDestroyNotify) group_free);
  gint64 now = g_get_real_time () / 1000;
  Group *group = NULL;
  CallLogEntry *first = NULL;
  int last_day = -1;

  for (guint i = 0; i < entries->len; i++)
    {
      CallLogEntry *entry = entries->pdata[i];

      if (!group || !joins (first, entry, country))
        {
          group = g_new0 (Group, 1);
          group->ids = g_array_new (FALSE, FALSE, sizeof (gint64));
          group->number = g_strdup (entry->number);
          group->presentation = entry->presentation;
          group->date = entry->date;
          group->day = call_dates_day_group (entry->date, now);
          group->header = (int) group->day != last_day;
          last_day = group->day;
          /* only a missed call is ever bold */
          group->unread = entry->type == CALL_TYPE_MISSED && !entry->is_read;
          g_ptr_array_add (groups, group);
          first = entry;
        }
      g_array_append_val (group->ids, entry->id);
      if (group->n_types < MAX_TYPE_ICONS)
        group->types[group->n_types++] = entry->type;
      group->count++;
    }
  for (guint i = 0; i < groups->len; i++)
    group_resolve (groups->pdata[i], contacts, country);
  return groups;
}

static void
type_icon (CallType type, const char **icon, guint32 *color)
{
  switch (type)
    {
    case CALL_TYPE_INCOMING:
    case CALL_TYPE_ANSWERED_EXTERNALLY:
      *icon = "dialer-call-received-symbolic";
      *color = TYPE_INCOMING;
      break;
    case CALL_TYPE_OUTGOING:
      *icon = "dialer-call-made-symbolic";
      *color = TYPE_OUTGOING;
      break;
    case CALL_TYPE_VOICEMAIL:
      *icon = "dialer-voicemail-symbolic";
      *color = TYPE_PLAIN;
      break;
    case CALL_TYPE_BLOCKED:
      *icon = "dialer-block-symbolic";
      *color = TYPE_PLAIN;
      break;
    default:
      *icon = "dialer-call-missed-symbolic";
      *color = TYPE_MISSED;
      break;
    }
}

/* ---- what an entry shows ------------------------------------------------------------------------------------ */

#define RECENTS_TYPE_CONTENT (recents_content_get_type ())
G_DECLARE_FINAL_TYPE (RecentsContent, recents_content, RECENTS, CONTENT, GtkWidget)

struct _RecentsContent
{
  GtkWidget parent;
  const Group *group;           /* the row's */
  GtkWidget *call;              /* the call-back button, or NULL for a withheld number */
  PangoLayout *name, *date;
  double laid_out;              /* the width the layouts are for; 0: to be made */
  double text_height, row_height;
};

G_DEFINE_FINAL_TYPE (RecentsContent, recents_content, GTK_TYPE_WIDGET)

static double
icons_width (const Group *group)
{
  /* each arrow is square, with ICON_MARGIN after it */
  return group->n_types * (TYPE_ICON + ICON_MARGIN);
}

/* the text column: from the picture to the button (or the end, without one) */
static double
text_column (RecentsContent *self, double width)
{
  return width - ROW_PAD - (PHOTO + INFO_START) - ROW_PAD - (self->call ? BUTTON : 0);
}

static void
lay_out (RecentsContent *self, double width)
{
  const Group *group = self->group;
  double column = text_column (self, width);
  double date_width = column - icons_width (group) - ICON_MARGIN - ICON_MARGIN;
  g_autofree char *when = call_dates_relative (group->date, g_get_real_time () / 1000);
  g_autofree char *date_line = NULL, *text = NULL;

  if (self->laid_out == width)
    return;
  self->laid_out = width;
  g_clear_object (&self->name);
  g_clear_object (&self->date);
  /* the name, bold while a missed call is unseen; ellipsized */
  self->name = text_layout (GTK_WIDGET (self), NAME_STYLE (group->unread), group->name, column - ICON_MARGIN);
  /* "[(N) ]<kind or place>, <when>": the count only past the three arrows. The style the XML asks for never
   * applies (it lacks "@style/"), so the line wraps instead of ellipsizing */
  date_line = group->detail ? g_strdup_printf ("%s, %s", group->detail, when) : g_strdup (when);
  text = group->count > MAX_TYPE_ICONS ? g_strdup_printf (_("(%1$u) %2$s"), group->count, date_line)
                                       : g_strdup (date_line);
  self->date = gtk_widget_create_pango_layout (GTK_WIDGET (self), text);
  text_layout_set_style (self->date, DATE_STYLE (group->unread));
  pango_layout_set_width (self->date, (int) (MAX (date_width, 1) * PANGO_SCALE));
  pango_layout_set_wrap (self->date, PANGO_WRAP_WORD_CHAR);
  self->row_height = MAX (TYPE_ICON, text_box_height (self->date, DATE_STYLE (group->unread)));
  self->text_height = text_box_height (self->name, NAME_STYLE (group->unread)) + NAME_GAP + self->row_height;
}

static double
content_height (RecentsContent *self)
{
  /* the picture, the text and the button side by side, ROW_PAD around them */
  return ROW_PAD + MAX (MAX (PHOTO, self->text_height), self->call ? BUTTON : 0) + ROW_PAD;
}

static GtkSizeRequestMode
recents_content_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
recents_content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  RecentsContent *self = RECENTS_CONTENT (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL || for_size < 0)
    {
      *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? ROW_PAD * 2 + PHOTO : 0;
      return;
    }
  lay_out (self, for_size);
  *minimum = *natural = (int) ceil (content_height (self));
}

static void
recents_content_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  RecentsContent *self = RECENTS_CONTENT (widget);

  lay_out (self, width);
  if (self->call)
    gtk_widget_allocate (self->call, BUTTON, BUTTON, -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (width - ROW_PAD - BUTTON,
                                                                              (height - BUTTON) / 2.0)));
}

static void
recents_content_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  RecentsContent *self = RECENTS_CONTENT (widget);
  const Group *group = self->group;
  double height = gtk_widget_get_height (widget), inner = height - 2 * ROW_PAD;
  double x = ROW_PAD + PHOTO + INFO_START, y, row_y;
  GdkRGBA name = text_rgb (COLOR_TEXT_PRIMARY);
  GdkRGBA date = text_rgb (group->unread ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECONDARY);
  GdkTexture *photo = group->photo ? avatar_photo (widget, group->uid, group->photo) : NULL;

  lay_out (self, gtk_widget_get_width (widget));
  /* the badge sits at the top, its picture under PHOTO_PAD_TOP of padding */
  avatar_snapshot (snapshot, widget, ROW_PAD + PHOTO / 2.0, ROW_PAD + PHOTO_PAD_TOP + (PHOTO - PHOTO_PAD_TOP) / 2.0,
                   PHOTO - PHOTO_PAD_TOP, group->identifier, group->is_contact ? group->name : NULL, photo);

  /* the text column is centred */
  y = ROW_PAD + (inner - self->text_height) / 2;
  text_draw (snapshot, self->name, NAME_STYLE (group->unread), x, y, &name);
  row_y = y + text_box_height (self->name, NAME_STYLE (group->unread)) + NAME_GAP;
  for (guint i = 0; i < group->n_types; i++)
    {
      const char *icon;
      guint32 color;
      GdkRGBA rgba;

      type_icon (group->types[i], &icon, &color);
      rgba = text_rgb (color);
      neo_snapshot_icon (snapshot, widget, icon, x + i * (TYPE_ICON + ICON_MARGIN),
                         row_y + (self->row_height - TYPE_ICON) / 2, TYPE_ICON, &rgba);
    }
  text_draw (snapshot, self->date, DATE_STYLE (group->unread), x + icons_width (group) + ICON_MARGIN,
             row_y + (self->row_height - text_box_height (self->date, DATE_STYLE (group->unread))) / 2, &date);
  if (self->call)
    gtk_widget_snapshot_child (widget, self->call, snapshot);
}

static void
recents_content_dispose (GObject *object)
{
  RecentsContent *self = RECENTS_CONTENT (object);

  g_clear_pointer (&self->call, gtk_widget_unparent);
  g_clear_object (&self->name);
  g_clear_object (&self->date);
  G_OBJECT_CLASS (recents_content_parent_class)->dispose (object);
}

static void
recents_content_class_init (RecentsContentClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = recents_content_dispose;
  widget_class->get_request_mode = recents_content_get_request_mode;
  widget_class->measure = recents_content_measure;
  widget_class->size_allocate = recents_content_size_allocate;
  widget_class->snapshot = recents_content_snapshot;
}

static void
recents_content_init (RecentsContent *self)
{
}

/* ---- a list item: the day's header and the card ---------------------------------------------------------- */

#define RECENTS_TYPE_ROW (recents_row_get_type ())
G_DECLARE_FINAL_TYPE (RecentsRow, recents_row, RECENTS, ROW, GtkWidget)

struct _RecentsRow
{
  GtkWidget parent;
  RecentsPage *page;
  Group *group;
  PangoLayout *header;
  GtkWidget *primary;           /* a NeoPressable around the content */
  RecentsContent *content;
  GtkWidget *actions;           /* the actions block, made on the first expand */
  gboolean expanded;
  NeoAnim *anim;                /* 0 … 1 over the 200 ms, linear: the curves are applied per value */
  double height_progress, color_progress;
  double press_x, press_y;      /* where the last touch went down: the context menu opens there */
};

G_DEFINE_FINAL_TYPE (RecentsRow, recents_row, GTK_TYPE_WIDGET)

static void recents_row_set_expanded (RecentsRow *self, gboolean expanded, gboolean animate);
static void page_row_clicked (RecentsPage *page, RecentsRow *row);
static void page_row_menu (RecentsPage *page, RecentsRow *row);
static void page_call (RecentsPage *page, const char *number);
static void page_block (RecentsPage *page, RecentsRow *row, gboolean block);
static void page_show_details (RecentsPage *page, RecentsRow *row);
static gboolean page_is_blocked (RecentsPage *page, const char *number);

static double
header_height (RecentsRow *self)
{
  return self->header ? HEADER_PAD_TOP + text_box_height (self->header, HEADER_STYLE) + HEADER_PAD_BOTTOM : 0;
}

static int
actions_height (RecentsRow *self, int width)
{
  int min, nat;

  if (!self->actions)
    return 0;
  gtk_widget_measure (self->actions, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  return nat;
}

static int
primary_height (RecentsRow *self, int width)
{
  int min, nat;

  gtk_widget_measure (self->primary, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  return nat;
}

static GtkSizeRequestMode
recents_row_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
recents_row_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                     int *minimum_baseline, int *natural_baseline)
{
  RecentsRow *self = RECENTS_ROW (widget);
  int card = for_size - 2 * CARD_MARGIN;

  if (orientation == GTK_ORIENTATION_HORIZONTAL || for_size < 0)
    {
      *minimum = *natural = 0;
      return;
    }
  /* the card's height animates between its two sizes; the actions only show at the end of an expand */
  *minimum = *natural = (int) ceil (header_height (self) + CARD_MARGIN + primary_height (self, card)
                                    + self->height_progress * actions_height (self, card) + CARD_MARGIN);
}

static void
recents_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  RecentsRow *self = RECENTS_ROW (widget);
  int card = width - 2 * CARD_MARGIN;
  double top = header_height (self) + CARD_MARGIN;
  int primary = primary_height (self, card);

  gtk_widget_allocate (self->primary, card, primary, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (CARD_MARGIN, top)));
  if (self->actions)
    gtk_widget_allocate (self->actions, card, actions_height (self, card), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (CARD_MARGIN, top + primary)));
}

static void
recents_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  RecentsRow *self = RECENTS_ROW (widget);
  double top = header_height (self) + CARD_MARGIN;
  graphene_rect_t card = GRAPHENE_RECT_INIT (CARD_MARGIN, top, gtk_widget_get_width (widget) - 2 * CARD_MARGIN,
                                             gtk_widget_get_height (widget) - top - CARD_MARGIN);
  GskRoundedRect outline;

  if (self->header)
    {
      GdkRGBA color = text_rgb (COLOR_TEXT_SECONDARY);

      text_draw (snapshot, self->header, HEADER_STYLE, HEADER_START, HEADER_PAD_TOP, &color);
    }
  /* the card clips what it holds to its outline: the actions go under its rounded foot as it shrinks */
  gsk_rounded_rect_init_from_rect (&outline, &card, CARD_RADIUS);
  gtk_snapshot_push_rounded_clip (snapshot, &outline);
  if (self->color_progress > 0)
    {
      GdkRGBA background = text_rgb (CARD_EXPANDED);

      background.alpha = self->color_progress;
      gtk_snapshot_append_color (snapshot, &background, &card);
    }
  gtk_widget_snapshot_child (widget, self->primary, snapshot);
  if (self->actions && gtk_widget_get_child_visible (self->actions))
    gtk_widget_snapshot_child (widget, self->actions, snapshot);
  gtk_snapshot_pop (snapshot);
}

/* ---- expanding ---------------------------------------------------------------------------------------------- */

static void
on_send_message (RecentsRow *self)
{
  g_autofree char *uri = g_strconcat ("sms:", self->group->number, NULL);
  g_autoptr(GError) error = NULL;

  if (!g_app_info_launch_default_for_uri (uri, NULL, &error))
    g_warning ("recents: %s: %s", uri, error->message);
}

/* "Add contact": the number to a new contact, or to one picked in the Contacts app */
static void
on_create_contact (RecentsRow *self)
{
  contacts_app_insert (GTK_WIDGET (self), self->group->number);
}

static void
on_add_to_contact (RecentsRow *self)
{
  contacts_app_insert_or_edit (GTK_WIDGET (self), self->group->number);
}

static void
on_block (RecentsRow *self)
{
  page_block (self->page, self, TRUE);
}

static void
on_unblock (RecentsRow *self)
{
  page_block (self->page, self, FALSE);
}

static void
on_details (RecentsRow *self)
{
  page_show_details (self->page, self);
}

static GtkWidget *
action_row (RecentsRow *self, const char *icon_name, const char *text, GCallback callback)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  GtkWidget *label = gtk_label_new (text);
  GtkWidget *row;

  gtk_widget_set_size_request (box, -1, ACTION_HEIGHT);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), ACTION_ICON);
  gtk_widget_set_margin_start (icon, ACTION_ICON_START);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, ACTION_TEXT_PAD);
  gtk_widget_add_css_class (label, "dialer-primary-text");
  neo_label_set_font_padding (label, 16);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  g_signal_connect_swapped (row, "clicked", callback, self);
  return row;
}

/* the action buttons, for what this phone can back: no video calls, no call subjects, no voicemail.
 * An emergency number has "Call details" alone; a number that is not a contact can become one (or be added to
 * one) where the Contacts app is there to take it; a number can be texted and blocked or unblocked unless it is
 * blocked (texting) or withheld; "Call details" always */
static void
make_actions (RecentsRow *self)
{
  const Group *group = self->group;
  GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  g_autoptr(GAppInfo) sms = g_app_info_get_default_for_uri_scheme ("sms");
  gboolean emergency = group->callable && phone_number_is_emergency (group->number);
  gboolean blocked = group->callable && page_is_blocked (self->page, group->number);

  self->actions = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (divider, "recents-divider");
  gtk_box_append (GTK_BOX (self->actions), divider);
  if (!emergency)
    {
      if (group->callable && !group->is_contact && !blocked && contacts_app_installed ())
        {
          gtk_box_append (GTK_BOX (self->actions), action_row (self, "dialer-person-add-symbolic",
                                                               _("Create new contact"),
                                                               G_CALLBACK (on_create_contact)));
          gtk_box_append (GTK_BOX (self->actions), action_row (self, "dialer-person-symbolic", _("Add to a contact"),
                                                               G_CALLBACK (on_add_to_contact)));
        }
      if (group->callable && !blocked && sms)
        gtk_box_append (GTK_BOX (self->actions), action_row (self, "dialer-message-symbolic", _("Send a message"),
                                                             G_CALLBACK (on_send_message)));
      if (group->callable)
        gtk_box_append (GTK_BOX (self->actions),
                        blocked ? action_row (self, "dialer-unblock-symbolic", _("Unblock number"),
                                              G_CALLBACK (on_unblock))
                                : action_row (self, "dialer-block-symbolic", _("Block number"), G_CALLBACK (on_block)));
    }
  {
    GtkWidget *details = action_row (self, "dialer-info-symbolic", _("Call details"), G_CALLBACK (on_details));
    g_autofree char *description = g_strdup_printf (_("Call details for %s"), group->name);

    gtk_accessible_update_property (GTK_ACCESSIBLE (details), GTK_ACCESSIBLE_PROPERTY_LABEL, description, -1);
    gtk_box_append (GTK_BOX (self->actions), details);
  }
  gtk_widget_set_parent (self->actions, GTK_WIDGET (self));
  gtk_widget_set_child_visible (self->actions, self->expanded && !neo_anim_is_running (self->anim));
}

/* the number was blocked or unblocked, or the Contacts app came or went: an open entry's actions are made again */
static void
recents_row_remake_actions (RecentsRow *self)
{
  if (!self->actions)
    return;
  g_clear_pointer (&self->actions, gtk_widget_unparent);
  if (self->expanded)
    make_actions (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
on_anim (NeoAnim *anim, double value, gpointer data)
{
  RecentsRow *self = data;

  /* expanding: the height and the colour accelerate-decelerate; collapsing: the height decelerating, the colour
   * accelerate-decelerate too */
  self->height_progress = self->expanded ? spongy_curve (SPONGY_ACCELERATE_DECELERATE, value)
                                         : 1 - spongy_curve (SPONGY_DECELERATE, value);
  self->color_progress = self->expanded ? spongy_curve (SPONGY_ACCELERATE_DECELERATE, value)
                                        : 1 - spongy_curve (SPONGY_ACCELERATE_DECELERATE, value);
  /* the actions appear when the card has grown, and go when it has shrunk */
  if (self->actions)
    gtk_widget_set_child_visible (self->actions, self->expanded ? !neo_anim_is_running (anim) : value < 1);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
recents_row_set_expanded (RecentsRow *self, gboolean expanded, gboolean animate)
{
  if (expanded == self->expanded && (animate || !neo_anim_is_running (self->anim)))
    return;
  self->expanded = expanded;
  if (expanded && !self->actions)
    make_actions (self);
  if (animate && gtk_widget_get_mapped (GTK_WIDGET (self)))
    neo_anim_start (self->anim, 0, 1, EXPAND_MS, 0, SPONGY_LINEAR);
  else
    {
      neo_anim_stop (self->anim);
      self->height_progress = self->color_progress = expanded;
      if (self->actions)
        gtk_widget_set_child_visible (self->actions, expanded);
      gtk_widget_queue_resize (GTK_WIDGET (self));
    }
}

/* a tap on a contact's picture opens the contact's card; anywhere else on the entry opens it */
static void
on_primary_clicked (RecentsRow *self)
{
  if (self->group->is_contact && contacts_app_installed ()
      && self->press_x >= ROW_PAD && self->press_x < ROW_PAD + PHOTO
      && self->press_y >= ROW_PAD && self->press_y < ROW_PAD + PHOTO)
    {
      contacts_app_view (GTK_WIDGET (self), self->group->uid, NULL);
      return;
    }
  page_row_clicked (self->page, self);
}

static void
on_call_clicked (RecentsRow *self)
{
  page_call (self->page, self->group->number);
}

static void
on_primary_long_pressed (RecentsRow *self)
{
  page_row_menu (self->page, self);
}

static void
on_primary_pressed (GtkGestureClick *click, int n_press, double x, double y, RecentsRow *self)
{
  self->press_x = x;
  self->press_y = y;
}

/* the row shows @group from now on (a rebind: the log or the contacts changed) */
static void
recents_row_set_group (RecentsRow *self, Group *group)
{
  RecentsContent *content = self->content;

  group_free (self->group);
  self->group = group;
  content->group = group;
  content->laid_out = 0;
  g_clear_object (&self->header);
  if (group->header)
    self->header = text_layout (GTK_WIDGET (self), HEADER_STYLE, call_dates_day_group_name (group->day), 0);
  if (group->callable && !content->call)
    {
      /* the call button, borderless */
      content->call = neo_icon_button_new ("dialer-call-symbolic");
      gtk_widget_add_css_class (content->call, "dialer-icon");
      neo_pressable_set_ripple_color (NEO_PRESSABLE (content->call), &(GdkRGBA) { 1, 1, 1, 0.06 });
      gtk_widget_set_parent (content->call, GTK_WIDGET (content));
      g_signal_connect_swapped (content->call, "clicked", G_CALLBACK (on_call_clicked), self);
    }
  else if (!group->callable && content->call)
    g_clear_pointer (&content->call, gtk_widget_unparent);
  if (content->call)
    {
      g_autofree char *description = g_strdup_printf (_("Call %s"), group->name);

      gtk_accessible_update_property (GTK_ACCESSIBLE (content->call), GTK_ACCESSIBLE_PROPERTY_LABEL, description,
                                      -1);
    }
  /* the actions depend on the number, and on whether it is a contact or blocked: made again (an open entry's at
   * once, the others' on their next expand) */
  recents_row_remake_actions (self);
  gtk_widget_queue_resize (GTK_WIDGET (content));
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
recents_row_dispose (GObject *object)
{
  RecentsRow *self = RECENTS_ROW (object);

  g_clear_pointer (&self->anim, neo_anim_free);
  g_clear_pointer (&self->primary, gtk_widget_unparent);
  g_clear_pointer (&self->actions, gtk_widget_unparent);
  g_clear_object (&self->header);
  g_clear_pointer (&self->group, group_free);
  G_OBJECT_CLASS (recents_row_parent_class)->dispose (object);
}

static void
recents_row_class_init (RecentsRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = recents_row_dispose;
  widget_class->get_request_mode = recents_row_get_request_mode;
  widget_class->measure = recents_row_measure;
  widget_class->size_allocate = recents_row_size_allocate;
  widget_class->snapshot = recents_row_snapshot;
  gtk_widget_class_set_css_name (widget_class, "recents-row");
}

static void
recents_row_init (RecentsRow *self)
{
  GtkGesture *press;

  self->content = g_object_new (RECENTS_TYPE_CONTENT, NULL);
  self->primary = neo_pressable_new (GTK_WIDGET (self->content));
  /* the press ripple, inside the card's corners */
  neo_pressable_set_ripple (NEO_PRESSABLE (self->primary), NEO_RIPPLE_BOUNDED, CARD_RADIUS);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (self->primary), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_widget_set_parent (self->primary, GTK_WIDGET (self));
  g_signal_connect_swapped (self->primary, "clicked", G_CALLBACK (on_primary_clicked), self);
  g_signal_connect_swapped (self->primary, "long-pressed", G_CALLBACK (on_primary_long_pressed), self);
  /* only watches: the pressable takes the touch */
  press = gtk_gesture_click_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (press), GTK_PHASE_CAPTURE);
  g_signal_connect (press, "pressed", G_CALLBACK (on_primary_pressed), self);
  gtk_widget_add_controller (self->primary, GTK_EVENT_CONTROLLER (press));
  self->anim = neo_anim_new (GTK_WIDGET (self), on_anim, self);
}

static RecentsRow *
recents_row_new (RecentsPage *page, Group *group)
{
  RecentsRow *self = g_object_new (RECENTS_TYPE_ROW, NULL);

  self->page = page;
  recents_row_set_group (self, group);
  return self;
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

struct _RecentsPage
{
  GtkWidget parent;
  Contacts *contacts;
  NeoSnackbar *snackbar;
  GtkWidget *stack, *scroller, *list, *empty;
  GPtrArray *groups;            /* Group, not yet given to a row */
  guint n_rows;
  guint build_id;               /* the idle making the rest of the rows */
  GCancellable *loading;
  gboolean reload;              /* the log changed while it loaded */
  gint64 expanded_id;           /* the open entry's newest call, or 0 */
  gboolean missed_only;         /* Call history's Missed page */
  gboolean has_calls;           /* the log had calls at the last load, missed or not */
};

G_DEFINE_FINAL_TYPE (RecentsPage, recents_page, GTK_TYPE_WIDGET)

enum { OPEN_DIALPAD, EDIT_NUMBER, LOADED, SHOW_DETAILS, N_SIGNALS };
static guint signals[N_SIGNALS];

static void load (RecentsPage *self);

/* the groups from @from on become rows: the rows already there are rebound, new ones made, the rest dropped */
static gboolean
build_rows (gpointer data)
{
  RecentsPage *self = data;
  GtkWidget *child = gtk_widget_get_first_child (self->list);
  guint limit = self->n_rows ? self->n_rows + CHUNK_ROWS : FIRST_ROWS;
  guint i = 0;

  self->build_id = 0;
  for (; i < self->groups->len && i < limit; i++)
    {
      Group *group = g_steal_pointer (&self->groups->pdata[i]);
      gboolean expanded;

      if (!group)
        {
          child = child ? gtk_widget_get_next_sibling (child) : NULL;
          continue;
        }
      expanded = group_id (group) == self->expanded_id;
      if (child)
        {
          recents_row_set_group (RECENTS_ROW (child), group);
          recents_row_set_expanded (RECENTS_ROW (child), expanded, FALSE);
          child = gtk_widget_get_next_sibling (child);
        }
      else
        {
          RecentsRow *row = recents_row_new (self, group);

          recents_row_set_expanded (row, expanded, FALSE);
          gtk_box_append (GTK_BOX (self->list), GTK_WIDGET (row));
        }
    }
  self->n_rows = i;
  if (i < self->groups->len)
    {
      self->build_id = g_idle_add (build_rows, self);
      return G_SOURCE_REMOVE;
    }
  /* all made: the rows of entries that are gone go too */
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_box_remove (GTK_BOX (self->list), child);
      child = next;
    }
  return G_SOURCE_REMOVE;
}

static void
show_groups (RecentsPage *self, GPtrArray *groups)
{
  g_clear_handle_id (&self->build_id, g_source_remove);
  g_clear_pointer (&self->groups, g_ptr_array_unref);
  self->groups = groups;
  self->n_rows = 0;
  gtk_stack_set_visible_child (GTK_STACK (self->stack), groups->len ? self->scroller : self->empty);
  /* a rebind of the rows on screen at once, the rest while idle: the first frame does not wait for a long log */
  build_rows (self);
}

static void
on_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  RecentsPage *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) entries = calls_client_get_call_log_finish (CALLS_CLIENT (source), result, &error);
  const char *country;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->loading);
  if (!entries)
    g_warning ("recents: %s", error->message);
  else
    {
      self->has_calls = entries->len > 0;
      /* the missed calls alone, grouped among themselves */
      for (guint i = entries->len; self->missed_only && i-- > 0;)
        if (((CallLogEntry *) entries->pdata[i])->type != CALL_TYPE_MISSED)
          g_ptr_array_remove_index (entries, i);
      country = calls_client_get_country (calls_client_get ());
      show_groups (self, make_groups (entries, self->contacts, country ? country : phone_number_locale_country ()));
      g_signal_emit (self, signals[LOADED], 0);
    }
  if (self->reload)
    load (self);
}

static void
load (RecentsPage *self)
{
  if (self->loading)
    {
      self->reload = TRUE;
      return;
    }
  self->reload = FALSE;
  self->loading = g_cancellable_new ();
  calls_client_get_call_log (calls_client_get (), self->loading, on_loaded, self);
}

static void
on_log_changed (RecentsPage *self)
{
  load (self);
}

/* a tap on an entry: it opens and the one open before closes */
static void
page_row_clicked (RecentsPage *self, RecentsRow *row)
{
  gint64 id = group_id (row->group);

  if (row->expanded)
    {
      recents_row_set_expanded (row, FALSE, TRUE);
      self->expanded_id = 0;
      return;
    }
  for (GtkWidget *child = gtk_widget_get_first_child (self->list); child; child = gtk_widget_get_next_sibling (child))
    if (RECENTS_ROW (child)->expanded)
      recents_row_set_expanded (RECENTS_ROW (child), FALSE, TRUE);
  /* opening a missed call marks it seen */
  if (row->group->types[0] == CALL_TYPE_MISSED && row->group->unread)
    calls_client_mark_calls_read (calls_client_get (), (gint64 *) row->group->ids->data, row->group->ids->len);
  recents_row_set_expanded (row, TRUE, TRUE);
  self->expanded_id = id;
}

/* ---- the context menu ------------------------------------------------------------------------------------------ */

typedef struct
{
  RecentsPage *page;
  char *number;
  GArray *ids;
} MenuTarget;

static void
menu_target_free (gpointer data)
{
  MenuTarget *target = data;

  g_object_unref (target->page);
  g_free (target->number);
  g_array_unref (target->ids);
  g_free (target);
}

static MenuTarget *
menu_target_copy (const MenuTarget *target)
{
  MenuTarget *copy = g_new0 (MenuTarget, 1);

  copy->page = g_object_ref (target->page);
  copy->number = g_strdup (target->number);
  copy->ids = g_array_ref (target->ids);
  return copy;
}

static void
on_menu_copy (gpointer data)
{
  MenuTarget *target = data;

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (target->page)), target->number);
}

static void
on_menu_edit (gpointer data)
{
  MenuTarget *target = data;

  g_signal_emit (target->page, signals[EDIT_NUMBER], 0, target->number);
}

/* every call of the entry goes at once: no confirmation, no undo */
static void
on_menu_delete (gpointer data)
{
  MenuTarget *target = data;

  calls_client_delete_call_log (calls_client_get (), (gint64 *) target->ids->data, target->ids->len);
}

static const char *
menu_country (void)
{
  const char *country = calls_client_get_country (calls_client_get ());

  return country && *country ? country : phone_number_locale_country ();
}

static void
on_block_response (NeoDialog *dialog, int response, gpointer data)
{
  MenuTarget *target = data;
  g_autoptr(GError) error = NULL;

  if (response == NEO_RESPONSE_OK
      && !blocked_numbers_add (blocked_numbers_get_default (), target->number, menu_country (), &error))
    {
      g_warning ("recents: %s", error->message);
      neo_snackbar_show (target->page->snackbar, _("Problem blocking this number"), NULL, NULL, NULL,
                         NEO_SNACKBAR_LONG);
    }
}

static void
on_unblock_response (NeoDialog *dialog, int response, gpointer data)
{
  MenuTarget *target = data;
  g_autoptr(GError) error = NULL;

  if (response == NEO_RESPONSE_OK
      && !blocked_numbers_remove (blocked_numbers_get_default (), target->number, menu_country (), &error))
    {
      g_warning ("recents: %s", error->message);
      neo_snackbar_show (target->page->snackbar, _("Problem unblocking this number"), NULL, NULL, NULL,
                         NEO_SNACKBAR_LONG);
    }
}

/* "Block %s?": into the list the Messaging shares, so it takes texts too */
static void
on_menu_block (gpointer data)
{
  MenuTarget *target = data;
  g_autofree char *shown = phone_number_format (target->number, menu_country ());
  g_autofree char *title = g_strdup_printf (_("Block %s?"), shown);
  NeoDialog *dialog = neo_dialog_new (title);

  neo_dialog_set_message (dialog, _("You will no longer receive calls or texts from this number."));
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("BLOCK"), NEO_RESPONSE_OK);
  /* the target lives with the menu, which goes before the dialog answers: the dialog holds its own */
  g_object_set_data_full (G_OBJECT (dialog), "menu-target", menu_target_copy (target), menu_target_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_block_response),
                    g_object_get_data (G_OBJECT (dialog), "menu-target"));
  neo_dialog_present (dialog, GTK_WIDGET (target->page));
}

/* "Unblock …?": no title, the question is the message */
static void
on_menu_unblock (gpointer data)
{
  MenuTarget *target = data;
  g_autofree char *shown = phone_number_format (target->number, menu_country ());
  g_autofree char *question = g_strdup_printf (_("Unblock %s?"), shown);
  NeoDialog *dialog = neo_dialog_new (NULL);

  neo_dialog_set_message (dialog, question);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("UNBLOCK"), NEO_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (dialog), "menu-target", menu_target_copy (target), menu_target_free);
  g_signal_connect (dialog, "response", G_CALLBACK (on_unblock_response),
                    g_object_get_data (G_OBJECT (dialog), "menu-target"));
  neo_dialog_present (dialog, GTK_WIDGET (target->page));
}

static gboolean
page_is_blocked (RecentsPage *self, const char *number)
{
  return blocked_numbers_contains (blocked_numbers_get_default (), number, menu_country ());
}

/* the expanded entry's "Block number" and "Unblock number": the context menu's dialogs */
static void
page_block (RecentsPage *self, RecentsRow *row, gboolean block)
{
  MenuTarget *target = g_new0 (MenuTarget, 1);

  target->page = g_object_ref (self);
  target->number = g_strdup (row->group->number);
  target->ids = g_array_ref (row->group->ids);
  if (block)
    on_menu_block (target);
  else
    on_menu_unblock (target);
  menu_target_free (target);
}

static void
page_show_details (RecentsPage *self, RecentsRow *row)
{
  g_signal_emit (self, signals[SHOW_DETAILS], 0, row->group->number, (int) row->group->presentation, row->group->ids);
}

/* the number as a header, then Copy number, Edit number before call, Block or Unblock number and Delete */
static void
page_row_menu (RecentsPage *self, RecentsRow *row)
{
  const Group *group = row->group;
  DialerPopupMenu *menu = dialer_popup_menu_new ();
  MenuTarget *target = g_new0 (MenuTarget, 1);
  const char *country = calls_client_get_country (calls_client_get ());
  g_autofree char *header = group->callable
    ? phone_number_format (group->number, country ? country : phone_number_locale_country ())
    : g_strdup (group->name);

  target->page = g_object_ref (self);
  target->number = g_strdup (group->number);
  target->ids = g_array_ref (group->ids);
  g_object_set_data_full (G_OBJECT (menu), "menu-target", target, menu_target_free);
  dialer_popup_menu_set_header (menu, header);
  if (*group->number)
    dialer_popup_menu_append (menu, _("Copy number"), on_menu_copy, target);
  if (group->callable && !strchr (group->number, '@'))
    dialer_popup_menu_append (menu, _("Edit number before call"), on_menu_edit, target);
  /* a withheld number has nothing to block, and an emergency number never is */
  if (group->callable && !phone_number_is_emergency (group->number))
    {
      if (blocked_numbers_contains (blocked_numbers_get_default (), group->number, menu_country ()))
        dialer_popup_menu_append (menu, _("Unblock number"), on_menu_unblock, target);
      else
        dialer_popup_menu_append (menu, _("Block number"), on_menu_block, target);
    }
  dialer_popup_menu_append (menu, _("Delete"), on_menu_delete, target);
  dialer_popup_menu_present_at (menu, row->primary, row->press_x, row->press_y);
}

static void
on_dialed (guint id, const GError *error, gpointer data)
{
  RecentsPage *self = data;

  if (error)
    neo_snackbar_show (self->snackbar, error->message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
  else
    phone_start_app (INCALL_APP_ID);
  g_object_unref (self);
}

/* the call-back button: the call goes out at once, the in-call screen comes up */
static void
page_call (RecentsPage *self, const char *number)
{
  calls_client_dial (calls_client_get (), number, on_dialed, g_object_ref (self));
}

static void
on_make_call (gpointer data)
{
  g_signal_emit (data, signals[OPEN_DIALPAD], 0);
}

/* the relative dates go stale while the page is away: shown again, it reads the log again */
static void
on_map (GtkWidget *widget, RecentsPage *self)
{
  load (self);
}

void
recents_page_reset (RecentsPage *self)
{
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller), 0);
}

gboolean
recents_page_has_calls (RecentsPage *self)
{
  return self->has_calls;
}

static void
recents_page_dispose (GObject *object)
{
  RecentsPage *self = RECENTS_PAGE (object);

  if (self->loading)
    g_cancellable_cancel (self->loading);
  g_clear_object (&self->loading);
  g_clear_handle_id (&self->build_id, g_source_remove);
  g_signal_handlers_disconnect_by_data (calls_client_get (), self);
  g_signal_handlers_disconnect_by_data (blocked_numbers_get_default (), self);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->groups, g_ptr_array_unref);
  g_clear_pointer (&self->stack, gtk_widget_unparent);
  G_OBJECT_CLASS (recents_page_parent_class)->dispose (object);
}

static void
recents_page_class_init (RecentsPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = recents_page_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "recents-page");
  signals[OPEN_DIALPAD] = g_signal_new ("open-dialpad", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
  signals[EDIT_NUMBER] = g_signal_new ("edit-number", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[LOADED] = g_signal_new ("loaded", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[SHOW_DETAILS] = g_signal_new ("show-details", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_ARRAY);
}

static void
recents_page_init (RecentsPage *self)
{
  self->stack = gtk_stack_new ();
  gtk_widget_set_parent (self->stack, GTK_WIDGET (self));

  /* the list: in from the sides, room at the foot, no scroll bar */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->list, LIST_PAD_X);
  gtk_widget_set_margin_end (self->list, LIST_PAD_X);
  gtk_widget_set_margin_bottom (self->list, LIST_PAD_BOTTOM);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_stack_add_child (GTK_STACK (self->stack), self->scroller);
  g_signal_connect (self, "map", G_CALLBACK (on_map), self);
}

static GtkWidget *
page_new (Contacts *contacts, NeoSnackbar *snackbar, gboolean missed_only, gboolean can_call)
{
  RecentsPage *self = g_object_new (RECENTS_TYPE_PAGE, NULL);

  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  self->missed_only = missed_only;
  /* the empty view: the clock picture, the line, and in the Recents tab "Make a call" (the dialpad);
   * Call history's pages have no action */
  self->empty = empty_view_new ("dialer-empty-call-log-symbolic",
                                missed_only ? _("You have no missed calls.") : _("Your call history is empty"),
                                can_call ? _("Make a call") : NULL, on_make_call, self);
  gtk_stack_add_child (GTK_STACK (self->stack), self->empty);
  g_signal_connect_swapped (calls_client_get (), "call-log-changed", G_CALLBACK (on_log_changed), self);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_log_changed), self);
  /* a number blocked or unblocked, here or in Messaging: the entries' actions and "Blocked" again */
  g_signal_connect_swapped (blocked_numbers_get_default (), "changed", G_CALLBACK (on_log_changed), self);
  load (self);
  return GTK_WIDGET (self);
}

GtkWidget *
recents_page_new (Contacts *contacts, NeoSnackbar *snackbar)
{
  return page_new (contacts, snackbar, FALSE, TRUE);
}

GtkWidget *
recents_page_new_history (Contacts *contacts, NeoSnackbar *snackbar, gboolean missed_only)
{
  return page_new (contacts, snackbar, missed_only, FALSE);
}
