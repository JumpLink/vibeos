/* The contact list (ct-list.h).
 *
 * Three levels: CtList holds the scroll view (libspongy: the drag, the fling, the stretch); inside it CtListContent,
 * a GtkScrollable as tall as the view, places the head, the rows and headers on screen and the pinned header, all
 * at their scrolled positions. The rows and headers are laid out once per change of the contacts (Item, below) and
 * get widgets only while they show. */
#include "ct-list.h"
#include "ct-avatar.h"
#include "ct-ui.h"

#include <glib/gi18n.h>
#include <math.h>
#include <spongy-gtk.h>
#include <vibeos.h>

#define SIDE            CT_LIST_SIDE_DP
#define ROW             CT_ROW_H_DP
#define SNIPPET_ROW     CT_ROW_SNIPPET_H_DP
#define GAP             CT_ROW_GAP_DP
#define HEADER          CT_SECTION_H_DP
#define CHECKBOX        32.0
#define LOAD_STAGGER    (CT_LIST_LOAD_MS * CT_LIST_LOAD_DELAY_FRAC)

typedef enum { ITEM_HEADER, ITEM_ROW } ItemKind;

/* a header or a row, where it is from the top of the first one */
typedef struct {
  ItemKind kind;
  guint index;                  /* the row's index in CtRows.rows, or the header's section */
  double y, h;
  gboolean first, last;         /* a row that starts or ends its card */
} Item;

struct _CtList {
  GtkWidget parent;
  GtkWidget *scroll, *content;
  CtRows *rows;
  GArray *items;                /* Item */
  GArray *section_items;        /* guint: each section's header item, G_MAXUINT for none */
  double items_h;
  guint generation;             /* a new set of rows: every widget is bound again */
  gboolean search;
  double bottom;
  GHashTable *selected;         /* uid set, or NULL outside selection mode */
  char *current;
  GtkWidget *head;
  GPtrArray *row_widgets, *header_widgets;
  GtkWidget *sticky;

  /* the rows' first appearance */
  VibeosAnim *load_anim;
  double load_ms;               /* ms into it; < 0 when done */
};

G_DEFINE_FINAL_TYPE (CtList, ct_list, GTK_TYPE_WIDGET)

enum { ROW_ACTIVATED, ROW_LONG_PRESSED, N_SIGNALS };
static guint signals[N_SIGNALS];

static Item *
item_at (CtList *self, guint i)
{
  return &g_array_index (self->items, Item, i);
}

/* ===== a row =================================================================================================== */

#define CT_TYPE_LIST_ROW (ct_list_row_get_type ())
G_DECLARE_FINAL_TYPE (CtListRow, ct_list_row, CT, LIST_ROW, GtkWidget)

struct _CtListRow {
  GtkWidget parent;
  CtList *list;
  GtkWidget *target, *avatar, *name, *snippet, *check;
  int item;                     /* the item it shows, −1: free */
  guint row;                    /* its row in CtRows */
  guint generation;
  double top_radius, bottom_radius;
  gboolean current;
};

G_DEFINE_FINAL_TYPE (CtListRow, ct_list_row, GTK_TYPE_WIDGET)

static GskRoundedRect
card_shape (double w, double h, double top, double bottom)
{
  GskRoundedRect shape;
  graphene_size_t t = GRAPHENE_SIZE_INIT ((float) top, (float) top);
  graphene_size_t b = GRAPHENE_SIZE_INIT ((float) bottom, (float) bottom);

  gsk_rounded_rect_init (&shape, &GRAPHENE_RECT_INIT (0, 0, (float) w, (float) h), &t, &t, &b, &b);
  return shape;
}

static void
put (GtkWidget *child, double x, double y, double w, double h)
{
  int min, nat;

  /* measured first, as GTK asks; then placed in the box the layout wants, which may be smaller than the text */
  gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, (int) MAX (w, 0), &min, &nat, NULL, NULL);
  gtk_widget_allocate (child, (int) MAX (w, 0), (int) h, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) x, (float) y)));
}

