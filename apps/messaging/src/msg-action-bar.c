/* Messaging's action bar (msg-action-bar.h). */
#include "msg-action-bar.h"

#include <glib/gi18n.h>
#include <neo.h>

#define HEIGHT 56
#define UP_WIDTH 56             /* the up button's least width */
#define INSET 16                /* the title's, after the up arrow or from the start */
#define ACTION_WIDTH 48
#define OVERFLOW_WIDTH 36       /* narrower than an action icon */

static GtkWidget *
icon_button (const char *icon, int width, const char *label)
{
  GtkWidget *image = gtk_image_new_from_icon_name (icon);
  GtkWidget *button;

  gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
  button = neo_pressable_new (image);
  gtk_widget_set_size_request (button, width, HEIGHT);
  /* the ripple: a circle around the icon, not the whole button */
  neo_pressable_set_ripple (NEO_PRESSABLE (button), NEO_RIPPLE_CIRCLE, 20);
  gtk_widget_add_css_class (button, "msg-action-icon");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return button;
}

GtkWidget *
msg_action_bar_new (gboolean up, const char *title)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *end = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "msg-action-bar");
  gtk_widget_set_size_request (bar, -1, HEIGHT);
  if (up)
    {
      GtkWidget *button = icon_button ("messaging-back-symbolic", UP_WIDTH, _("Back"));

      /* named "Back", the button the shell's back gesture clicks; "Navigate up" is its description */
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                      _("Navigate up"), -1);
      gtk_box_append (GTK_BOX (bar), button);
      g_object_set_data (G_OBJECT (bar), "up", button);
    }
  /* the title's start: UP_WIDTH plus INSET with the up arrow, else INSET */
  gtk_widget_set_margin_start (label, INSET);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "msg-action-title");
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (bar), label);
  g_object_set_data (G_OBJECT (bar), "title", label);
  gtk_box_append (GTK_BOX (bar), end);
  g_object_set_data (G_OBJECT (bar), "end", end);
  return bar;
}

GtkWidget *
msg_action_bar_get_up (GtkWidget *bar)
{
  return g_object_get_data (G_OBJECT (bar), "up");
}

GtkWidget *
msg_action_bar_get_title (GtkWidget *bar)
{
  return g_object_get_data (G_OBJECT (bar), "title");
}

void
msg_action_bar_set_title (GtkWidget *bar, const char *title)
{
  GtkWidget *label = msg_action_bar_get_title (bar);

  gtk_label_set_text (GTK_LABEL (label), title);
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
}

GtkWidget *
msg_action_bar_add_action (GtkWidget *bar, const char *icon, const char *label)
{
  GtkWidget *end = g_object_get_data (G_OBJECT (bar), "end");
  GtkWidget *overflow = g_object_get_data (G_OBJECT (bar), "overflow");
  GtkWidget *button = icon_button (icon, ACTION_WIDTH, label);

  gtk_box_insert_child_after (GTK_BOX (end), button, overflow ? gtk_widget_get_prev_sibling (overflow)
                                                              : gtk_widget_get_last_child (end));
  return button;
}

GtkWidget *
msg_action_bar_add_close (GtkWidget *bar, const char *label)
{
  GtkWidget *button = icon_button ("messaging-cancel-symbolic", UP_WIDTH, _("Back"));

  /* "Back" as the up arrow is, so the back gesture leaves select mode */
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, label, -1);
  gtk_box_prepend (GTK_BOX (bar), button);
  return button;
}

GtkWidget *
msg_action_bar_add_overflow (GtkWidget *bar)
{
  GtkWidget *end = g_object_get_data (G_OBJECT (bar), "end");
  GtkWidget *button = icon_button ("messaging-more-symbolic", OVERFLOW_WIDTH, _("More options"));

  gtk_box_append (GTK_BOX (end), button);
  g_object_set_data (G_OBJECT (bar), "overflow", button);
  return button;
}

void
msg_action_bar_set_custom (GtkWidget *bar, GtkWidget *custom)
{
  GtkWidget *title = msg_action_bar_get_title (bar);

  gtk_widget_set_visible (title, FALSE);
  gtk_widget_set_hexpand (custom, TRUE);
  gtk_box_insert_child_after (GTK_BOX (bar), custom, title);
}
