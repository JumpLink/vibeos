/* Home (spec §5): the grid of locations: Main storage and the media with their free space, the XDG folders,
 * Recent files, the Recycle Bin, Remote and the Local Network. */
#pragma once

#include <gtk/gtk.h>

#include "fm-window.h"

G_BEGIN_DECLS

FmHome     *fm_home_new        (FmShell *shell);
void        fm_home_free       (FmHome *home);
GtkWidget  *fm_home_get_widget (FmHome *home);
/* the tiles again: a medium came or went, the free space and counts changed */
void        fm_home_refresh    (FmHome *home);

G_END_DECLS