static void
ct_list_row_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtListRow *self = CT_LIST_ROW (widget);
  gboolean check = gtk_widget_get_visible (self->check);
  gboolean snippet = gtk_widget_get_visible (self->snippet);
  double text_end = width - CT_ROW_END_DP - (check ? CHECKBOX + CT_ROW_CHECK_GAP_DP : 0);
  double text_w = text_end - CT_ROW_NAME_X_DP;

  put (self->target, 0, 0, width, height);
  put (self->avatar, CT_ROW_PHOTO_X_DP, (height - CT_ROW_PHOTO_DP) / 2, CT_ROW_PHOTO_DP, CT_ROW_PHOTO_DP);
  if (snippet)
    {
      put (self->name, CT_ROW_NAME_X_DP, height / 2.0 - CT_ROW_NAME_BOX_DP + 2, text_w, CT_ROW_NAME_BOX_DP);
      put (self->snippet, CT_ROW_NAME_X_DP, height / 2.0 + 2, text_w, CT_ROW_SNIPPET_BOX_DP);
    }
  else
    put (self->name, CT_ROW_NAME_X_DP, (height - CT_ROW_NAME_BOX_DP) / 2, text_w, CT_ROW_NAME_BOX_DP);
  if (check)
    put (self->check, width - CT_ROW_END_DP - CHECKBOX + 4, (height - CHECKBOX) / 2, CHECKBOX, CHECKBOX);
}

static void
ct_list_row_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtListRow *self = CT_LIST_ROW (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  GskRoundedRect shape = card_shape (w, h, self->top_radius, self->bottom_radius);
  /* the contact open beside the list shows in the selected colour */
  GdkRGBA fill = vibeos_color (self->current ? VIBEOS_SECONDARY_CONTAINER : CT_CARD);

  gtk_snapshot_push_rounded_clip (snapshot, &shape);
  gtk_snapshot_append_color (snapshot, &fill, &shape.bounds);
  gtk_widget_snapshot_child (widget, self->target, snapshot);
  gtk_snapshot_pop (snapshot);
  for (GtkWidget *child = gtk_widget_get_next_sibling (self->target); child; child = gtk_widget_get_next_sibling (child))
    gtk_widget_snapshot_child (widget, child, snapshot);
}

static void
ct_list_row_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (ct_list_row_parent_class)->dispose (object);
}

static void
ct_list_row_class_init (CtListRowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ct_list_row_dispose;
  GTK_WIDGET_CLASS (klass)->size_allocate = ct_list_row_size_allocate;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_list_row_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-list-row");
}

static void
ct_list_row_init (CtListRow *self)
{
  self->item = -1;
}

static void
on_row_clicked (VibeosPressable *target, CtListRow *row)
{
  if (row->item >= 0)
    g_signal_emit (row->list, signals[ROW_ACTIVATED], 0, row->row);
}

static void
on_row_long_pressed (VibeosPressable *target, CtListRow *row)
{
  if (row->item >= 0)
    g_signal_emit (row->list, signals[ROW_LONG_PRESSED], 0, row->row);
}

static GtkWidget *
row_label (const char *css_class)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_add_css_class (label, css_class);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_set_can_target (label, FALSE);
  return label;
}

static CtListRow *
list_row_new (CtList *list)
{
  CtListRow *self = g_object_new (CT_TYPE_LIST_ROW, NULL);

  self->list = list;
  self->target = vibeos_pressable_new (NULL);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->target), VIBEOS_RIPPLE_BOUNDED, 0);
  g_signal_connect (self->target, "clicked", G_CALLBACK (on_row_clicked), self);
  g_signal_connect (self->target, "long-pressed", G_CALLBACK (on_row_long_pressed), self);
  gtk_widget_set_parent (self->target, GTK_WIDGET (self));
  self->avatar = ct_avatar_view_new (CT_ROW_PHOTO_DP);
  gtk_widget_set_parent (self->avatar, GTK_WIDGET (self));
  self->name = row_label ("ct-row-name");
  gtk_widget_set_parent (self->name, GTK_WIDGET (self));
  self->snippet = row_label ("ct-row-snippet");
  gtk_widget_set_parent (self->snippet, GTK_WIDGET (self));
  self->check = ct_check_new ();
  gtk_widget_set_parent (self->check, GTK_WIDGET (self));
  return self;
}

