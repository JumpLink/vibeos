/* The drawer: a panel over a scrim, with three tabs: Locations (Home, Main storage, the media, System,
 * the Recycle Bin, then the last-visited locations), Bookmarks and History. It slides in from ☰ or a swipe from the
 * left edge, and settles open or closed from where the finger left it. */
#pragma once

#include <gtk/gtk.h>

#include "fm-window.h"

G_BEGIN_DECLS

FmDrawer   *fm_drawer_new        (FmShell *shell);
void        fm_drawer_free       (FmDrawer *drawer);
/* the layer to put over the window's content */
GtkWidget  *fm_drawer_get_widget (FmDrawer *drawer);
/* the left-edge swipe that opens it, on @content */
void        fm_drawer_attach_edge (FmDrawer *drawer, GtkWidget *content);
void        fm_drawer_open       (FmDrawer *drawer, gboolean open);
gboolean    fm_drawer_is_open    (FmDrawer *drawer);
/* the lists again: a visit, a bookmark, a mount came or went */
void        fm_drawer_refresh    (FmDrawer *drawer);

G_END_DECLS
