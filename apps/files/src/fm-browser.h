/* The folder page (spec §2, §3, §7): the path bar over the file list, for any location GIO can list (a local
 * folder, a server's share, the Recycle Bin, Recent files, the Local Network). It lists in batches without blocking,
 * mounts what is not mounted yet, counts the folders' items in a worker, follows changes with a GFileMonitor,
 * filters by the search bar and searches the folders under it. */
#pragma once

#include <gtk/gtk.h>

#include "fm-list.h"
#include "fm-window.h"

G_BEGIN_DECLS

FmBrowser  *fm_browser_new           (FmShell *shell);
void        fm_browser_free          (FmBrowser *browser);
GtkWidget  *fm_browser_get_widget    (FmBrowser *browser);
FmList     *fm_browser_get_list      (FmBrowser *browser);

/* lists @location (mounting it first when it has to be) */
void        fm_browser_load          (FmBrowser *browser, GFile *location);
/* lists it again, keeping the scroll position; @select_name: a row to scroll to once it is there (New folder) */
void        fm_browser_reload        (FmBrowser *browser, const char *select_name);
GFile      *fm_browser_get_location  (FmBrowser *browser);
FmPlace    *fm_browser_get_place     (FmBrowser *browser);
/* the Recycle Bin, Recent files or the Local Network: no new folder, no paste */
gboolean    fm_browser_is_virtual    (FmBrowser *browser);
gboolean    fm_browser_is_trash      (FmBrowser *browser);
/* whether a row of that name is listed (Create Folder, Rename: "File or folder already exists") */
gboolean    fm_browser_has_name      (FmBrowser *browser, const char *name);

/* search: the bar's text filters the rows; Enter searches the folders under this one */
void        fm_browser_search_begin  (FmBrowser *browser);
void        fm_browser_search_filter (FmBrowser *browser, const char *text);
void        fm_browser_search_run    (FmBrowser *browser, const char *text);
void        fm_browser_search_end    (FmBrowser *browser);
gboolean    fm_browser_is_searching  (FmBrowser *browser);
gboolean    fm_browser_search_busy   (FmBrowser *browser);

/* a setting that changes the listing (hidden files, the order, thumbnails) */
void        fm_browser_settings_changed (FmBrowser *browser);

G_END_DECLS
