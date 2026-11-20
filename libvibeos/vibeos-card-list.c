/* VibeosCardList: see vibeos-card-list.h.
 *
 * The children in order, each either a row or a group's header. Rows are placed at exact (not whole px) positions:
 * a VibeosListRow is as tall as its lines of text, which are not whole px, and the gaps between rows stay even all
 * the way down only if nothing is rounded on the way. */
#include "vibeos-card-list.h"
#include "vibeos-list-row.h"
#include "vibeos-text.h"

#include <math.h>

#define SIDE          16      /* the cards' margin from the list's sides */
#define ROW_GAP       2       /* between the rows of a group */
#define GROUP_GAP     16      /* between groups */
#define HEADER_H      52      /* a header's band: the space over its line, the line, the space under it */
#define HEADER_TOP    24
#define HEADER_LINE   20      /* for its text of HEADER_SIZE sp at a font size of 1: it grows with it */
#define HEADER_SIZE   14
#define HEADER_X      24
#define OUTER_RADIUS  20
#define INNER_RADIUS  4

typedef struct {
  GtkWidget *widget;
  gboolean   header;          /* a group's header, not a row */
  gboolean   starts_group;    /* a row that starts a group without a header */
} Entry;

struct _VibeosCardList {
  GtkWidget  parent;
  GArray    *entries;
  gboolean   new_group;       /* the next row starts a group */
  int        top, bottom;
};

G_DEFINE_FINAL_TYPE (VibeosCardList, vibeos_card_list, GTK_TYPE_WIDGET)

static Entry *
entry (VibeosCardList *self, guint i)
{
  return &g_array_index (self->entries, Entry, i);
}

/* Each row's corners: outer where its group starts and ends, small between. Only the rows that show count: a hidden
 * row (a choice that does not apply, or a row a search filters out) leaves the rows around it with the corners they
 * would have without it. A group starts at a header or at a row that starts one, shown or not. */
static void
set_place (GtkWidget *row, gboolean first, gboolean last)
{
  if (VIBEOS_IS_LIST_ROW (row))
    vibeos_list_row_set_corners (VIBEOS_LIST_ROW (row), first ? OUTER_RADIUS : INNER_RADIUS,
                                 last ? OUTER_RADIUS : INNER_RADIUS);
  if (first)
    gtk_widget_add_css_class (row, "vibeos-card-first");
  else
    gtk_widget_remove_css_class (row, "vibeos-card-first");
  if (last)
    gtk_widget_add_css_class (row, "vibeos-card-last");
  else
    gtk_widget_remove_css_class (row, "vibeos-card-last");
}

static void
update_corners (VibeosCardList *self)
{
  GtkWidget *before = NULL;       /* the last row shown in the group so far */
  gboolean before_first = FALSE;

  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = entry (self, i);

      if (e->header || e->starts_group)
        {
          if (before)
            set_place (before, before_first, TRUE);
          before = NULL;
        }
      if (e->header || !gtk_widget_get_visible (e->widget))
        continue;
      if (before)
        set_place (before, before_first, FALSE);
      before_first = before == NULL;
      before = e->widget;
    }
  if (before)
    set_place (before, before_first, TRUE);
}

static void
on_child_visible (GtkWidget *child, GParamSpec *pspec, VibeosCardList *self)
{
  update_corners (self);
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

static double
row_height (GtkWidget *row, int width)
{
  int min, nat;

  if (VIBEOS_IS_LIST_ROW (row))
    return vibeos_list_row_get_exact_height (VIBEOS_LIST_ROW (row), width);
  gtk_widget_measure (row, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
  return nat;
}

/* lays every child out at `width` (allocating them when `allocate`), and returns the height it all takes */
static double
layout (VibeosCardList *self, int width, gboolean allocate)
{
  double y = self->top;
  int card_w = MAX (width - 2 * SIDE, 0);
  enum { NOTHING, ROW, HEADER } before = NOTHING;
  gboolean new_group = FALSE;       /* a hidden header or group start since the last row shown */

  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = entry (self, i);

      if (!gtk_widget_get_visible (e->widget))
        {
          new_group = new_group || e->header || e->starts_group;
          continue;
        }
      if (e->header)
        {
          int label_w = MAX (width - 2 * HEADER_X, 0), min, nat;
          double line = HEADER_LINE * (vibeos_sp (HEADER_SIZE) / HEADER_SIZE);

          y += before == ROW ? ROW_GAP : 0;
          gtk_widget_measure (e->widget, GTK_ORIENTATION_VERTICAL, label_w, &min, &nat, NULL, NULL);
          if (allocate)
            gtk_widget_allocate (e->widget, label_w, MAX (nat, (int) ceil (line)), -1,
                                 gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (HEADER_X, y + HEADER_TOP)));
          y += HEADER_H - HEADER_LINE + MAX (nat, line);
          before = HEADER;
          new_group = FALSE;
          continue;
        }
      if (before == ROW)
        y += e->starts_group || new_group ? GROUP_GAP : ROW_GAP;
      else if (before == HEADER)
        y += ROW_GAP;
      new_group = FALSE;
      {
        double h = row_height (e->widget, card_w);

        if (allocate)
          gtk_widget_allocate (e->widget, card_w, (int) ceil (h - 0.001), -1,
                               gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (SIDE, y)));
        y += h;
      }
      before = ROW;
    }
  return y + self->bottom;
}

