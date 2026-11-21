/* VibeosClamp: see vibeos-clamp.h. */
#include "vibeos-clamp.h"

struct _VibeosClamp {
  GtkWidget  parent;
  GtkWidget *child;
  int        maximum;           /* px, or -1 */
};

G_DEFINE_FINAL_TYPE (VibeosClamp, vibeos_clamp, GTK_TYPE_WIDGET)

/* the child's width in `width`: all of it, or the maximum, and never under the child's minimum */
static int
child_width (VibeosClamp *self, int width)
{
  int minimum;

  gtk_widget_measure (self->child, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  if (self->maximum >= 0)
    width = MIN (width, self->maximum);
  return MAX (width, minimum);
}

static void
vibeos_clamp_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                      int *minimum_baseline, int *natural_baseline)
{
  VibeosClamp *self = VIBEOS_CLAMP (widget);

  *minimum = *natural = 0;
  if (!self->child || !gtk_widget_should_layout (self->child))
    return;
  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      gtk_widget_measure (self->child, orientation, for_size, minimum, natural, NULL, NULL);
      if (self->maximum >= 0)
        *natural = MAX (*minimum, MIN (*natural, self->maximum));
    }
  else
    /* the height for the width the child gets */
    gtk_widget_measure (self->child, orientation, for_size < 0 ? -1 : child_width (self, for_size), minimum, natural,
                        NULL, NULL);
}

static void
vibeos_clamp_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  VibeosClamp *self = VIBEOS_CLAMP (widget);
  int child;

  if (!self->child || !gtk_widget_should_layout (self->child))
    return;
  child = child_width (self, width);
  gtk_widget_allocate (self->child, child, height, -1,
                       gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT ((width - child) / 2, 0)));
}

static GtkSizeRequestMode
vibeos_clamp_get_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
vibeos_clamp_dispose (GObject *object)
{
  VibeosClamp *self = VIBEOS_CLAMP (object);

  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (vibeos_clamp_parent_class)->dispose (object);
}

static void
vibeos_clamp_class_init (VibeosClampClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = vibeos_clamp_dispose;
  widget_class->measure = vibeos_clamp_measure;
  widget_class->size_allocate = vibeos_clamp_size_allocate;
  widget_class->get_request_mode = vibeos_clamp_get_request_mode;
  gtk_widget_class_set_css_name (widget_class, "vibeos-clamp");
}

static void
vibeos_clamp_init (VibeosClamp *self)
{
  self->maximum = -1;
}

GtkWidget *
vibeos_clamp_new (GtkWidget *child, int maximum)
{
  VibeosClamp *self = g_object_new (VIBEOS_TYPE_CLAMP, NULL);

  g_return_val_if_fail (GTK_IS_WIDGET (child), GTK_WIDGET (self));
  self->child = child;
  self->maximum = maximum;
  gtk_widget_set_parent (child, GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

void
vibeos_clamp_set_maximum (VibeosClamp *self, int maximum)
{
  g_return_if_fail (VIBEOS_IS_CLAMP (self));
  if (self->maximum == maximum)
    return;
  self->maximum = maximum;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

int
vibeos_clamp_get_maximum (VibeosClamp *self)
{
  g_return_val_if_fail (VIBEOS_IS_CLAMP (self), -1);
  return self->maximum;
}

GtkWidget *
vibeos_clamp_get_child (VibeosClamp *self)
{
  g_return_val_if_fail (VIBEOS_IS_CLAMP (self), NULL);
  return self->child;
}
