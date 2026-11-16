/* Launcher icons: the theme's icon, an icon pack's art, or the app icon wrapped/shaped the way VibeOS does. */
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

typedef struct _Icons Icons;

typedef struct {
  char *id, *title, *dir;
} IconPack;

Icons     *icons_new        (GSettings *settings);
void       icons_free       (Icons *self);
/* installed packs, a user's pack winning over a system one of the same name; g_ptr_array_unref frees them */
GPtrArray *icons_list_packs (void);
/* The icon for `info` at `px` device pixels, ready to show: a pack/shape icon from the cache, or the stock icon.
 * *pending is set when a custom icon still has to be drawn (icons_render). */
GdkPixbuf *icons_lookup     (Icons *self, GAppInfo *info, int px, gboolean *pending);
/* Draws the custom icon (up to ~100 ms of cairo); NULL when the stock icon stays. */
GdkPixbuf *icons_render     (Icons *self, GAppInfo *info, int px);
/* the cache's file of a custom icon at px, when one was drawn (NULL for the stock icon) */
char      *icons_cached_path (Icons *self, GAppInfo *info, int px);
/* the app's stock icon at px, never NULL */
GdkPixbuf *icons_stock      (GAppInfo *info, int px);
void       icons_reset      (Icons *self);   /* the settings changed: read the pack again */
