/* The apps a launcher shows and the names it shows them by. */
#pragma once

#include <gio/gdesktopappinfo.h>

/* desktop id -> GDesktopAppInfo (owned), the visible .desktop entries */
GHashTable  *apps_installed  (void);
const char  *app_name        (GAppInfo *info);
/* "<id>.desktop" or its lower-case variant for a window's app id, or NULL */
GAppInfo    *app_for_app_id  (const char *app_id);
gint         app_compare     (gconstpointer a, gconstpointer b);   /* two GAppInfo* by name */

/* desktop ids another app took over: { from, to }. While "to" is installed, "from" is not one of the apps, and its
 * places on the home screen go to "to" (model_reconcile). */
extern const char *const apps_replaced[][2];
extern const guint       apps_n_replaced;

/* The desktop id to keep for one read from a layout or from hidden-from-home: the new name of an app that had
 * another id up to 0.1.6 (legacy-ids.h) once its renamed desktop file is installed and the old one is not, else the
 * id as it is. Layouts the user saved, a ROM's in /etc/xdg and the old ids in hidden-from-home all go through it. */
char        *apps_current_id (const char *id);
