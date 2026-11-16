/* Pictures into textures: wallpapers and parallax layers (gdk-pixbuf), and icons from the icon theme (the
 * freedesktop lookup, PNG or SVG through librsvg), all as premultiplied BGRA. */
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include "gfx.h"

/* `path` scaled to w × h: cover crops the middle out of a picture of another shape, else it is stretched */
GdkPixbuf *image_load          (const char *path, int w, int h, gboolean cover);
GfxTex    *image_texture       (GdkPixbuf *pb);

/* the theme's file for an icon name at px, or NULL; the theme is org.gnome.desktop.interface icon-theme */
char      *image_icon_path     (const char *name, int px);
/* an app's icon (a themed icon or a file) drawn at px × px, the art centred; NULL when there is none */
GdkPixbuf *image_icon          (GIcon *icon, int px);
void       image_theme_reset   (void);             /* the icon theme changed */