/* ===== a section's header ====================================================================================== */

#define CT_TYPE_LIST_HEADER (ct_list_header_get_type ())
G_DECLARE_FINAL_TYPE (CtListHeader, ct_list_header, CT, LIST_HEADER, GtkWidget)

struct _CtListHeader {
  GtkWidget parent;
  GtkWidget *star, *label;
  int item;                     /* −1: free */
  guint generation;
  gboolean sticky;              /* the pinned one: it covers the rows passing under it */
};

G_DEFINE_FINAL_TYPE (CtListHeader, ct_list_header, GTK_TYPE_WIDGET)

static void
ct_list_header_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtListHeader *self = CT_LIST_HEADER (widget);
  gboolean star = gtk_widget_get_visible (self->star);
  double x = star ? CT_SECTION_STAR_TEXT_X_DP : CT_SECTION_TEXT_X_DP;
  int min, nat;

  gtk_widget_measure (self->label, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
  if (star)
    put (self->star, CT_SECTION_TEXT_X_DP, (height - CT_SECTION_STAR_DP) / 2, CT_SECTION_STAR_DP, CT_SECTION_STAR_DP);
  put (self->label, x, (height - CT_SECTION_TEXT_BOX_DP) / 2, MIN (nat, width - x - CT_SECTION_TEXT_X_DP),
       CT_SECTION_TEXT_BOX_DP);
}

static void
ct_list_header_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtListHeader *self = CT_LIST_HEADER (widget);

  if (self->sticky)
    {
      /* the band the width of the whole list, in the background's colour */
      GtkWidget *content = gtk_widget_get_parent (widget);
      GdkRGBA background = vibeos_color (VIBEOS_SURFACE_CONTAINER);
      double w = content ? gtk_widget_get_width (content) : gtk_widget_get_width (widget);

      gtk_snapshot_append_color (snapshot, &background,
                                 &GRAPHENE_RECT_INIT ((float) -SIDE, 0, (float) w,
                                                      (float) gtk_widget_get_height (widget)));
    }
  gtk_widget_snapshot_child (widget, self->star, snapshot);
  gtk_widget_snapshot_child (widget, self->label, snapshot);
}

static void
ct_list_header_dispose (GObject *object)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (ct_list_header_parent_class)->dispose (object);
}

static void
ct_list_header_class_init (CtListHeaderClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = ct_list_header_dispose;
  GTK_WIDGET_CLASS (klass)->size_allocate = ct_list_header_size_allocate;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_list_header_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-list-header");
}

