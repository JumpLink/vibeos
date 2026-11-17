/* Segmented cards (msg-card.h). */
#include "msg-card.h"

#include <vibeos.h>

#define GAP 2
#define INNER_RADIUS 4
#define ICON 24
#define ICON_X 16
#define TEXT_X 48               /* the title's inset with an icon */
#define TEXT_PLAIN 16

GtkWidget *
msg_card_group_new (void)
{
  GtkWidget *group = gtk_box_new (GTK_ORIENTATION_VERTICAL, GAP);

  gtk_widget_add_css_class (group, "msg-card-group");
  return group;
}

void
msg_card_group_append (GtkWidget *group, GtkWidget *row)
{
  GtkWidget *before = gtk_widget_get_last_child (group);

  gtk_widget_add_css_class (row, "msg-card");
  if (before)
    gtk_widget_remove_css_class (before, "last");
  else
    gtk_widget_add_css_class (row, "first");
  gtk_widget_add_css_class (row, "last");
  gtk_box_append (GTK_BOX (group), row);
}

GtkWidget *
msg_card_row_new (GtkWidget *child)
{
  GtkWidget *row = vibeos_pressable_new (child);

  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, INNER_RADIUS);
  gtk_widget_set_overflow (row, GTK_OVERFLOW_HIDDEN);
  return row;
}

GtkWidget *
msg_card_menu_row_new (const char *icon_name, const char *title, int height)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *row;

  if (icon_name)
    {
      GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

      gtk_image_set_pixel_size (GTK_IMAGE (icon), ICON);
      gtk_widget_set_margin_start (icon, ICON_X);
      gtk_widget_add_css_class (icon, "msg-card-icon");
      gtk_box_append (GTK_BOX (box), icon);
    }
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_margin_start (label, icon_name ? TEXT_X - ICON_X - ICON : TEXT_PLAIN);
  gtk_widget_set_margin_end (label, TEXT_PLAIN);
  gtk_widget_add_css_class (label, "msg-card-title");
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_set_size_request (box, -1, height);
  row = msg_card_row_new (box);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return row;
}
