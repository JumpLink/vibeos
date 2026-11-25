/* TermToolbar: see term-toolbar.h */
#include "term-toolbar.h"

#include <vibeos.h>

#define MARGIN_V   8          /* between the bar and the text it is about */
#define MARGIN_H   16         /* the bar keeps this far inside the window */

struct _TermToolbar {
  GtkWidget  parent;
  GtkWidget *box;
};

G_DEFINE_FINAL_TYPE (TermToolbar, term_toolbar, GTK_TYPE_WIDGET)

static void
term_toolbar_dispose (GObject *object)
{
  TermToolbar *self = TERM_TOOLBAR (object);

  g_clear_pointer (&self->box, gtk_widget_unparent);
  G_OBJECT_CLASS (term_toolbar_parent_class)->dispose (object);
}

static void
term_toolbar_class_init (TermToolbarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = term_toolbar_dispose;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "term-toolbar");
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_DIALOG);
}

static void
term_toolbar_init (TermToolbar *self)
{
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));
  gtk_widget_set_focusable (GTK_WIDGET (self), FALSE);
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_START);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_START);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

TermToolbar *
term_toolbar_new (void)
{
  return g_object_new (TERM_TYPE_TOOLBAR, NULL);
}

GtkWidget *
term_toolbar_add (TermToolbar *self, const char *label)
{
  GtkWidget *button = vibeos_text_button_new (label);

  gtk_widget_set_focusable (button, FALSE);
  gtk_box_append (GTK_BOX (self->box), button);
  return button;
}

void
term_toolbar_show_at (TermToolbar *self, GtkWidget *widget, const graphene_rect_t *content)
{
  GtkOverlay *overlay = vibeos_window_get_overlay (widget);
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  graphene_point_t a, b;
  int width, height, bar_w, bar_h, x, y;

  g_return_if_fail (overlay != NULL);
  if (!parent) {
    gtk_overlay_add_overlay (overlay, GTK_WIDGET (self));
    gtk_overlay_set_measure_overlay (overlay, GTK_WIDGET (self), FALSE);
    gtk_overlay_set_clip_overlay (overlay, GTK_WIDGET (self), TRUE);
  }
  if (!gtk_widget_compute_point (widget, GTK_WIDGET (overlay), &content->origin, &a)
      || !gtk_widget_compute_point (widget, GTK_WIDGET (overlay),
                                    &GRAPHENE_POINT_INIT (content->origin.x + content->size.width,
                                                          content->origin.y + content->size.height), &b))
    return;
  width = gtk_widget_get_width (GTK_WIDGET (overlay));
  height = gtk_widget_get_height (GTK_WIDGET (overlay));
  /* (a hidden widget measures 0, and a widget's measure has its margins in it: the last place's) */
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 0);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 0);
  gtk_widget_measure (GTK_WIDGET (self), GTK_ORIENTATION_HORIZONTAL, -1, NULL, &bar_w, NULL, NULL);
  gtk_widget_measure (GTK_WIDGET (self), GTK_ORIENTATION_VERTICAL, bar_w, NULL, &bar_h, NULL, NULL);
  /* centred on the content, inside the window's margins; above it if it fits, else below */
  x = (int) ((a.x + b.x) / 2 - bar_w / 2.0);
  x = CLAMP (x, MARGIN_H, MAX (MARGIN_H, width - MARGIN_H - bar_w));
  y = (int) a.y - MARGIN_V - bar_h;
  if (y < 0)
    y = (int) b.y + MARGIN_V;
  y = CLAMP (y, 0, MAX (0, height - bar_h));
  gtk_widget_set_margin_start (GTK_WIDGET (self), x);
  gtk_widget_set_margin_top (GTK_WIDGET (self), y);
}

void
term_toolbar_hide (TermToolbar *self)
{
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

gboolean
term_toolbar_is_shown (TermToolbar *self)
{
  return gtk_widget_get_visible (GTK_WIDGET (self)) && gtk_widget_get_parent (GTK_WIDGET (self)) != NULL;
}
