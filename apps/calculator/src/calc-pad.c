/* CalcPad: see calc-pad.h */
#include "calc-pad.h"
#include "calc-button.h"

#include <math.h>
#include <neo.h>

#define MAX_GUIDES 8

typedef struct {
  GtkWidget *child;
  int        column, row, margin;
} Cell;

struct _CalcPad {
  GtkWidget  parent;
  double     columns[MAX_GUIDES + 2];   /* 0, the guides, 1 */
  double     rows[MAX_GUIDES + 2];
  int        n_columns, n_rows;
  int        top, bottom, start, end;
  double     exact_width, exact_height;   /* 0: the allocation's */
  GArray    *cells;
};

G_DEFINE_FINAL_TYPE (CalcPad, calc_pad, GTK_TYPE_WIDGET)

static void
calc_pad_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                  int *minimum_baseline, int *natural_baseline)
{
  /* sized by the window's ratios */
  *minimum = *natural = 0;
}

/* a guideline's position in device pixels: its percent of the content box, rounded */
static int
guide_px (double fraction, int start, int length)
{
  return start + (int) floor (fraction * length + 0.5);
}

static void
calc_pad_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalcPad *self = CALC_PAD (widget);
  int scale = neo_device_scale (widget);
  /* the pad's own size in device pixels, which the window knows better than the whole logical pixels around it */
  double exact_w = self->exact_width > 0 ? self->exact_width : width;
  double exact_h = self->exact_height > 0 ? self->exact_height : height;
  int W = (int) round (exact_w * scale), H = (int) round (exact_h * scale);
  int left = self->start * scale, top = self->top * scale;
  int content_w = W - (self->start + self->end) * scale, content_h = H - (self->top + self->bottom) * scale;

  /* always left to right, whatever the language */
  for (guint i = 0; i < self->cells->len; i++) {
    Cell *cell = &g_array_index (self->cells, Cell, i);
    int x0 = guide_px (self->columns[cell->column], left, content_w);
    int x1 = guide_px (self->columns[cell->column + 1], left, content_w);
    int y0 = guide_px (self->rows[cell->row], top, content_h);
    int y1 = guide_px (self->rows[cell->row + 1], top, content_h);
    double x = (double) (x0 + cell->margin * scale) / scale;
    double w = (double) (x1 - x0 - 2 * cell->margin * scale) / scale;
    double y = (double) y0 / scale;
    double h = (double) (y1 - y0) / scale;

    if (!gtk_widget_get_visible (cell->child))
      continue;
    if (CALC_IS_BUTTON (cell->child))
      calc_button_set_exact_size (CALC_BUTTON (cell->child), w, h);
    gtk_widget_allocate (cell->child, (int) ceil (w - 0.001), (int) ceil (h - 0.001), -1,
                         gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)));
  }
}

static void
calc_pad_dispose (GObject *object)
{
  CalcPad *self = CALC_PAD (object);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);
  g_clear_pointer (&self->cells, g_array_unref);
  G_OBJECT_CLASS (calc_pad_parent_class)->dispose (object);
}

static void
calc_pad_class_init (CalcPadClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = calc_pad_dispose;
  widget_class->measure = calc_pad_measure;
  widget_class->size_allocate = calc_pad_size_allocate;
  gtk_widget_class_set_css_name (widget_class, "calc-pad");
}

static void
calc_pad_init (CalcPad *self)
{
  self->cells = g_array_new (FALSE, FALSE, sizeof (Cell));
}

GtkWidget *
calc_pad_new (const double *column_guides, int n_columns, const double *row_guides, int n_rows)
{
  CalcPad *self = g_object_new (CALC_TYPE_PAD, NULL);

  g_return_val_if_fail (n_columns <= MAX_GUIDES + 1 && n_rows <= MAX_GUIDES + 1, GTK_WIDGET (self));
  self->n_columns = n_columns;
  self->n_rows = n_rows;
  self->columns[0] = self->rows[0] = 0;
  for (int i = 0; i < n_columns - 1; i++)
    self->columns[i + 1] = column_guides[i];
  self->columns[n_columns] = 1;
  for (int i = 0; i < n_rows - 1; i++)
    self->rows[i + 1] = row_guides[i];
  self->rows[n_rows] = 1;
  return GTK_WIDGET (self);
}

void
calc_pad_set_padding (CalcPad *self, int top, int bottom, int start, int end)
{
  self->top = top;
  self->bottom = bottom;
  self->start = start;
  self->end = end;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_pad_set_exact_size (CalcPad *self, double width, double height)
{
  if (self->exact_width == width && self->exact_height == height)
    return;
  self->exact_width = width;
  self->exact_height = height;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
calc_pad_attach (CalcPad *self, GtkWidget *child, int column, int row, int margin)
{
  Cell cell = { child, column, row, margin };

  g_return_if_fail (column < self->n_columns && row < self->n_rows);
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  g_array_append_val (self->cells, cell);
}
