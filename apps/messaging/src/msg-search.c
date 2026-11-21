/* The search page (msg-search.h). */
#include "msg-search.h"
#include "msg-avatar.h"
#include "msg-dates.h"
#include "msg-people.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define SIDE 16
#define PILL_TOP 8
#define PILL_H 48
#define BACK_INSET 4            /* the back arrow's inset in the pill */
#define TILE_H 52
#define TILE_GAP 4
#define TILE_ICON 20
#define TILE_ICON_X 16
#define TILE_TEXT_GAP 8         /* from the icon's box to the label */
#define GRID_TOP 16             /* from the pill to the tiles */
#define RESULT_H 72
#define RESULT_AVATAR 40

typedef enum {
  FILTER_NONE,
  FILTER_UNREAD,
  FILTER_KNOWN,
  FILTER_UNKNOWN,
  FILTER_LINKS,
  N_FILTERS
} Filter;

static const struct {
  const char *icon;
  const char *label;
} filters[N_FILTERS] = {
  [FILTER_UNREAD] = { "msg-mark-chat-unread-symbolic", N_("Unread") },
  [FILTER_KNOWN] = { "msg-person-symbolic", N_("Known") },
  [FILTER_UNKNOWN] = { "msg-person-off-symbolic", N_("Unknown") },
  [FILTER_LINKS] = { "msg-link-symbolic", N_("Links") },
};

struct _MsgSearch {
  GtkWidget parent;
  SmsClient *client;
  Contacts *contacts;
  GtkWidget *box, *field, *chip, *chip_label, *pages, *results, *none;
  Filter filter;
};

G_DEFINE_FINAL_TYPE (MsgSearch, msg_search, GTK_TYPE_WIDGET)

enum { OPEN_CONVERSATION, CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void update (MsgSearch *self);

/* --- matching */

static char *
fold (const char *text)
{
  g_autofree char *normal = g_utf8_normalize (text ? text : "", -1, G_NORMALIZE_NFD);
  g_autoptr(GString) folded = g_string_new (NULL);

  for (const char *p = normal; p && *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (!g_unichar_ismark (c))
        g_string_append_unichar (folded, g_unichar_tolower (c));
    }
  return g_string_free (g_steal_pointer (&folded), FALSE);
}

static gboolean
has_link (const char *text)
{
  g_autofree char *folded = fold (text);

  return strstr (folded, "http://") || strstr (folded, "https://") || strstr (folded, "www.");
}

/* --- the results */

static void
on_result_clicked (GtkWidget *row, MsgSearch *self)
{
  g_signal_emit (self, signals[OPEN_CONVERSATION], 0, *(gint64 *) g_object_get_data (G_OBJECT (row), "id"));
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

/* a conversation found: its avatar and name, the words found (or its newest), and when */
static GtkWidget *
make_result (MsgSearch *self, const SmsConversation *conversation, GPtrArray *people, const char *text,
             gint64 time)
{
  g_autofree char *name = msg_people_name (people);
  g_autofree char *when = msg_dates_conversation (time, g_get_real_time () / 1000);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *avatar = msg_avatar_new (RESULT_AVATAR);
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *name_label = text_label (name, "msg-row-name"), *row;
  MsgPerson *only = people->len == 1 ? people->pdata[0] : NULL;

  if (only)
    msg_avatar_set_person (MSG_AVATAR (avatar), only->name, only->identifier, only->photo);
  else
    msg_avatar_set_person (MSG_AVATAR (avatar), name, name, NULL);
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (avatar, SIDE);
  gtk_box_append (GTK_BOX (box), avatar);
  gtk_widget_set_hexpand (name_label, TRUE);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_grid_attach (GTK_GRID (grid), name_label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), text_label (when, "msg-row-time"), 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), text_label (text, "msg-row-snippet"), 0, 1, 2, 1);
  gtk_widget_set_hexpand (grid, TRUE);
  gtk_widget_set_valign (grid, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (grid, SIDE);
  gtk_widget_set_margin_end (grid, 24);
  gtk_box_append (GTK_BOX (box), grid);
  gtk_widget_set_size_request (box, -1, RESULT_H);
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 16);
  gtk_widget_add_css_class (row, "msg-search-result");
  g_object_set_data_full (G_OBJECT (row), "id", g_memdup2 (&conversation->id, sizeof conversation->id), g_free);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect (row, "clicked", G_CALLBACK (on_result_clicked), self);
  return row;
}

static gboolean
conversation_passes (MsgSearch *self, const SmsConversation *conversation, GPtrArray *people)
{
  gboolean known = FALSE;

  for (guint i = 0; i < people->len; i++)
    known |= ((MsgPerson *) people->pdata[i])->is_contact;
  switch (self->filter)
    {
    case FILTER_UNREAD:
      return !conversation->read;
    case FILTER_KNOWN:
      return known;
    case FILTER_UNKNOWN:
      return !known;
    default:
      return TRUE;
    }
}

