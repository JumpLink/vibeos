/* NbNavigator: the browser's activities (the browser page, History, Favorites, Settings, Downloads, Desktop
 * shortcuts) as pages in one tab's window, with Android's activity transitions between them (a copy of Clock's
 * ClockNavigator): a page opened comes in from 96 dp to the right while fading in, over the one it covers, which
 * moves 96 dp left; going back reverses it. The page on top is the one shown and the one with the focus. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NB_TYPE_NAVIGATOR (nb_navigator_get_type ())
G_DECLARE_FINAL_TYPE (NbNavigator, nb_navigator, NB, NAVIGATOR, GtkWidget)

GtkWidget *nb_navigator_new (void);
/* opens @page over the others (startActivity) */
void       nb_navigator_push (NbNavigator *self, GtkWidget *page);
/* closes the top page, back to the one under it (finish); the first page stays. FALSE when there was none */
gboolean   nb_navigator_pop (NbNavigator *self);
/* swaps the top page for @page with the open transition: the contact picker becomes the conversation it picked */
void       nb_navigator_replace (NbNavigator *self, GtkWidget *page);
/* closes every page down to the first */
void       nb_navigator_pop_to_root (NbNavigator *self);
GtkWidget *nb_navigator_get_top (NbNavigator *self);
guint      nb_navigator_get_depth (NbNavigator *self);

/* Signals: "top-changed" (): a page came to the top (its transition may still run) */

G_END_DECLS