static void
ct_list_header_init (CtListHeader *self)
{
  self->item = -1;
  self->star = gtk_image_new_from_icon_name ("ct-star-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->star), (int) CT_SECTION_STAR_DP);
  gtk_widget_add_css_class (self->star, "ct-list-header-star");
  gtk_widget_set_parent (self->star, GTK_WIDGET (self));
  self->label = row_label ("ct-list-header-text");
  gtk_widget_set_parent (self->label, GTK_WIDGET (self));
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

/* the header of section @index */
static void
header_show (CtList *list, CtListHeader *header, guint section_index)
{
  const CtSection *section = &g_array_index (list->rows->sections, CtSection, section_index);
  gboolean favourites = section->rank == CT_BUCKET_FAVOURITES;

  gtk_widget_set_visible (header->star, favourites);
  gtk_label_set_label (GTK_LABEL (header->label), favourites ? _("Favorites") : section->label);
}

/* ===== the layout ============================================================================================== */

/* the rows and headers, top to bottom: a section's header, then its rows on one card */
static void
build_items (CtList *self)
{
  double y = 0;

  g_array_set_size (self->items, 0);
  g_array_set_size (self->section_items, 0);
  self->generation++;
  if (!self->rows)
    {
      self->items_h = 0;
      return;
    }
  for (guint s = 0; !self->search && s < self->rows->sections->len; s++)
    {
      guint none = G_MAXUINT;

      g_array_append_val (self->section_items, none);
    }
  for (guint i = 0; i < self->rows->rows->len; i++)
    {
      CtRow *row = &g_array_index (self->rows->rows, CtRow, i);
      gboolean first = i == 0 || (!self->search && row->first);
      gboolean last = i + 1 == self->rows->rows->len
                      || (!self->search && g_array_index (self->rows->rows, CtRow, i + 1).first);
      Item item;

      if (first && !self->search && row->section >= 0)
        {
          Item header = { ITEM_HEADER, (guint) row->section, y, HEADER, FALSE, FALSE };

          g_array_index (self->section_items, guint, row->section) = self->items->len;
          g_array_append_val (self->items, header);
          y += HEADER;
        }
      else if (!first)
        y += GAP;
      item = (Item) { ITEM_ROW, i, y, self->search && row->match.snippet ? SNIPPET_ROW : ROW, first, last };
      g_array_append_val (self->items, item);
      y += item.h;
    }
  self->items_h = y;
}

/* the first item whose bottom is below @y */
static guint
first_item_below (CtList *self, double y)
{
  guint lo = 0, hi = self->items->len;

  while (lo < hi)
    {
      guint mid = (lo + hi) / 2;
      Item *item = item_at (self, mid);

      if (item->y + item->h <= y)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo;
}

/* the section the item at the top of the view is in, −1 for none */
static int
section_of_item (CtList *self, guint index)
{
  Item *item;

  if (index >= self->items->len)
    return -1;
  item = item_at (self, index);
  return item->kind == ITEM_HEADER ? (int) item->index
                                   : g_array_index (self->rows->rows, CtRow, item->index).section;
}

/* ===== the content ============================================================================================ */

#define CT_TYPE_LIST_CONTENT (ct_list_content_get_type ())
G_DECLARE_FINAL_TYPE (CtListContent, ct_list_content, CT, LIST_CONTENT, GtkWidget)

struct _CtListContent {
  GtkWidget parent;
  CtList *list;
  GtkAdjustment *vadjustment, *hadjustment;
};

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

G_DEFINE_FINAL_TYPE_WITH_CODE (CtListContent, ct_list_content, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

static double
head_height (CtList *self, int width)
{
  int min, nat;

  if (!self->head || !gtk_widget_get_visible (self->head))
    return CT_LIST_PAD_TOP_DP;
  gtk_widget_measure (self->head, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  return nat;
}

static void
bind_row (CtList *self, CtListRow *widget, guint index)
{
  Item *item = item_at (self, index);
  CtRow *row = &g_array_index (self->rows->rows, CtRow, item->index);
  gboolean current = self->current && g_strcmp0 (self->current, row->entry->uid) == 0;

  if (widget->item != (int) index || widget->generation != self->generation)
    {
      const char *name = row->name && *row->name ? row->name : " ";

      widget->item = (int) index;
      widget->row = item->index;
      widget->generation = self->generation;
      gtk_label_set_label (GTK_LABEL (widget->name), name);
      gtk_label_set_label (GTK_LABEL (widget->snippet), row->match.snippet ? row->match.snippet : "");
      gtk_widget_set_visible (widget->snippet, self->search && row->match.snippet);
      ct_avatar_view_set (widget->avatar, row->entry->uid, row->name, row->entry->photo);
      gtk_accessible_update_property (GTK_ACCESSIBLE (widget->target), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
      widget->top_radius = item->first ? CT_CARD_OUTER_RADIUS_DP : CT_CARD_INNER_RADIUS_DP;
      widget->bottom_radius = item->last ? CT_CARD_OUTER_RADIUS_DP : CT_CARD_INNER_RADIUS_DP;
      gtk_widget_queue_resize (GTK_WIDGET (widget));
    }
  gtk_widget_set_visible (widget->check, self->selected != NULL);
  if (self->selected)
    ct_check_set_active (widget->check, g_hash_table_contains (self->selected, row->entry->uid));
  if (widget->current != current)
    {
      widget->current = current;
      gtk_widget_queue_draw (GTK_WIDGET (widget));
    }
}

/* the widget that shows item @index: the one that showed it last time, else a free one, else a new one */
static GtkWidget *
widget_for (CtList *self, guint index, GPtrArray *taken)
{
  Item *item = item_at (self, index);
  GPtrArray *pool = item->kind == ITEM_ROW ? self->row_widgets : self->header_widgets;
  GtkWidget *found = NULL;

  for (guint i = 0; i < pool->len && !found; i++)
    {
      GtkWidget *widget = pool->pdata[i];
      int bound = item->kind == ITEM_ROW ? CT_LIST_ROW (widget)->item : CT_LIST_HEADER (widget)->item;
      guint generation = item->kind == ITEM_ROW ? CT_LIST_ROW (widget)->generation
                                                : CT_LIST_HEADER (widget)->generation;

      if (bound == (int) index && generation == self->generation && !g_ptr_array_find (taken, widget, NULL))
        found = widget;
    }
  for (guint i = 0; i < pool->len && !found; i++)
    if (!g_ptr_array_find (taken, pool->pdata[i], NULL))
      {
        GtkWidget *widget = pool->pdata[i];
        int bound = item->kind == ITEM_ROW ? CT_LIST_ROW (widget)->item : CT_LIST_HEADER (widget)->item;

        /* one bound to an item still on screen is left for it */
        if (bound < 0 || !g_object_get_data (G_OBJECT (widget), "ct-wanted"))
          found = widget;
      }
  if (!found)
    {
      found = item->kind == ITEM_ROW ? GTK_WIDGET (list_row_new (self)) : g_object_new (CT_TYPE_LIST_HEADER, NULL);
      gtk_widget_insert_before (found, self->content, self->sticky);
      g_ptr_array_add (pool, found);
    }
  g_ptr_array_add (taken, found);
  if (item->kind == ITEM_ROW)
    bind_row (self, CT_LIST_ROW (found), index);
  else
    {
      CtListHeader *header = CT_LIST_HEADER (found);

      if (header->item != (int) index || header->generation != self->generation)
        {
          header->item = (int) index;
          header->generation = self->generation;
          header_show (self, header, item->index);
        }
    }
  return found;
}

static void
configure (CtListContent *self, double total)
{
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  double upper, value;

  if (!self->vadjustment)
    return;
  upper = MAX (total, height);
  value = CLAMP (gtk_adjustment_get_value (self->vadjustment), 0, upper - height);
  if (gtk_adjustment_get_upper (self->vadjustment) != upper || gtk_adjustment_get_page_size (self->vadjustment) != height)
    gtk_adjustment_configure (self->vadjustment, value, 0, upper, ROW, height * 0.9, height);
}

static double
offset (CtList *self)
{
  CtListContent *content = CT_LIST_CONTENT (self->content);

  return content->vadjustment ? gtk_adjustment_get_value (content->vadjustment) : 0;
}

static void
place (GtkWidget *child, double x, double y, double w, double h)
{
  gtk_widget_allocate (child, (int) w, (int) ceil (h), -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((float) x, (float) y)));
}

static void
ct_list_content_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtList *self = CT_LIST_CONTENT (widget)->list;
  double head = head_height (self, width);
  double scrolled, top, card_w = MAX (width - 2 * SIDE, 0);
  g_autoptr(GPtrArray) taken = g_ptr_array_new ();
  guint first, shown = 0;
  int section = -1;

  configure (CT_LIST_CONTENT (widget), head + self->items_h + self->bottom);
  scrolled = offset (self);
  if (self->head && gtk_widget_get_visible (self->head))
    place (self->head, 0, -scrolled, width, head);

  /* the items on screen: the head's height down from the content's top */
  top = scrolled - head;
  first = first_item_below (self, top);
  /* which widgets keep their items: marked first, so a free widget is not one about to be wanted */
  for (guint pass = 0; pass < 2; pass++)
    {
      GPtrArray *pool = pass == 0 ? self->row_widgets : self->header_widgets;

      for (guint i = 0; i < pool->len; i++)
        {
          GtkWidget *w = pool->pdata[i];
          int bound = pass == 0 ? CT_LIST_ROW (w)->item : CT_LIST_HEADER (w)->item;
          guint generation = pass == 0 ? CT_LIST_ROW (w)->generation : CT_LIST_HEADER (w)->generation;
          gboolean wanted = bound >= (int) first && generation == self->generation && bound < (int) self->items->len
                            && item_at (self, (guint) bound)->y < top + height;

          g_object_set_data (G_OBJECT (w), "ct-wanted", GINT_TO_POINTER (wanted));
        }
    }
  for (guint i = first; i < self->items->len && item_at (self, i)->y < top + height; i++)
    {
      Item *item = item_at (self, i);
      GtkWidget *child = widget_for (self, i, taken);
      double y = head + item->y - scrolled;

      if (item->kind == ITEM_ROW && self->load_ms >= 0)
        {
          /* row k on screen starts a stagger after the one above it: from transparent, a little low */
          double t = CLAMP ((self->load_ms - shown * LOAD_STAGGER) / CT_LIST_LOAD_MS, 0, 1);
          double eased = spongy_curve (SPONGY_LINEAR_OUT_SLOW_IN, (float) t);

          gtk_widget_set_opacity (child, eased);
          y += (1 - eased) * ROW * CT_LIST_LOAD_TRANSLATE_FRAC;
        }
      else
        gtk_widget_set_opacity (child, 1);
      if (item->kind == ITEM_ROW)
        shown++;
      gtk_widget_set_child_visible (child, TRUE);
      place (child, SIDE, y, card_w, item->h);
    }
  /* the rest go off screen, free */
  for (guint pass = 0; pass < 2; pass++)
    {
      GPtrArray *pool = pass == 0 ? self->row_widgets : self->header_widgets;

      for (guint i = 0; i < pool->len; i++)
        if (!g_ptr_array_find (taken, pool->pdata[i], NULL))
          {
            GtkWidget *w = pool->pdata[i];

            if (pass == 0)
              CT_LIST_ROW (w)->item = -1;
            else
              CT_LIST_HEADER (w)->item = -1;
            gtk_widget_set_child_visible (w, FALSE);
          }
    }

  /* the pinned header: the section at the top, once its own header has gone up past the top, pushed up by the
   * next section's header as it comes */
  if (!self->search && top > 0)
    section = section_of_item (self, first);
  if (section >= 0 && (guint) section < self->section_items->len
      && g_array_index (self->section_items, guint, section) != G_MAXUINT
      && item_at (self, g_array_index (self->section_items, guint, section))->y < top)
    {
      double y = 0;
      guint next;

      for (next = (guint) section + 1; next < self->section_items->len; next++)
        if (g_array_index (self->section_items, guint, next) != G_MAXUINT)
          break;
      if (next < self->section_items->len)
        y = MIN (0, item_at (self, g_array_index (self->section_items, guint, next))->y - top - HEADER);
      header_show (self, CT_LIST_HEADER (self->sticky), (guint) section);
      gtk_widget_set_child_visible (self->sticky, TRUE);
      place (self->sticky, SIDE, y, card_w, HEADER);
    }
  else
    {
      gtk_widget_set_child_visible (self->sticky, FALSE);
      place (self->sticky, SIDE, -HEADER, card_w, HEADER);
    }
}

static void
ct_list_content_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                         int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = 0;
}

static void on_value_changed (GtkAdjustment *adjustment, CtListContent *self);

static void
set_adjustment (CtListContent *self, GtkAdjustment **slot, GtkAdjustment *adjustment)
{
  if (*slot == self->vadjustment && self->vadjustment)
    g_signal_handlers_disconnect_by_func (self->vadjustment, on_value_changed, self);
  g_clear_object (slot);
  if (adjustment)
    *slot = g_object_ref_sink (adjustment);
  if (slot == &self->vadjustment && self->vadjustment)
    {
      g_signal_connect (self->vadjustment, "value-changed", G_CALLBACK (on_value_changed), self);
      gtk_widget_queue_allocate (GTK_WIDGET (self));
    }
}

static void
ct_list_content_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  CtListContent *self = CT_LIST_CONTENT (object);

  switch (id)
    {
    case PROP_VADJUSTMENT:
      set_adjustment (self, &self->vadjustment, g_value_get_object (value));
      break;
    case PROP_HADJUSTMENT:
      set_adjustment (self, &self->hadjustment, g_value_get_object (value));
      break;
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
ct_list_content_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  CtListContent *self = CT_LIST_CONTENT (object);

  switch (id)
    {
    case PROP_VADJUSTMENT:
      g_value_set_object (value, self->vadjustment);
      break;
    case PROP_HADJUSTMENT:
      g_value_set_object (value, self->hadjustment);
      break;
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
      g_value_set_enum (value, GTK_SCROLL_MINIMUM);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
ct_list_content_dispose (GObject *object)
{
  CtListContent *self = CT_LIST_CONTENT (object);
  GtkWidget *child;

  set_adjustment (self, &self->vadjustment, NULL);
  set_adjustment (self, &self->hadjustment, NULL);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))))
    gtk_widget_unparent (child);
  G_OBJECT_CLASS (ct_list_content_parent_class)->dispose (object);
}

static void
ct_list_content_class_init (CtListContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_list_content_dispose;
  object_class->set_property = ct_list_content_set_property;
  object_class->get_property = ct_list_content_get_property;
  widget_class->measure = ct_list_content_measure;
  widget_class->size_allocate = ct_list_content_size_allocate;
  g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
  gtk_widget_class_set_css_name (widget_class, "ct-list-content");
}

static void
ct_list_content_init (CtListContent *self)
{
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

static void
on_value_changed (GtkAdjustment *adjustment, CtListContent *content)
{
  gtk_widget_queue_allocate (GTK_WIDGET (content));
}

/* ===== CtList ================================================================================================= */

static void
ct_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                 int *minimum_baseline, int *natural_baseline)
{
  gtk_widget_measure (CT_LIST (widget)->scroll, orientation, for_size, minimum, natural, NULL, NULL);
}

static void
ct_list_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  gtk_widget_allocate (CT_LIST (widget)->scroll, width, height, -1, NULL);
}

