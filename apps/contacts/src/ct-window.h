/* The Contacts window: a stack of pages (ct-page.h), the home page at the bottom. A page pushed on top slides in
 * from 96 px to the right while it fades in, and the one under it slides 96 px to the left; Back slides them the
 * other way. QuickContact is translucent: it covers the page under it with its own scrim and animates itself.
 *
 * The home page has three tabs, Contacts, Highlights and Organize: on a phone under a navigation bar at the bottom
 * (its items beside their labels when the phone is on its side), on a tablet beside a rail at the start, which has
 * the FAB at its top. On a tablet, upright too, Contacts is two panes: the list, and the contact chosen in it beside
 * it instead of over it.
 *
 * The window also has what outlives a page: the toast and the snackbar. */
#pragma once

#include "ct-store.h"

#include <gtk/gtk.h>
#include <vibeos.h>

G_BEGIN_DECLS

GtkWidget *ct_window_new           (GtkApplication *app, CtStore *store, GSettings *settings);

CtStore   *ct_window_get_store     (GtkWidget *window);
GSettings *ct_window_get_settings  (GtkWidget *window);

/* a page on top, and a page away (with whatever is above it) */
void       ct_window_push          (GtkWidget *window, GtkWidget *page);
void       ct_window_pop           (GtkWidget *window, GtkWidget *page);
/* the page on top, and the home page */
GtkWidget *ct_window_top           (GtkWidget *window);
GtkWidget *ct_window_home          (GtkWidget *window);
/* the home page's tab: 0 Contacts, 1 Highlights, 2 Organize */
void       ct_window_select_tab    (GtkWidget *window, int tab);

/* "Text copied", "Contact saved" (a short toast) */
void       ct_window_toast         (GtkWidget *window, const char *text);
/* the full-width snackbar ("Label deleted" UNDO), shown long */
void       ct_window_snackbar      (GtkWidget *window, const char *text, const char *action,
                                    VibeosSnackbarAction callback, gpointer data);
VibeosSnackbar *ct_window_get_snackbar (GtkWidget *window);

/* the screens, opened from anywhere (the list, another app's request) */
void       ct_window_show_contact  (GtkWidget *window, const char *uid);          /* QuickContact */
void       ct_window_edit_contact  (GtkWidget *window, const char *uid);          /* the editor */
/* the editor for a new contact, with @prefill's fields (a number from the Dialer) when not NULL */
void       ct_window_new_contact   (GtkWidget *window, EContact *prefill);
/* the people list: back to the top screen (a request from another app lands there) */
GtkWidget *ct_window_get_people    (GtkWidget *window);

G_END_DECLS
