/* The apps a launcher shows and the names it shows them by. */
#pragma once

#include <gio/gdesktopappinfo.h>

/* desktop id -> GDesktopAppInfo (owned), the visible .desktop entries */
GHashTable  *apps_installed  (void);
const char  *app_name        (GAppInfo *info);
/* "<id>.desktop" or its lower-case variant for a window's app id, or NULL */
GAppInfo    *app_for_app_id  (const char *app_id);
gint         app_compare     (gconstpointer a, gconstpointer b);   /* two GAppInfo* by name */