static void
fill_results (MsgSearch *self, const char *query)
{
  SmsStore *store = sms_client_get_store (self->client);
  g_autofree char *folded = fold (query);
  gboolean by_message = *folded || self->filter == FILTER_LINKS;
  GtkWidget *child;
  guint found = 0;

  while ((child = gtk_widget_get_first_child (self->results)))
    gtk_box_remove (GTK_BOX (self->results), child);
  for (int archived = 0; store && archived < 2; archived++)
    {
      g_autoptr(GPtrArray) conversations = sms_store_list_conversations (store, archived);

      for (guint i = 0; i < conversations->len; i++)
        {
          SmsConversation *conversation = conversations->pdata[i];
          g_autoptr(GPtrArray) people = msg_people_new (self->contacts,
                                                        (const char * const *) conversation->destinations,
                                                        sms_client_get_country (self->client));

          if (!conversation_passes (self, conversation, people))
            continue;
          if (!by_message)
            {
              gtk_box_append (GTK_BOX (self->results),
                              make_result (self, conversation, people, conversation->snippet,
                                           conversation->sort_timestamp));
              found++;
              continue;
            }
          {
            g_autoptr(GPtrArray) messages = sms_store_list_messages (store, conversation->id);
            g_autofree char *people_name = msg_people_name (people);
            g_autofree char *name = fold (people_name);
            gboolean name_found = *folded && strstr (name, folded);

            /* the newest message that matches, one result for each conversation */
            for (guint m = messages->len; m > 0; m--)
              {
                SmsMessage *message = messages->pdata[m - 1];
                g_autofree char *text = fold (message->text);

                if ((*folded && !strstr (text, folded) && !name_found)
                    || (self->filter == FILTER_LINKS && !has_link (message->text)))
                  continue;
                gtk_box_append (GTK_BOX (self->results),
                                make_result (self, conversation, people, message->text,
                                             MESSAGE_STATUS_IS_INCOMING (message->status) ? message->received
                                                                                         : message->sent));
                found++;
                break;
              }
          }
        }
    }
  gtk_widget_set_visible (self->none, found == 0);
}

static void
update (MsgSearch *self)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->field))));
  gboolean searching = *text || self->filter != FILTER_NONE;

  gtk_widget_set_visible (self->chip, self->filter != FILTER_NONE);
  if (self->filter != FILTER_NONE)
    gtk_label_set_text (GTK_LABEL (self->chip_label), _(filters[self->filter].label));
  gtk_stack_set_visible_child_name (GTK_STACK (self->pages), searching ? "results" : "filters");
  if (searching)
    fill_results (self, text);
}

static void
on_tile_clicked (GtkWidget *tile, MsgSearch *self)
{
  self->filter = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tile), "filter"));
  update (self);
}

static void
on_chip_clicked (GtkWidget *chip, MsgSearch *self)
{
  self->filter = FILTER_NONE;
  update (self);
}

gboolean
msg_search_back (MsgSearch *self)
{
  if (self->filter != FILTER_NONE)
    {
      self->filter = FILTER_NONE;
      update (self);
      return TRUE;
    }
  return FALSE;
}

static void
on_back (MsgSearch *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

/* --- the page */

static GtkWidget *
make_tile (MsgSearch *self, Filter filter)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name (filters[filter].icon);
  GtkWidget *label = text_label (_(filters[filter].label), "msg-tile-label");
  GtkWidget *tile;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), TILE_ICON);
  gtk_widget_set_size_request (icon, TILE_ICON, 24);
  gtk_widget_set_margin_start (icon, TILE_ICON_X);
  gtk_widget_add_css_class (icon, "msg-tile-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_margin_start (label, TILE_TEXT_GAP);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_size_request (box, -1, TILE_H);
  tile = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (tile), VIBEOS_RIPPLE_BOUNDED, TILE_H / 2.0);
  gtk_widget_add_css_class (tile, "msg-tile");
  gtk_widget_set_overflow (tile, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_hexpand (tile, TRUE);
  gtk_widget_set_name (tile, "filter_tile");
  g_object_set_data (G_OBJECT (tile), "filter", GINT_TO_POINTER (filter));
  gtk_accessible_update_property (GTK_ACCESSIBLE (tile), GTK_ACCESSIBLE_PROPERTY_LABEL, _(filters[filter].label), -1);
  g_signal_connect (tile, "clicked", G_CALLBACK (on_tile_clicked), self);
  return tile;
}

