/* VibeosFitLabel: see vibeos-fit-label.h. */
#include "vibeos-fit-label.h"

#include <math.h>

/* the scale goes down in steps this big from the one at which every word fits, as autoSize goes down in steps of its
 * granularity */
#define STEP 0.02

struct _VibeosFitLabel {
  GtkWidget  parent;
  GtkWidget *label;
};

G_DEFINE_FINAL_TYPE (VibeosFitLabel, vibeos_fit_label, GTK_TYPE_WIDGET)

/* ---- the scale ------------------------------------------------------------------------------------------------- */

/* the scale, from 1 down to `minimum`, at which `layout`'s text fits `width` px: no word wider than the width, and
 * at most `lines` lines (0 or less: any number); `minimum` when not even that is enough. `layout` is not changed.
 * Apart from any widget, so the tests measure it without a display (test-fit.c) */
double _vibeos_fit_label_scale (PangoLayout *layout, int width, int lines, double minimum);

double
_vibeos_fit_label_scale (PangoLayout *layout, int width, int lines, double minimum)
{
  g_autoptr (PangoLayout) copy = NULL;
  PangoRectangle logical;
  double scale, words;

  if (width <= 0)
    return 1;
  copy = pango_layout_copy (layout);
  pango_layout_set_ellipsize (copy, PANGO_ELLIPSIZE_NONE);
  pango_layout_set_height (copy, -1);
  pango_layout_set_wrap (copy, PANGO_WRAP_WORD);
  /* wrapped at every chance, the widest line is the widest word (GtkLabel's minimum width, the same way) */
  pango_layout_set_width (copy, 0);
  pango_layout_get_pixel_extents (copy, NULL, &logical);
  words = logical.width > width ? (double) width / logical.width : 1;
  if (words < minimum)
    return minimum;
  if (lines <= 0)
    return words;
  /* the largest scale, from there, at which the text takes few enough lines */
  for (scale = words; scale >= minimum; scale -= STEP)
    {
      pango_layout_set_width (copy, (int) floor (width / scale) * PANGO_SCALE);
      if (pango_layout_get_line_count (copy) <= lines)
        return scale;
    }
  return minimum;
}

static double
scale_at (VibeosFitLabel *self, int width)
{
  GtkLabel *label = GTK_LABEL (self->label);

  return _vibeos_fit_label_scale (gtk_label_get_layout (label), width, gtk_label_get_lines (label),
                                  VIBEOS_FIT_LABEL_MIN_SCALE);
}

/* ---- layout ---------------------------------------------------------------------------------------------------- */

static void
vibeos_fit_label_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                          int *minimum_baseline, int *natural_baseline)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);
  double scale = 1;
  int width = -1;

  *minimum = *natural = 0;
  if (!gtk_widget_should_layout (self->label))
    return;
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      /* the minimum is the label's at its smallest, where it breaks words */
      gtk_widget_measure (self->label, orientation, -1, minimum, natural, NULL, NULL);
      *minimum = (int) ceil (*minimum * VIBEOS_FIT_LABEL_MIN_SCALE);
      return;
    }
  /* the height, for a width, of the label drawn at the scale it takes there */
  if (for_size >= 0)
    {
      scale = scale_at (self, for_size);
      width = (int) ceil (for_size / scale);
    }
  gtk_widget_measure (self->label, orientation, width, minimum, natural, NULL, NULL);
  *minimum = (int) ceil (*minimum * scale);
  *natural = (int) ceil (*natural * scale);
}

static void
vibeos_fit_label_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);
  double scale;

  if (!gtk_widget_should_layout (self->label))
    return;
  /* the label laid out in as much more room as it is drawn smaller */
  scale = scale_at (self, width);
  gtk_widget_allocate (self->label, (int) ceil (width / scale), (int) ceil (height / scale), -1,
                       scale < 1 ? gsk_transform_scale (NULL, scale, scale) : NULL);
}

static GtkSizeRequestMode
vibeos_fit_label_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
vibeos_fit_label_compute_expand (GtkWidget *widget, gboolean *hexpand, gboolean *vexpand)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (widget);

  *hexpand = gtk_widget_compute_expand (self->label, GTK_ORIENTATION_HORIZONTAL);
  *vexpand = gtk_widget_compute_expand (self->label, GTK_ORIENTATION_VERTICAL);
}

/* ---- GObject --------------------------------------------------------------------------------------------------- */

static void
vibeos_fit_label_dispose (GObject *object)
{
  VibeosFitLabel *self = VIBEOS_FIT_LABEL (object);

  g_clear_pointer (&self->label, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_fit_label_parent_class)->dispose (object);
}

static void
vibeos_fit_label_class_init (VibeosFitLabelClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_fit_label_dispose;
  widget_class->measure = vibeos_fit_label_measure;
  widget_class->size_allocate = vibeos_fit_label_size_allocate;
  widget_class->get_request_mode = vibeos_fit_label_get_request_mode;
  widget_class->compute_expand = vibeos_fit_label_compute_expand;
  gtk_widget_class_set_css_name (widget_class, "vibeos-fit-label");
}

static void
vibeos_fit_label_init (VibeosFitLabel *self)
{
}

GtkWidget *
vibeos_fit_label_new (GtkWidget *label)
{
  VibeosFitLabel *self;

  g_return_val_if_fail (GTK_IS_LABEL (label), NULL);
  self = g_object_new (VIBEOS_TYPE_FIT_LABEL, NULL);
  self->label = label;
  /* at words; a word breaks only where the scale is at its smallest, the only place one is still too wide */
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_parent (label, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

GtkWidget *
vibeos_fit_label_get_label (VibeosFitLabel *self)
{
  g_return_val_if_fail (VIBEOS_IS_FIT_LABEL (self), NULL);
  return self->label;
}
