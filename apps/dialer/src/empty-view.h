/* What an empty list shows: a picture, a line of text under it and, if there is something to do about
 * it, a filled button ("Make a call"), the whole block centred in the page. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*EmptyAction) (gpointer data);

/* @icon_name: a symbolic icon, or a resource path ("/neo/Dialer/images/…") for a bitmap.
 * @action NULL: no button; a tap on the button calls @callback with @data */
GtkWidget *empty_view_new (const char *icon_name, const char *message, const char *action, EmptyAction callback,
                           gpointer data);

G_END_DECLS
