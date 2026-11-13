/* The Contacts tab: see contacts-page.h. A list of contacts under "Create new contact", each section's letter
 * anchored at the top while its rows scroll, and a fast scroller at the end. */
#include "contacts-page.h"
#include "avatar.h"
#include "contacts-app.h"
#include "empty-view.h"
#include "text.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>

#define LIST_PAD_BOTTOM 88      /* room at the foot, so the dialpad button covers no row */
#define ROW_HEIGHT      56
#define HEADER_WIDTH    56      /* the section letter's square */
#define TARGET_END      16      /* the row's touch area, in from the end */
#define TARGET_PAD      8       /* its padding above and below */
#define TARGET_START    16      /* its padding at the start */
#define PHOTO           40
#define NAME_START      16      /* the name's padding at both ends */
#define NAME_END        8
#define ADD_TOP         8       /* over the "Create new contact" row */
#define FIRST_ROWS      20      /* rows made at once; the rest come in chunks while idle */
#define CHUNK_ROWS      40

#define THUMB_WIDTH     4       /* the fast scroller's thumb */
#define THUMB_HEIGHT    32
#define THUMB_END       16      /* in from the end */
#define SCROLLER_MARGIN 8       /* the fast scroller's top and bottom margins */
#define TOUCH_WIDTH     20      /* the strip at the end that takes the fast scroller's drags */
#define BUBBLE          88      /* the letter shown beside the thumb while it is dragged */
#define THUMB_IDLE      0xa3a7ac
#define THUMB_DRAGGED   0x1f2728
#define BUBBLE_TEXT     0xdadce0

#define NAME_STYLE      ((TextStyle) { TEXT_GSF, 16, 700 })
#define BUBBLE_STYLE    ((TextStyle) { TEXT_GSF, 48, 700 })
#define HEADER_STYLE    ((TextStyle) { TEXT_GSF, 16, 1000 })    /* a section's letter, as heavy as it goes */

/* ---- an entry: what a row needs of a contact, which outlives the model's next "changed" --------------------- */

typedef struct
{
  char *uid;
  char *name;
  char *section;                /* "A", "#", "…" */
  GBytes *photo;
} Entry;

static void
entry_free (Entry *entry)
{
  if (!entry)
    return;
  g_free (entry->uid);
  g_free (entry->name);
  g_free (entry->section);
  g_clear_pointer (&entry->photo, g_bytes_unref);
  g_free (entry);
}

/* the address-book index for a Latin locale: a name's first letter without its accents, "#"
 * for a digit, and the underflow bucket "…" for anything else (a symbol, an emoji). ICU's AlphabeticIndex would
 * also fold some letters of other scripts together; the base letter is what it gives for these */
static char *
section_of (const char *name)
{
  gunichar c, base;
  g_autofree char *first = NULL, *decomposed = NULL;

  while (*name && g_unichar_isspace (g_utf8_get_char (name)))
    name = g_utf8_next_char (name);
  c = g_utf8_get_char (name);
  if (g_unichar_isdigit (c))
    return g_strdup ("#");
  if (!g_unichar_isalpha (c))
    return g_strdup ("…");
  first = g_ucs4_to_utf8 (&c, 1, NULL, NULL, NULL);
  decomposed = g_utf8_normalize (first, -1, G_NORMALIZE_NFD);
  base = g_unichar_toupper (g_utf8_get_char (decomposed));
  return g_ucs4_to_utf8 (&base, 1, NULL, NULL, NULL);
}

static Entry *
entry_new (const Contact *contact)
{
  Entry *entry = g_new0 (Entry, 1);

  entry->uid = g_strdup (contact->uid);
  entry->name = g_strdup (contact->name && *contact->name ? contact->name : "");
  /* the letter of the name the list is sorted by: under "Sort by: Last name", Anna Schmidt is under S */
  entry->section = section_of (contact->sort_name && *contact->sort_name ? contact->sort_name : "");
  entry->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
  return entry;
}

/* ---- a row: the section letter and the click target with the picture and the name -------------------------- */

#define CONTACTS_TYPE_CONTENT (contacts_content_get_type ())
G_DECLARE_FINAL_TYPE (ContactsContent, contacts_content, CONTACTS, CONTENT, GtkWidget)

struct _ContactsContent
{
  GtkWidget parent;
  const Entry *entry;
  PangoLayout *name;
  double laid_out;
};

