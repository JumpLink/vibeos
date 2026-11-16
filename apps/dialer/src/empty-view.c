/* The empty list: see empty-view.h. */
#include "empty-view.h"

#include <vibeos.h>

#define IMAGE_SIZE      108     /* the picture's height; an icon is scaled up to it */
#define BUTTON_MIN_W    88
#define BUTTON_HEIGHT   48
#define BUTTON_PADDING  16
#define BUTTON_COLOR    0x33666d

typedef struct
{
  EmptyAction callback;
  gpointer data;
} Action;

static void
on_clicked (GtkWidget *button, Action *action)
{
  action->callback (action->data);
}

GtkWidget *
empty_view_new (const char *icon_name, const char *message, const char *action, EmptyAction callback,
                gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *text = gtk_label_new (message);
  GtkWidget *space = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *image;

  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (box, "dialer-empty");
  if (icon_name[0] == '/')
    {
      /* a bitmap, fitted into a full-width box IMAGE_SIZE tall. The picture floats over a space of that height and
       * leaves the measuring to it: measured itself, it would ask for the height its aspect ratio gives at the
       * whole width (over twice IMAGE_SIZE for the speed-dial picture) */
      GtkWidget *picture = gtk_picture_new_for_resource (icon_name);
      GtkWidget *space = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

      gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
      gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
      gtk_widget_set_size_request (space, -1, IMAGE_SIZE);
      image = gtk_overlay_new ();
      gtk_overlay_set_child (GTK_OVERLAY (image), space);
      gtk_overlay_add_overlay (GTK_OVERLAY (image), picture);
    }
  else
    {
      /* a symbolic icon, tinted by dialer.css */
      image = gtk_image_new_from_icon_name (icon_name);
      gtk_image_set_pixel_size (GTK_IMAGE (image), IMAGE_SIZE);
      gtk_widget_add_css_class (image, "dialer-empty-image");
    }
  gtk_box_append (GTK_BOX (box), image);

  /* the message, wrapped and centred */
  gtk_label_set_wrap (GTK_LABEL (text), TRUE);
  gtk_label_set_justify (GTK_LABEL (text), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (text, "dialer-empty-text");
  vibeos_label_set_font_padding (text, 16);
  gtk_box_append (GTK_BOX (box), text);

  if (action)
    {
      /* the action: a filled pill, its text in capitals; a press shows no ripple */
      g_autofree char *caps = g_utf8_strup (action, -1);
      GtkWidget *label = gtk_label_new (caps);
      GtkWidget *button;
      Action *what = g_new (Action, 1);
      GdkRGBA fill = { ((BUTTON_COLOR >> 16) & 0xff) / 255.0, ((BUTTON_COLOR >> 8) & 0xff) / 255.0,
                       (BUTTON_COLOR & 0xff) / 255.0, 1 };

      gtk_widget_add_css_class (label, "dialer-empty-button");
      gtk_widget_set_margin_start (label, BUTTON_PADDING);
      gtk_widget_set_margin_end (label, BUTTON_PADDING);
      gtk_widget_set_size_request (label, BUTTON_MIN_W - 2 * BUTTON_PADDING, BUTTON_HEIGHT);
      button = vibeos_pressable_new (label);
      vibeos_pressable_set_fill (VIBEOS_PRESSABLE (button), &fill, FALSE);
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_NONE, BUTTON_HEIGHT / 2.0);
      gtk_widget_set_halign (button, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_top (button, 16);
      what->callback = callback;
      what->data = data;
      g_object_set_data_full (G_OBJECT (button), "empty-action", what, g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_clicked), what);
      gtk_box_append (GTK_BOX (box), button);
    }
  gtk_widget_set_size_request (space, -1, 40);
  gtk_box_append (GTK_BOX (box), space);
  return box;
}
