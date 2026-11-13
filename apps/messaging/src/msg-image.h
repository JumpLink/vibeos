/* Messaging's bitmaps (PNGs from the APK, 3 device px to the GTK px) at their size in GTK px: a picture of a
 * 594 × 308 image is 198 × 103 px, where GtkPicture would make it 594 px wide. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* a GtkPicture of the image at @resource, @width × @height px */
GtkWidget *msg_image_new (const char *resource, int width, int height);

G_END_DECLS