G_DEFINE_FINAL_TYPE (ContactsContent, contacts_content, GTK_TYPE_WIDGET)

static void
contacts_content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                          int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? ROW_HEIGHT : 0;
}

static void
contacts_content_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  ContactsContent *self = CONTACTS_CONTENT (widget);
  const Entry *entry = self->entry;
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double x = TARGET_START + PHOTO + NAME_START;
  GdkRGBA color = text_rgb (COLOR_TEXT_PRIMARY);
  GdkTexture *photo = entry->photo ? avatar_photo (widget, entry->uid, entry->photo) : NULL;

  if (self->laid_out != width)
    {
      g_clear_object (&self->name);
      self->name = text_layout (widget, NAME_STYLE, entry->name, MAX (width - x - NAME_END, 1));
      self->laid_out = width;
    }
  avatar_snapshot (snapshot, widget, TARGET_START + PHOTO / 2.0, height / 2.0, PHOTO, entry->uid, entry->name,
                   photo);
  /* the name is centred in the row */
  text_draw (snapshot, self->name, NAME_STYLE, x, (height - text_box_height (self->name, NAME_STYLE)) / 2, &color);
}

static void
contacts_content_dispose (GObject *object)
{
  g_clear_object (&CONTACTS_CONTENT (object)->name);
  G_OBJECT_CLASS (contacts_content_parent_class)->dispose (object);
}

static void
contacts_content_class_init (ContactsContentClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = contacts_content_dispose;
  widget_class->measure = contacts_content_measure;
  widget_class->snapshot = contacts_content_snapshot;
}

static void
contacts_content_init (ContactsContent *self)
{
}

#define CONTACTS_TYPE_ROW (contacts_row_get_type ())
G_DECLARE_FINAL_TYPE (ContactsRow, contacts_row, CONTACTS, ROW, GtkWidget)

struct _ContactsRow
{
  GtkWidget parent;
  ContactsPage *page;
  Entry *entry;
  gboolean first;               /* the first row of its section */
  gboolean header_hidden;       /* the anchored header shows its letter instead */
  PangoLayout *header;
  GtkWidget *target;            /* a NeoPressable around the content */
  ContactsContent *content;
};

G_DEFINE_FINAL_TYPE (ContactsRow, contacts_row, GTK_TYPE_WIDGET)

static void page_open_contact (ContactsPage *page, const Entry *entry);

static void
contacts_row_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = orientation == GTK_ORIENTATION_VERTICAL ? ROW_HEIGHT : 0;
}

static void
contacts_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  ContactsRow *self = CONTACTS_ROW (widget);

  gtk_widget_allocate (self->target, MAX (width - HEADER_WIDTH - TARGET_END, 0), height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (HEADER_WIDTH, 0)));
}

static void
draw_section (GtkSnapshot *snapshot, PangoLayout *layout, double top)
{
  GdkRGBA color = text_rgb (COLOR_ACCENT_TEXT);
  PangoRectangle ink, logical;

  /* centred in its square, by its line box */
  pango_layout_get_pixel_extents (layout, &ink, &logical);
  text_draw (snapshot, layout, HEADER_STYLE, (HEADER_WIDTH - logical.width) / 2.0 - logical.x,
             top + (ROW_HEIGHT - text_box_height (layout, HEADER_STYLE)) / 2, &color);
}

static void
contacts_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  ContactsRow *self = CONTACTS_ROW (widget);

  if (self->first && !self->header_hidden)
    draw_section (snapshot, self->header, 0);
  gtk_widget_snapshot_child (widget, self->target, snapshot);
}

static void
on_target_clicked (ContactsRow *self)
{
  page_open_contact (self->page, self->entry);
}

static void
contacts_row_set_entry (ContactsRow *self, Entry *entry, gboolean first)
{
  entry_free (self->entry);
  self->entry = entry;
  self->first = first;
  self->header_hidden = FALSE;
  self->content->entry = entry;
  self->content->laid_out = 0;
  g_clear_object (&self->header);
  if (first)
    self->header = text_layout (GTK_WIDGET (self), HEADER_STYLE, entry->section, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->target), GTK_ACCESSIBLE_PROPERTY_LABEL, entry->name, -1);
  gtk_widget_queue_draw (GTK_WIDGET (self->content));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