static void
on_load_anim (VibeosAnim *anim, double value, gpointer data)
{
  CtList *self = data;

  self->load_ms = value >= 1 ? -1 : value * (CT_LIST_LOAD_MS + 14 * LOAD_STAGGER);
  gtk_widget_queue_allocate (self->content);
}

static void
ct_list_dispose (GObject *object)
{
  CtList *self = CT_LIST (object);

  g_clear_pointer (&self->load_anim, vibeos_anim_free);
  g_clear_pointer (&self->scroll, gtk_widget_unparent);
  g_clear_pointer (&self->rows, ct_rows_free);
  g_clear_pointer (&self->items, g_array_unref);
  g_clear_pointer (&self->section_items, g_array_unref);
  g_clear_pointer (&self->row_widgets, g_ptr_array_unref);
  g_clear_pointer (&self->header_widgets, g_ptr_array_unref);
  g_clear_pointer (&self->current, g_free);
  G_OBJECT_CLASS (ct_list_parent_class)->dispose (object);
}

static void
ct_list_class_init (CtListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ct_list_dispose;
  widget_class->measure = ct_list_measure;
  widget_class->size_allocate = ct_list_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "ct-list");
  signals[ROW_ACTIVATED] = g_signal_new ("row-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                         NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[ROW_LONG_PRESSED] = g_signal_new ("row-long-pressed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
ct_list_init (CtList *self)
{
  self->load_ms = -1;
  self->bottom = CT_LIST_PAD_BOTTOM_DP;
  self->items = g_array_new (FALSE, FALSE, sizeof (Item));
  self->section_items = g_array_new (FALSE, FALSE, sizeof (guint));
  /* the pools hold the widgets the content has as children: the content unparents them */
  self->row_widgets = g_ptr_array_new ();
  self->header_widgets = g_ptr_array_new ();
  self->content = g_object_new (CT_TYPE_LIST_CONTENT, NULL);
  CT_LIST_CONTENT (self->content)->list = self;
  self->sticky = g_object_new (CT_TYPE_LIST_HEADER, NULL);
  CT_LIST_HEADER (self->sticky)->sticky = TRUE;
  gtk_widget_add_css_class (self->sticky, "sticky");
  gtk_widget_set_child_visible (self->sticky, FALSE);
  gtk_widget_set_parent (self->sticky, self->content);
  self->load_anim = vibeos_anim_new (GTK_WIDGET (self), on_load_anim, self);
  self->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroll), self->content);
  gtk_widget_set_parent (self->scroll, GTK_WIDGET (self));
}

