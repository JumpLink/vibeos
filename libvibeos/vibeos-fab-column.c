/* VibeosFabColumn: see vibeos-fab-column.h.
 *
 * Each smaller button sits in the space between the FAB and the edge on its side, placed in it by a bias: 0 at the
 * edge, 1 against the FAB. The buttons are laid out on their own, not in a box, so that a touch between them still
 * reaches the content under them. */
#include "vibeos-fab-column.h"

#include <math.h>

#define COLUMN_W      180     /* the column at the end, on a wide widget */
#define COLUMN_AFTER  0.65    /* in the column: the button after the FAB, above it */
#define COLUMN_BEFORE 0.65    /* and the one before it, below it (from the bottom edge) */
#define ROW_H         120     /* the row at the bottom, on a tall widget: the FAB with a margin over and under it */
#define ROW_BEFORE    0.6     /* in the row: the button before the FAB, to its left */
#define ROW_AFTER     0.6     /* and the one after it, to its right (from the right edge) */

struct _VibeosFabColumn {
  GtkWidget  parent;
  GtkWidget *content, *fab, *before, *after;
};

G_DEFINE_FINAL_TYPE (VibeosFabColumn, vibeos_fab_column, GTK_TYPE_WIDGET)

static gboolean
wide (int width, int height)
{
  return width > height;
}

gboolean
vibeos_fab_column_is_column (VibeosFabColumn *self)
{
  return wide (gtk_widget_get_width (GTK_WIDGET (self)), gtk_widget_get_height (GTK_WIDGET (self)));
}

static void
natural_size (GtkWidget *widget, int *width, int *height)
{
  int unused;

  gtk_widget_measure (widget, GTK_ORIENTATION_HORIZONTAL, -1, &unused, width, NULL, NULL);
  gtk_widget_measure (widget, GTK_ORIENTATION_VERTICAL, *width, &unused, height, NULL, NULL);
}

static void
put (GtkWidget *widget, double x, double y, int width, int height)
{
  gtk_widget_allocate (widget, width, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (round (x), round (y))));
}

static void
vibeos_fab_column_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                           int *min_baseline, int *nat_baseline)
{
  VibeosFabColumn *self = VIBEOS_FAB_COLUMN (widget);

  *min = *nat = 0;
  if (self->content)
    gtk_widget_measure (self->content, orientation, -1, min, nat, NULL, NULL);
}

static void
vibeos_fab_column_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosFabColumn *self = VIBEOS_FAB_COLUMN (widget);
  int fab_w = 0, fab_h = 0, w, h;
  double fab_x, fab_y;

  if (wide (width, height))
    {
      /* the column: the content stops where it starts */
      double centre_x = width - COLUMN_W / 2.0;

      if (self->content)
        gtk_widget_allocate (self->content, MAX (width - COLUMN_W, 0), height, -1, NULL);
      if (self->fab)
        natural_size (self->fab, &fab_w, &fab_h);
      fab_x = centre_x - fab_w / 2.0;
      fab_y = (height - fab_h) / 2.0;
      if (self->fab)
        put (self->fab, fab_x, fab_y, fab_w, fab_h);
      if (self->after)
        {
          natural_size (self->after, &w, &h);
          put (self->after, centre_x - w / 2.0, COLUMN_AFTER * (fab_y - h), w, h);
        }
      if (self->before)
        {
          natural_size (self->before, &w, &h);
          put (self->before, centre_x - w / 2.0, height - h - COLUMN_BEFORE * (height - fab_y - fab_h - h), w, h);
        }
      return;
    }

  /* the row, over the bottom of the content */
  if (self->content)
    gtk_widget_allocate (self->content, width, height, -1, NULL);
  if (self->fab)
    natural_size (self->fab, &fab_w, &fab_h);
  fab_x = (width - fab_w) / 2.0;
  fab_y = height - ROW_H / 2.0 - fab_h / 2.0;
  if (self->fab)
    put (self->fab, fab_x, fab_y, fab_w, fab_h);
  if (self->before)
    {
      natural_size (self->before, &w, &h);
      put (self->before, ROW_BEFORE * (fab_x - w), height - ROW_H / 2.0 - h / 2.0, w, h);
    }
  if (self->after)
    {
      natural_size (self->after, &w, &h);
      put (self->after, width - w - ROW_AFTER * (width - fab_x - fab_w - w), height - ROW_H / 2.0 - h / 2.0, w, h);
    }
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_fab_column_dispose (GObject *object)
{
  VibeosFabColumn *self = VIBEOS_FAB_COLUMN (object);

  g_clear_pointer (&self->content, gtk_widget_unparent);
  g_clear_pointer (&self->fab, gtk_widget_unparent);
  g_clear_pointer (&self->before, gtk_widget_unparent);
  g_clear_pointer (&self->after, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_fab_column_parent_class)->dispose (object);
}

static void
vibeos_fab_column_class_init (VibeosFabColumnClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_fab_column_dispose;
  widget_class->measure = vibeos_fab_column_measure;
  widget_class->size_allocate = vibeos_fab_column_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "vibeos-fab-column");
}

static void
vibeos_fab_column_init (VibeosFabColumn *self)
{
}

GtkWidget *
vibeos_fab_column_new (void)
{
  return g_object_new (VIBEOS_TYPE_FAB_COLUMN, NULL);
}

/* the content first, so that the buttons are drawn over it and picked before it */
static void
set_child (VibeosFabColumn *self, GtkWidget **slot, GtkWidget *child)
{
  g_clear_pointer (slot, gtk_widget_unparent);
  *slot = child;
  if (!child)
    return;
  if (slot == &self->content)
    gtk_widget_insert_after (child, GTK_WIDGET (self), NULL);
  else
    gtk_widget_set_parent (child, GTK_WIDGET (self));
}

void
vibeos_fab_column_set_content (VibeosFabColumn *self, GtkWidget *content)
{
  set_child (self, &self->content, content);
}

void
vibeos_fab_column_set_fab (VibeosFabColumn *self, GtkWidget *fab)
{
  set_child (self, &self->fab, fab);
}

void
vibeos_fab_column_set_buttons (VibeosFabColumn *self, GtkWidget *before, GtkWidget *after)
{
  set_child (self, &self->before, before);
  set_child (self, &self->after, after);
}