/* the height depends on the width (its rows wrap): without this GTK measures it for no width, at its natural one */
static GtkSizeRequestMode
vibeos_card_list_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
vibeos_card_list_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                          int *min_baseline, int *nat_baseline)
{
  VibeosCardList *self = VIBEOS_CARD_LIST (widget);

  if (orientation == GTK_ORIENTATION_VERTICAL)
    {
      int width = for_size;

      if (width < 0)
        vibeos_card_list_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, min, &width, NULL, NULL);
      *min = *nat = (int) ceil (layout (self, width, FALSE) - 0.001);
      return;
    }
  *min = *nat = 0;
  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = entry (self, i);
      int child_min, child_nat, side = e->header ? HEADER_X : SIDE;

      if (!gtk_widget_get_visible (e->widget))
        continue;
      gtk_widget_measure (e->widget, GTK_ORIENTATION_HORIZONTAL, -1, &child_min, &child_nat, NULL, NULL);
      *min = MAX (*min, child_min + 2 * side);
      *nat = MAX (*nat, child_nat + 2 * side);
    }
}

static void
vibeos_card_list_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  layout (VIBEOS_CARD_LIST (widget), width, TRUE);
}

/* ---- the children ---------------------------------------------------------------------------------------------- */

void
vibeos_card_list_append (VibeosCardList *self, GtkWidget *row)
{
  Entry e = { row, FALSE, self->new_group && self->entries->len > 0 };

  g_return_if_fail (VIBEOS_IS_CARD_LIST (self));
  g_return_if_fail (GTK_IS_WIDGET (row));

  self->new_group = FALSE;
  g_array_append_val (self->entries, e);
  gtk_widget_set_parent (row, GTK_WIDGET (self));
  g_signal_connect (row, "notify::visible", G_CALLBACK (on_child_visible), self);
  update_corners (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

GtkWidget *
vibeos_card_list_add_group (VibeosCardList *self, const char *header)
{
  Entry e = { NULL, TRUE, FALSE };

  g_return_val_if_fail (VIBEOS_IS_CARD_LIST (self), NULL);

  if (!header)
    {
      self->new_group = TRUE;
      return NULL;
    }
  self->new_group = FALSE;
  e.widget = g_object_new (GTK_TYPE_LABEL, "label", header, "xalign", 0.f, "wrap", TRUE,
                           "accessible-role", GTK_ACCESSIBLE_ROLE_HEADING, NULL);
  gtk_widget_add_css_class (e.widget, "vibeos-card-list-header");
  g_array_append_val (self->entries, e);
  gtk_widget_set_parent (e.widget, GTK_WIDGET (self));
  update_corners (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  return e.widget;
}

void
vibeos_card_list_remove (VibeosCardList *self, GtkWidget *row)
{
  for (guint i = 0; i < self->entries->len; i++)
    if (entry (self, i)->widget == row)
      {
        /* the group it started goes on from the row after it */
        if (entry (self, i)->starts_group && i + 1 < self->entries->len && !entry (self, i + 1)->header)
          entry (self, i + 1)->starts_group = TRUE;
        g_array_remove_index (self->entries, i);
        g_signal_handlers_disconnect_by_func (row, on_child_visible, self);
        gtk_widget_unparent (row);
        update_corners (self);
        gtk_widget_queue_resize (GTK_WIDGET (self));
        return;
      }
}

void
vibeos_card_list_remove_all (VibeosCardList *self)
{
  for (guint i = 0; i < self->entries->len; i++)
    {
      g_signal_handlers_disconnect_by_func (entry (self, i)->widget, on_child_visible, self);
      gtk_widget_unparent (entry (self, i)->widget);
    }
  g_array_set_size (self->entries, 0);
  self->new_group = FALSE;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
vibeos_card_list_set_margins (VibeosCardList *self, int top, int bottom)
{
  self->top = top;
  self->bottom = bottom;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_card_list_dispose (GObject *object)
{
  VibeosCardList *self = VIBEOS_CARD_LIST (object);

  if (self->entries)
    {
      vibeos_card_list_remove_all (self);
      g_clear_pointer (&self->entries, g_array_unref);
    }
  G_OBJECT_CLASS (vibeos_card_list_parent_class)->dispose (object);
}

static void
vibeos_card_list_class_init (VibeosCardListClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_card_list_dispose;
  widget_class->measure = vibeos_card_list_measure;
  widget_class->get_request_mode = vibeos_card_list_get_request_mode;
  widget_class->size_allocate = vibeos_card_list_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "vibeos-card-list");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_LIST);
}

static void
vibeos_card_list_init (VibeosCardList *self)
{
  self->entries = g_array_new (FALSE, FALSE, sizeof (Entry));
}

GtkWidget *
vibeos_card_list_new (void)
{
  return g_object_new (VIBEOS_TYPE_CARD_LIST, NULL);
}
