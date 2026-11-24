/* The drawer: a panel over a scrim, with three tabs: Locations (Home, Main storage, the media, System,
 * the Recycle Bin, then the last-visited locations), Bookmarks and History. It slides in from ☰ or a swipe from the
 * left edge, and settles open or closed from where the finger left it.
 *
 * On a tablet held sideways it is docked instead: a sidebar always there at the window's start, beside the content,
 * with no ☰, scrim or swipe. Held upright, a rail of the places is there instead, with ☰ over it opening the drawer. */
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
/* docked: the panel moves into @dock (a horizontal box, at its start) and stays there; undocked it goes back over
 * the content, closed. A docked drawer is never "open": Back and ☰ leave it alone. */
void        fm_drawer_set_docked (FmDrawer *drawer, GtkWidget *dock, gboolean docked);
gboolean    fm_drawer_is_docked  (FmDrawer *drawer);
/* the rail of a tablet held upright: ☰ (the drawer, over the content) over Home, the storages, the media and the
 * Recycle Bin. A box for the window to place at its start and show or hide; it follows the places as they change */
GtkWidget  *fm_drawer_get_rail   (FmDrawer *drawer);
/* the rail's selection: the place the window shows */
void        fm_drawer_sync_rail  (FmDrawer *drawer);

G_END_DECLS
