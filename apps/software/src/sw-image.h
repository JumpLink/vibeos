/* SwImage: a picture from the store (an icon, a screenshot, a banner), loaded when it comes near the screen.
 *
 * The store's media URLs never change their bytes (docs/api.md, Caching), so a downloaded picture is kept for good
 * under ~/.cache/vibeos-software/images and never asked for again. Pictures are WebP, decoded by gdk-pixbuf's WebP
 * loader in a worker thread; at most six downloads run at once, the ones asked for last first (they are the ones on
 * the screen now).
 *
 * Until it is in, the picture is a plain tile of the surface's colour, so rows keep their height. A picture loads
 * once it is mapped and within a screen's height (or width) of the window: the scroll views it sits in are watched,
 * and a long list loads as it scrolls. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SW_TYPE_IMAGE (sw_image_get_type ())
G_DECLARE_FINAL_TYPE (SwImage, sw_image, SW, IMAGE, GtkWidget)

/* a picture `width` by `height` (either may be -1: then the other and `aspect` decide), its corners rounded by
 * `radius` */
GtkWidget *sw_image_new          (int width, int height, double radius);
void       sw_image_set_url      (SwImage *self, const char *url);
/* width / height of the picture to come: the widget's height follows its width (banners, screenshots) */
void       sw_image_set_aspect   (SwImage *self, double aspect);
/* drawn when there is no URL, or the picture could not be loaded (an icon name, symbolic) */
void       sw_image_set_fallback (SwImage *self, const char *icon_name);
/* a picture already at hand (no URL) */
void       sw_image_set_paintable (SwImage *self, GdkPaintable *paintable);

/* the picture behind `url` if it is in memory already, else NULL */
GdkTexture *sw_image_cache_lookup (const char *url);

G_END_DECLS