GtkWidget *
ct_list_new (void)
{
  return g_object_new (CT_TYPE_LIST, NULL);
}

void
ct_list_set_rows (CtList *self, CtRows *rows, gboolean to_top)
{
  g_clear_pointer (&self->rows, ct_rows_free);
  self->rows = rows;
  build_items (self);
  if (to_top)
    spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), 0);
  gtk_widget_queue_allocate (self->content);
}

CtRows *
ct_list_get_rows (CtList *self)
{
  return self->rows;
}

CtEntry *
ct_list_get_entry (CtList *self, guint index)
{
  return self->rows && index < self->rows->rows->len ? g_array_index (self->rows->rows, CtRow, index).entry : NULL;
}

void
ct_list_set_search (CtList *self, gboolean search)
{
  if (self->search == search)
    return;
  self->search = search;
  build_items (self);
  gtk_widget_queue_allocate (self->content);
}

void
ct_list_set_head (CtList *self, GtkWidget *head)
{
  if (self->head == head)
    return;
  if (self->head)
    gtk_widget_unparent (self->head);
  self->head = head;
  /* under everything else: the rows never pass under it, and the pinned header goes over it */
  if (head)
    gtk_widget_insert_after (head, self->content, NULL);
  gtk_widget_queue_allocate (self->content);
}

void
ct_list_set_bottom_padding (CtList *self, double padding)
{
  self->bottom = padding;
  gtk_widget_queue_allocate (self->content);
}

void
ct_list_set_selection (CtList *self, GHashTable *selected)
{
  self->selected = selected;
  gtk_widget_queue_allocate (self->content);
}

void
ct_list_update_selection (CtList *self)
{
  for (guint i = 0; i < self->row_widgets->len; i++)
    {
      CtListRow *row = self->row_widgets->pdata[i];

      if (row->item >= 0)
        bind_row (self, row, (guint) row->item);
    }
}

void
ct_list_set_current (CtList *self, const char *uid)
{
  if (g_strcmp0 (self->current, uid) == 0)
    return;
  g_free (self->current);
  self->current = g_strdup (uid);
  ct_list_update_selection (self);
}

void
ct_list_animate_load (CtList *self)
{
  self->load_ms = 0;
  vibeos_anim_start (self->load_anim, 0, 1, (guint) (CT_LIST_LOAD_MS + 14 * LOAD_STAGGER), 0, SPONGY_LINEAR);
}

void
ct_list_scroll_to_top (CtList *self)
{
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (self->scroll), 0);
}