static GtkWidget *
make_pill (MsgSearch *self)
{
  GtkWidget *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *back = vibeos_icon_button_new ("msg-arrow-back-symbolic");
  GtkWidget *close = gtk_image_new_from_icon_name ("msg-close-symbolic");

  gtk_accessible_update_property (GTK_ACCESSIBLE (back), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  gtk_widget_set_size_request (back, 48, 48);
  gtk_widget_set_margin_start (back, BACK_INSET);
  gtk_widget_add_css_class (back, "msg-bar-icon");
  g_signal_connect_swapped (back, "clicked", G_CALLBACK (on_back), self);
  gtk_box_append (GTK_BOX (pill), back);
  /* the filter picked, as a chip before the words; a tap takes it away */
  self->chip_label = gtk_label_new (NULL);
  {
    GtkWidget *chip_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

    gtk_image_set_pixel_size (GTK_IMAGE (close), 16);
    gtk_box_append (GTK_BOX (chip_box), self->chip_label);
    gtk_box_append (GTK_BOX (chip_box), close);
    self->chip = vibeos_pressable_new (chip_box);
    vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->chip), VIBEOS_RIPPLE_BOUNDED, 8);
    gtk_widget_add_css_class (self->chip, "msg-search-chip");
    gtk_widget_set_valign (self->chip, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end (self->chip, 8);
    gtk_widget_set_visible (self->chip, FALSE);
    g_signal_connect (self->chip, "clicked", G_CALLBACK (on_chip_clicked), self);
    gtk_box_append (GTK_BOX (pill), self->chip);
  }
  self->field = gtk_text_new ();
  gtk_text_set_placeholder_text (GTK_TEXT (self->field), _("Search messages"));
  gtk_widget_set_hexpand (self->field, TRUE);
  gtk_widget_set_valign (self->field, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (self->field, SIDE);
  gtk_widget_add_css_class (self->field, "msg-search-field");
  gtk_widget_set_name (self->field, "search_field");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->field), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Search messages"), -1);
  g_signal_connect_swapped (self->field, "changed", G_CALLBACK (update), self);
  gtk_box_append (GTK_BOX (pill), self->field);
  gtk_widget_add_css_class (pill, "msg-search-pill");
  gtk_widget_set_size_request (pill, -1, PILL_H);
  gtk_widget_set_margin_start (pill, SIDE);
  gtk_widget_set_margin_end (pill, SIDE);
  gtk_widget_set_margin_top (pill, PILL_TOP);
  return pill;
}

static GtkWidget *
make_filters (MsgSearch *self)
{
  GtkWidget *grid = gtk_grid_new ();
  int n = 0;

  gtk_grid_set_column_spacing (GTK_GRID (grid), TILE_GAP);
  gtk_grid_set_row_spacing (GTK_GRID (grid), TILE_GAP);
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  for (Filter filter = FILTER_UNREAD; filter < N_FILTERS; filter++, n++)
    gtk_grid_attach (GTK_GRID (grid), make_tile (self, filter), n % 2, n / 2, 1, 1);
  gtk_widget_set_margin_start (grid, SIDE);
  gtk_widget_set_margin_end (grid, SIDE);
  gtk_widget_set_margin_top (grid, GRID_TOP);
  gtk_widget_set_valign (grid, GTK_ALIGN_START);
  return grid;
}

static void
msg_search_dispose (GObject *object)
{
  MsgSearch *self = MSG_SEARCH (object);

  g_clear_object (&self->client);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (msg_search_parent_class)->dispose (object);
}

static gboolean
msg_search_grab_focus (GtkWidget *widget)
{
  return gtk_widget_grab_focus (MSG_SEARCH (widget)->field);
}

static void
msg_search_class_init (MsgSearchClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = msg_search_dispose;
  widget_class->grab_focus = msg_search_grab_focus;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "msg-search");
  signals[OPEN_CONVERSATION] = g_signal_new ("open-conversation", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}

static void
msg_search_init (MsgSearch *self)
{
}

GtkWidget *
msg_search_new (SmsClient *client, Contacts *contacts)
{
  MsgSearch *self = g_object_new (MSG_TYPE_SEARCH, NULL);
  GtkWidget *results_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = spongy_scroll_view_new ();

  self->client = g_object_ref (client);
  self->contacts = g_object_ref (contacts);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_add_css_class (self->box, "msg-search-page");
  gtk_box_append (GTK_BOX (self->box), make_pill (self));

  self->pages = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (self->pages), 150);
  gtk_stack_add_named (GTK_STACK (self->pages), make_filters (self), "filters");
  self->results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->results, 8);
  gtk_box_append (GTK_BOX (inner), self->results);
  self->none = gtk_label_new (_("No results"));
  gtk_widget_add_css_class (self->none, "msg-empty-text");
  gtk_widget_set_margin_top (self->none, 48);
  gtk_box_append (GTK_BOX (inner), self->none);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), inner);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (results_page), scroll);
  gtk_stack_add_named (GTK_STACK (self->pages), results_page, "results");
  gtk_widget_set_vexpand (self->pages, TRUE);
  /* the filters and the results under the field: in the list's pane on a tablet, a pane of their own as the list's
   * rows are (messaging.css) */
  gtk_widget_add_css_class (self->pages, "msg-search-content");
  gtk_widget_set_overflow (self->pages, GTK_OVERFLOW_HIDDEN);
  gtk_box_append (GTK_BOX (self->box), self->pages);
  return GTK_WIDGET (self);
}
