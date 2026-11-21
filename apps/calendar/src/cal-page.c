/* CalPage (cal-page.h). */
#include "cal-page.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <vibeos.h>

struct _CalPage {
  GtkBox parent;
  GtkWidget *window;
  CalPageBack back;
  gpointer back_data;
};

G_DEFINE_FINAL_TYPE (CalPage, cal_page, GTK_TYPE_BOX)

static void
cal_page_class_init (CalPageClass *klass)
{
}

static void
cal_page_init (CalPage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "cal-page");
}

GtkWidget *
cal_page_new (GtkWidget *window)
{
  CalPage *self = g_object_new (CAL_TYPE_PAGE, NULL);

  self->window = window;
  return GTK_WIDGET (self);
}

GtkWidget *
cal_page_get_window (CalPage *self)
{
  return self->window;
}

void
cal_page_set_back_handler (CalPage *self, CalPageBack back, gpointer data)
{
  self->back = back;
  self->back_data = data;
}

void
cal_page_back (CalPage *self)
{
  if (self->back && self->back (self, self->back_data))
    return;
  cal_window_pop (self->window);
}

static void
on_back (VibeosPressable *button, CalPage *self)
{
  cal_page_back (self);
}

GtkWidget *
cal_page_action (const char *icon_name, const char *name)
{
  GtkWidget *button = vibeos_icon_button_new (icon_name);

  gtk_widget_add_css_class (button, "cal-action");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  return button;
}

GtkWidget *
cal_page_toolbar (CalPage *self, const char *title, GtkWidget **actions)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *back = cal_page_action ("cal-back-symbolic", _("Back"));
  GtkWidget *end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "cal-toolbar");
  /* "Back" is the name the shell's back gesture presses (extension/a11y.js) */
  gtk_widget_add_css_class (back, "cal-toolbar-back");
  g_signal_connect (back, "clicked", G_CALLBACK (on_back), self);
  gtk_box_append (GTK_BOX (bar), back);
  if (title)
    {
      GtkWidget *label = gtk_label_new (title);

      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class (label, "cal-toolbar-title");
      /* centred with font padding, its line the font's bounding box (a little lower than a plain label's) */
      vibeos_label_set_font_padding_sp (label, 22);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_box_append (GTK_BOX (bar), label);
    }
  else
    gtk_widget_set_hexpand (end, TRUE);
  gtk_widget_set_halign (end, GTK_ALIGN_END);
  gtk_widget_add_css_class (end, "cal-toolbar-actions");
  gtk_box_append (GTK_BOX (bar), end);
  if (actions)
    *actions = end;
  return bar;
}
