/* Another app's action, as GApplication exports them (org.freedesktop.Application.ActivateAction on the app's id
 * and path): the app is started by the bus if it is not running, and its window may come to the front, with the
 * activation token of @from's display in the platform data (xdg-activation). */
#pragma once

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the app is installed: it has a desktop file */
gboolean app_action_installed (const char *app_id);
/* its @action with @parameter (floating or not; NULL: none). FALSE, and nothing sent, if the app is not installed */
gboolean app_action_activate (GtkWidget *from, const char *app_id, const char *action, GVariant *parameter);

G_END_DECLS
