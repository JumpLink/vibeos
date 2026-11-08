/* Messaging's bitmaps (the xxhdpi PNGs from the APK) at their size in dp: a picture of a 594 × 308 px image is 198 ×
 * 103 dp, the way Android draws a drawable-xxhdpi one, where GtkPicture would make it 594 px wide. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* a GtkPicture of the image at @resource, @width × @height dp */
GtkWidget *msg_image_new (const char *resource, int width, int height);

G_END_DECLS
