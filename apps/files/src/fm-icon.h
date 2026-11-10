/* FmIcon: the app's own coloured icons (spec, "Icon colours of our own drawings"): folders, the house, drives,
 * media, the Recycle Bin, the remote folders with their badges and the file types, drawn in code at any size with a
 * Material Symbol on some of them. None of them is File Manager+'s artwork; they only sit where its icons sit.
 * An image's thumbnail, when there is one, takes the drawing's place. */
#pragma once

#include <gtk/gtk.h>

#include "fm-places.h"

G_BEGIN_DECLS

#define FM_TYPE_ICON (fm_icon_get_type ())
G_DECLARE_FINAL_TYPE (FmIcon, fm_icon, FM, ICON, GtkWidget)

GtkWidget *fm_icon_new         (FmGlyph glyph, int size);
void       fm_icon_set_glyph   (FmIcon *self, FmGlyph glyph);
FmGlyph    fm_icon_get_glyph   (FmIcon *self);
void       fm_icon_set_texture (FmIcon *self, GdkTexture *texture);
/* a dimmed icon (a hidden file's) */
void       fm_icon_set_dimmed  (FmIcon *self, gboolean dimmed);

/* the drawing at (x, y), @size dp square, for widgets that draw their own */
void       fm_icon_snapshot    (GtkSnapshot *snapshot, GtkWidget *widget, FmGlyph glyph, double x, double y,
                                double size);

G_END_DECLS