contacts_row_set_header_hidden (ContactsRow *self, gboolean hidden)
{
  if (hidden == self->header_hidden)
    return;
  self->header_hidden = hidden;
  if (self->first)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
contacts_row_dispose (GObject *object)
{
  ContactsRow *self = CONTACTS_ROW (object);

  g_clear_pointer (&self->target, gtk_widget_unparent);
  g_clear_object (&self->header);
  g_clear_pointer (&self->entry, entry_free);
  G_OBJECT_CLASS (contacts_row_parent_class)->dispose (object);
}

static void
contacts_row_class_init (ContactsRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = contacts_row_dispose;
  widget_class->measure = contacts_row_measure;
  widget_class->size_allocate = contacts_row_size_allocate;
  widget_class->snapshot = contacts_row_snapshot;
  gtk_widget_class_set_css_name (widget_class, "contacts-row");
}

static void
contacts_row_init (ContactsRow *self)
{
  self->content = g_object_new (CONTACTS_TYPE_CONTENT, NULL);
  self->target = neo_pressable_new (GTK_WIDGET (self->content));
  /* the press ripple, over the touch area only */
  neo_pressable_set_ripple (NEO_PRESSABLE (self->target), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (self->target), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_widget_set_parent (self->target, GTK_WIDGET (self));
  g_signal_connect_swapped (self->target, "clicked", G_CALLBACK (on_target_clicked), self);
}

static ContactsRow *
contacts_row_new (ContactsPage *page, Entry *entry, gboolean first)
{
  ContactsRow *self = g_object_new (CONTACTS_TYPE_ROW, NULL);

  self->page = page;
  contacts_row_set_entry (self, entry, first);
  return self;
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

struct _ContactsPage
{
  GtkWidget parent;
  Contacts *contacts;
  NeoSnackbar *snackbar;
  GtkWidget *stack, *scroller, *list, *add, *empty;
  GPtrArray *entries;           /* Entry, not yet given to a row */
  guint n_rows;
  guint build_id;
  PangoLayout *anchored;        /* the pinned letter, or NULL while the rows show their own */
  char *anchored_section;
  ContactsRow *hidden[2];       /* the rows whose letter the pinned one stands for */
  gboolean picker;
  gboolean empty_creates;       /* the empty view has "Create new contact" */
  gboolean dragging;            /* the fast scroller's thumb is held */
  PangoLayout *bubble;          /* its letter while it is */
};

G_DEFINE_FINAL_TYPE (ContactsPage, contacts_page, GTK_TYPE_WIDGET)

enum { PICKED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* The anchored header: when the first visible row and the first completely visible
 * one are in the same section, the letter is pinned at the top and those rows' own letters hide; otherwise the
 * rows show theirs, so the next letter pushes the last one out. The "Create new contact" row does not count. */
static ContactsRow *
row_at (ContactsPage *self, int index)
{
  GtkWidget *child = gtk_widget_get_next_sibling (self->add);

  for (int i = 0; child && i < index; i++)
    child = gtk_widget_get_next_sibling (child);
  return (ContactsRow *) child;
}

static void
update_anchored (ContactsPage *self)
{
  /* the rows are all ROW_HEIGHT, under the add row and its ADD_TOP margin (a picker has none) */
  double y = spongy_scroll_view_get_offset (SPONGY_SCROLL_VIEW (self->scroller))
             - (gtk_widget_get_visible (self->add) ? ADD_TOP + ROW_HEIGHT : 0);
  ContactsRow *visible = NULL, *complete = NULL;
  const char *section = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (self->hidden); i++)
    if (self->hidden[i])
      contacts_row_set_header_hidden (g_steal_pointer (&self->hidden[i]), FALSE);
  if (y > 0)
    {
      visible = row_at (self, (int) floor (y / ROW_HEIGHT));
      complete = row_at (self, (int) ceil (y / ROW_HEIGHT));
    }
  if (visible && complete && visible != complete && g_str_equal (visible->entry->section, complete->entry->section))
    {
      section = visible->entry->section;
      self->hidden[0] = visible;
      self->hidden[1] = complete;
      contacts_row_set_header_hidden (visible, TRUE);
      contacts_row_set_header_hidden (complete, TRUE);
    }
  if (g_strcmp0 (section, self->anchored_section) == 0)
    return;
  g_free (self->anchored_section);
  self->anchored_section = g_strdup (section);
  g_clear_object (&self->anchored);
  if (section)
    self->anchored = text_layout (GTK_WIDGET (self), HEADER_STYLE, section, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- the fast scroller ------------------------------------------------------------------------------------- */

static GtkAdjustment *
vadjustment (ContactsPage *self)
{
  return spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller));
}

/* the thumb's top: down the track as far as the list is scrolled */
static double
thumb_top (ContactsPage *self)
{
  GtkAdjustment *adj = vadjustment (self);
  double track = gtk_widget_get_height (GTK_WIDGET (self)) - 2 * SCROLLER_MARGIN;
  double range = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  double proportion = range > 0 ? CLAMP (gtk_adjustment_get_value (adj) / range, 0, 1) : 0;

  return SCROLLER_MARGIN + proportion * (track - THUMB_HEIGHT);
}

static gboolean
scroller_shown (ContactsPage *self)
{
  GtkAdjustment *adj = vadjustment (self);

  return gtk_stack_get_visible_child (GTK_STACK (self->stack)) == self->scroller
         && gtk_adjustment_get_upper (adj) > gtk_adjustment_get_page_size (adj);
}

static void
draw_fast_scroller (ContactsPage *self, GtkSnapshot *snapshot)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  double top = thumb_top (self);
  graphene_rect_t thumb = GRAPHENE_RECT_INIT (width - THUMB_END - THUMB_WIDTH, top, THUMB_WIDTH, THUMB_HEIGHT);
  GskRoundedRect shape;
  GdkRGBA color = text_rgb (self->dragging ? THUMB_DRAGGED : THUMB_IDLE);

  gsk_rounded_rect_init_from_rect (&shape, &thumb, 2);
  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &color, &thumb);
  gtk_snapshot_pop (snapshot);

  /* the bubble: a teardrop pointing at the thumb, left of it, its foot level with the thumb's centre */
  if (self->dragging && self->bubble)
    {
      graphene_rect_t box = GRAPHENE_RECT_INIT (thumb.origin.x - BUBBLE, top + THUMB_HEIGHT / 2.0 - BUBBLE, BUBBLE,
                                                BUBBLE);
      graphene_size_t round = GRAPHENE_SIZE_INIT (BUBBLE / 2.0, BUBBLE / 2.0), square = GRAPHENE_SIZE_INIT (0, 0);
      GdkRGBA fill = text_rgb (THUMB_DRAGGED), ink = text_rgb (BUBBLE_TEXT);
      PangoRectangle logical;

      box.origin.y = MAX (box.origin.y, 0);
      gsk_rounded_rect_init (&shape, &box, &round, &round, &square, &round);
      gtk_snapshot_push_rounded_clip (snapshot, &shape);
      gtk_snapshot_append_color (snapshot, &fill, &box);
      gtk_snapshot_pop (snapshot);
      pango_layout_get_pixel_extents (self->bubble, NULL, &logical);
      text_draw (snapshot, self->bubble, BUBBLE_STYLE, box.origin.x + (BUBBLE - logical.width) / 2.0 - logical.x,
                 box.origin.y + (BUBBLE - text_box_height (self->bubble, BUBBLE_STYLE)) / 2, &ink);
    }
}

/* the row under @y of the track: the list scrolls to put it on top */
static void
drag_to (ContactsPage *self, double y)
{
  double track = gtk_widget_get_height (GTK_WIDGET (self));
  guint rows = self->n_rows;
  double first = gtk_widget_get_visible (self->add) ? ADD_TOP + ROW_HEIGHT : 0;
  guint index;
  ContactsRow *row;
  GtkAdjustment *adj = vadjustment (self);

  if (!rows || track <= 0)
    return;
  index = MIN ((guint) (CLAMP (y / track, 0, 1) * rows), rows - 1);
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller),
                                MIN (first + index * ROW_HEIGHT,
                                     gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj)));
  row = row_at (self, index);
  g_clear_object (&self->bubble);
  if (row)
    self->bubble = text_layout (GTK_WIDGET (self), BUBBLE_STYLE, row->entry->section, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, ContactsPage *self)
{
  if (!scroller_shown (self) || x < gtk_widget_get_width (GTK_WIDGET (self)) - TOUCH_WIDTH)
    {
      gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
  self->dragging = TRUE;
  drag_to (self, y);
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, ContactsPage *self)
{
  double x, y;

  if (self->dragging && gtk_gesture_drag_get_start_point (drag, &x, &y))
    drag_to (self, y + dy);
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, ContactsPage *self)
{
  self->dragging = FALSE;
  g_clear_object (&self->bubble);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
contacts_page_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  ContactsPage *self = CONTACTS_PAGE (widget);

  gtk_widget_snapshot_child (widget, self->stack, snapshot);
  /* the pinned header, over the list at its top-left */
  if (self->anchored && gtk_stack_get_visible_child (GTK_STACK (self->stack)) == self->scroller)
    draw_section (snapshot, self->anchored, 0);
  if (scroller_shown (self))
    draw_fast_scroller (self, snapshot);
}

static void
on_scrolled (ContactsPage *self)
{
  update_anchored (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
build_rows (gpointer data)
{
  ContactsPage *self = data;
  GtkWidget *child = gtk_widget_get_next_sibling (self->add);
  guint limit = self->n_rows ? self->n_rows + CHUNK_ROWS : FIRST_ROWS;
  const char *previous = NULL;
  guint i = 0;

  self->build_id = 0;
  /* the rows already made keep theirs; the section of the row before the first new one says if it starts one */
  for (; i < self->n_rows && child; i++)
    {
      previous = CONTACTS_ROW (child)->entry->section;
      child = gtk_widget_get_next_sibling (child);
    }
  for (; i < self->entries->len && i < limit; i++)
    {
      Entry *entry = g_steal_pointer (&self->entries->pdata[i]);
      gboolean first = !previous || !g_str_equal (previous, entry->section);

      previous = entry->section;
      if (child)
        {
          contacts_row_set_entry (CONTACTS_ROW (child), entry, first);
          child = gtk_widget_get_next_sibling (child);
        }
      else
        gtk_box_append (GTK_BOX (self->list), GTK_WIDGET (contacts_row_new (self, entry, first)));
    }
  self->n_rows = i;
  if (i < self->entries->len)
    {
      self->build_id = g_idle_add (build_rows, self);
      return G_SOURCE_REMOVE;
    }
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      gtk_box_remove (GTK_BOX (self->list), child);
      child = next;
    }
  update_anchored (self);
  return G_SOURCE_REMOVE;
}

static void
load (ContactsPage *self)
{
  GPtrArray *all = contacts_get_all (self->contacts);

  g_clear_handle_id (&self->build_id, g_source_remove);
  g_clear_pointer (&self->entries, g_ptr_array_unref);
  /* the rows are rebound (their letters showing again) or go */
  self->hidden[0] = self->hidden[1] = NULL;
  self->entries = g_ptr_array_new_full (all->len, (GDestroyNotify) entry_free);
  for (guint i = 0; i < all->len; i++)
    g_ptr_array_add (self->entries, entry_new (all->pdata[i]));
  self->n_rows = 0;
  /* the empty view only once the address books have been read: until then "no contacts" is not known */
  gtk_stack_set_visible_child (GTK_STACK (self->stack),
                               all->len || !contacts_is_loaded (self->contacts) ? self->scroller : self->empty);
  build_rows (self);
}

static void
on_contacts_changed (ContactsPage *self)
{
  load (self);
}

/* a tap on a contact: its card in the Contacts app (QuickContact), or in a picker the contact picked */
static void
page_open_contact (ContactsPage *self, const Entry *entry)
{
  if (self->picker)
    g_signal_emit (self, signals[PICKED], 0, entry->uid);
  else
    contacts_app_view (GTK_WIDGET (self), entry->uid, self->snackbar);
}

/* "Create new contact": the Contacts app's editor for a new contact */
static void
on_add_clicked (ContactsPage *self)
{
  contacts_app_insert (GTK_WIDGET (self), NULL);
}

static void
on_create (gpointer data)
{
  contacts_app_insert (GTK_WIDGET (data), NULL);
}

/* "Create new contact", on top of the list and under the empty view's line, only where the Contacts app is there to
 * make one: looked at again each time the page shows, as an app can come or go meanwhile */
static void
update_create (ContactsPage *self)
{
  gboolean can_create = !self->picker && contacts_app_installed ();
  GtkWidget *empty;

  gtk_widget_set_visible (self->add, can_create);
  if (self->empty && can_create == self->empty_creates)
    return;
  /* the empty view: the contacts picture, the line, "Create new contact" */
  empty = empty_view_new ("/neo/Dialer/images/empty-contacts.png", _("You don't have any contacts yet"),
                          can_create ? _("Create new contact") : NULL, on_create, self);
  gtk_stack_add_child (GTK_STACK (self->stack), empty);
  if (self->empty)
    {
      if (gtk_stack_get_visible_child (GTK_STACK (self->stack)) == self->empty)
        gtk_stack_set_visible_child (GTK_STACK (self->stack), empty);
      gtk_stack_remove (GTK_STACK (self->stack), self->empty);
    }
  self->empty = empty;
  self->empty_creates = can_create;
  update_anchored (self);
}

/* the "Create new contact" row at the top, its icon lined up with the contacts' photos */
static GtkWidget *
add_contact_row (ContactsPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("dialer-person-add-symbolic");
  GtkWidget *label = gtk_label_new (_("Create new contact"));
  GtkWidget *row;

  gtk_widget_set_size_request (box, -1, ROW_HEIGHT);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_size_request (icon, PHOTO, PHOTO);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (icon, TARGET_START);
  gtk_widget_add_css_class (icon, "dialer-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (label, NAME_START);
  gtk_widget_set_margin_end (label, NAME_END);
  gtk_widget_add_css_class (label, "dialer-primary-text");
  neo_label_set_font_padding (label, 16);
  gtk_box_append (GTK_BOX (box), label);
  row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
  neo_pressable_set_ripple_color (NEO_PRESSABLE (row), &(GdkRGBA) { 1, 1, 1, 0.06 });
  gtk_widget_set_margin_top (row, ADD_TOP);
  gtk_widget_set_margin_start (row, HEADER_WIDTH);
  gtk_widget_set_margin_end (row, TARGET_END);
  g_signal_connect_swapped (row, "clicked", G_CALLBACK (on_add_clicked), self);
  return row;
}

void
contacts_page_reset (ContactsPage *self)
{
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroller), 0);
}

static void
contacts_page_dispose (GObject *object)
{
  ContactsPage *self = CONTACTS_PAGE (object);

  g_clear_handle_id (&self->build_id, g_source_remove);
  if (self->contacts)
    g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_clear_object (&self->contacts);
  g_clear_pointer (&self->entries, g_ptr_array_unref);
  g_clear_object (&self->anchored);
  g_clear_object (&self->bubble);
  g_clear_pointer (&self->anchored_section, g_free);
  g_clear_pointer (&self->stack, gtk_widget_unparent);
  G_OBJECT_CLASS (contacts_page_parent_class)->dispose (object);
}

static void
contacts_page_class_init (ContactsPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = contacts_page_dispose;
  widget_class->snapshot = contacts_page_snapshot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "contacts-page");
  signals[PICKED] = g_signal_new ("picked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
contacts_page_init (ContactsPage *self)
{
  GtkGesture *drag;

  self->stack = gtk_stack_new ();
  gtk_widget_set_parent (self->stack, GTK_WIDGET (self));

  /* the list: no side padding, room at the foot */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_bottom (self->list, LIST_PAD_BOTTOM);
  gtk_widget_set_valign (self->list, GTK_ALIGN_START);
  self->add = add_contact_row (self);
  gtk_box_append (GTK_BOX (self->list), self->add);
  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  gtk_stack_add_child (GTK_STACK (self->stack), self->scroller);
  g_signal_connect_swapped (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller)),
                            "value-changed", G_CALLBACK (on_scrolled), self);

  /* the fast scroller's strip at the end: it takes the touch before the list can */
  drag = gtk_gesture_drag_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

GtkWidget *
contacts_page_new (Contacts *contacts, NeoSnackbar *snackbar)
{
  ContactsPage *self = g_object_new (CONTACTS_TYPE_PAGE, NULL);

  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  update_create (self);
  g_signal_connect (self, "map", G_CALLBACK (update_create), NULL);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  load (self);
  return GTK_WIDGET (self);
}

GtkWidget *
contacts_page_new_picker (Contacts *contacts, NeoSnackbar *snackbar)
{
  ContactsPage *self = g_object_new (CONTACTS_TYPE_PAGE, NULL);

  self->picker = TRUE;
  gtk_widget_set_visible (self->add, FALSE);
  self->contacts = g_object_ref (contacts);
  self->snackbar = snackbar;
  self->empty = empty_view_new ("/neo/Dialer/images/empty-contacts.png", _("You don't have any contacts yet"),
                                NULL, NULL, NULL);
  gtk_stack_add_child (GTK_STACK (self->stack), self->empty);
  g_signal_connect_swapped (contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  load (self);
  return GTK_WIDGET (self);
}
