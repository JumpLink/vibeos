/* FmIcon: a place's or a file's icon, drawn from the user's GTK icon theme (Adwaita unless another is set, and
 * whatever theme is set later): a glyph names the theme icon it stands for (folder, user-home, drive-harddisk,
 * user-trash or user-trash-full, folder-remote, network-server…), a file brings its content type's icon. An image's
 * thumbnail, when there is one, takes the icon's place. A symbolic icon (a theme without the coloured one) takes
 * the widget's colour. */
#pragma once

#include <gtk/gtk.h>

#include "fm-places.h"

G_BEGIN_DECLS

#define FM_TYPE_ICON (fm_icon_get_type ())
G_DECLARE_FINAL_TYPE (FmIcon, fm_icon, FM, ICON, GtkWidget)

GtkWidget *fm_icon_new         (FmGlyph glyph, int size);
/* the glyph's theme icon; it drops an icon set with fm_icon_set_gicon */
void       fm_icon_set_glyph   (FmIcon *self, FmGlyph glyph);
FmGlyph    fm_icon_get_glyph   (FmIcon *self);
/* a theme icon of its own (a content type's), in the glyph's place; NULL for the glyph's again */
void       fm_icon_set_gicon   (FmIcon *self, GIcon *icon);
void       fm_icon_set_texture (FmIcon *self, GdkTexture *texture);
/* a dimmed icon (a hidden file's) */
void       fm_icon_set_dimmed  (FmIcon *self, gboolean dimmed);

/* the theme icon a glyph stands for (a GThemedIcon with its fallbacks; the Recycle Bin's is the empty one) */
GIcon     *fm_glyph_get_icon   (FmGlyph glyph);

G_END_DECLS
