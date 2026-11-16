/* CalcToast: a short notice inside the app's own window, the "Text copied" a result's Copy shows.
 *
 * A pill with the app's icon and the text, centred, a little above the bottom. It fades in, stays 2 s and fades
 * out; a new one replaces the one showing.
 *
 * A libvibeos candidate: any app that confirms with a toast would want it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* shows `text` over the window `widget` is in (made by vibeos_window_new) */
void calc_toast_show (GtkWidget *widget, const char *text);

G_END_DECLS
