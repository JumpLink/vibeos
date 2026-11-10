/* CtPage (ct-page.h). */
#include "ct-page.h"
#include "ct-ui.h"
#include "ct-window.h"

#include <neo.h>

struct _CtPage {
  GtkBox parent;
  GtkWidget *window;
  CtPageBack back;
  gpointer back_data;
  gboolean translucent;
  int width, height;
};

G_DEFINE_FINAL_TYPE (CtPage, ct_page, GTK_TYPE_BOX)

static guint resized_signal;

/* "resized" before the children are placed, so that what it changes (a margin) takes effect in this same pass */
static void
ct_page_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CtPage *self = CT_PAGE (widget);

  if (width != self->width || height != self->height)
    {
      self->width = width;
      self->height = height;
      g_signal_emit (self, resized_signal, 0, width, height);
    }
  GTK_WIDGET_CLASS (ct_page_parent_class)->size_allocate (widget, width, height, baseline);
}

static void
ct_page_class_init (CtPageClass *klass)
{
  GTK_WIDGET_CLASS (klass)->size_allocate = ct_page_size_allocate;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-page");
  resized_signal = g_signal_new ("resized", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void
ct_page_init (CtPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}

GtkWidget *
ct_page_new (GtkWidget *window)
{
  CtPage *self = g_object_new (CT_TYPE_PAGE, NULL);

  self->window = window;
  return GTK_WIDGET (self);
}

GtkWidget *
ct_page_get_window (CtPage *self)
{
  return self->window;
}

void
ct_page_set_back_handler (CtPage *self, CtPageBack back, gpointer data)
{
  self->back = back;
  self->back_data = data;
}

void
ct_page_back (CtPage *self)
{
  if (self->back && self->back (self, self->back_data))
    return;
  ct_window_pop (self->window, GTK_WIDGET (self));
}

void
ct_page_set_translucent (CtPage *self, gboolean translucent)
{
  self->translucent = translucent;
  if (translucent)
    gtk_widget_add_css_class (GTK_WIDGET (self), "translucent");
}

gboolean
ct_page_get_translucent (CtPage *self)
{
  return self->translucent;
}

static void
on_nav (NeoPressable *button, CtPage *self)
{
  ct_page_back (self);
}

GtkWidget *
ct_page_toolbar (CtPage *self, const char *css_class, const char *nav_icon, const char *nav_name, const char *title,
                 GtkWidget **actions, GtkWidget **title_label)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label;

  gtk_widget_add_css_class (bar, "ct-toolbar");
  if (css_class)
    gtk_widget_add_css_class (bar, css_class);
  if (nav_icon)
    {
      /* the navigation button: 56 x 56 dp, a borderless ripple of half its diagonal (#1fffffff, §1.2) */
      GtkWidget *nav = ct_icon_button (nav_icon, nav_name, CT_TOOLBAR_NAV_W_DP, CT_TOOLBAR_H_DP,
                                       CT_RIPPLE_APPCOMPAT);

      gtk_widget_add_css_class (nav, "ct-toolbar-nav");
      g_signal_connect (nav, "clicked", G_CALLBACK (on_nav), self);
      gtk_box_append (GTK_BOX (bar), nav);
    }
  /* contentInsetStartWithNavigation: the title starts 72 dp in, 16 dp after the 56 dp button */
  label = ct_label (title ? title : "", "ct-toolbar-title", CT_TOOLBAR_TITLE_SP);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_box_append (GTK_BOX (bar), label);
  gtk_widget_set_halign (end, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (bar), end);
  if (actions)
    *actions = end;
  if (title_label)
    *title_label = label;
  return bar;
}
