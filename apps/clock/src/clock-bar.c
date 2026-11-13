/* The second-level pages' action bar: see clock-bar.h. */
#include "clock-bar.h"

#include <glib/gi18n.h>
#include <neo.h>

#define HEIGHT 64
#define UP_WIDTH 56             /* the Up button, as tall as it is wide */
#define TITLE_START 16          /* the title's gap after the Up button */

static GtkWidget *
icon_button (const char *icon, int width, int height, const char *label)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  button = neo_pressable_new (image);
  gtk_widget_set_size_request (button, width, height);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  /* the ripple: a circle around the icon, not the whole button */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (button, "clock-bar-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return button;
}

GtkWidget *
clock_bar_new (const char *title)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *up = icon_button ("clock-back-symbolic", UP_WIDTH, UP_WIDTH, _("Back"));
  GtkWidget *end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *middle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "clock-bar");
  gtk_widget_set_size_request (bar, -1, HEIGHT);
  /* the button sits at the top of the bar, not centred in it */
  gtk_widget_set_valign (up, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (up), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Navigate up"), -1);
  gtk_box_append (GTK_BOX (bar), up);
  g_object_set_data (G_OBJECT (bar), "up", up);

  gtk_widget_set_hexpand (middle, TRUE);
  if (title)
    {
      GtkWidget *label = gtk_label_new (title);

      /* the title, its box (font padding included) centred on the bar */
      gtk_widget_set_margin_start (label, TITLE_START);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class (label, "clock-bar-title");
      neo_label_set_font_padding (label, 20);
      gtk_box_append (GTK_BOX (middle), label);
    }
  gtk_box_append (GTK_BOX (bar), middle);
  g_object_set_data (G_OBJECT (bar), "middle", middle);
  gtk_box_append (GTK_BOX (bar), end);
  g_object_set_data (G_OBJECT (bar), "end", end);
  return bar;
}

GtkWidget *
clock_bar_get_up (GtkWidget *bar)
{
  return g_object_get_data (G_OBJECT (bar), "up");
}

GtkWidget *
clock_bar_add_icon (GtkWidget *bar, const char *icon, int width, const char *label)
{
  GtkWidget *button = icon_button (icon, width, 48, label);

  gtk_box_append (GTK_BOX (g_object_get_data (G_OBJECT (bar), "end")), button);
  return button;
}

void
clock_bar_set_custom (GtkWidget *bar, GtkWidget *custom)
{
  GtkWidget *middle = g_object_get_data (G_OBJECT (bar), "middle");

  gtk_widget_set_hexpand (custom, TRUE);
  gtk_box_append (GTK_BOX (middle), custom);
}
