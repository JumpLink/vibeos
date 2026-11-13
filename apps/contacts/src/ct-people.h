/* The contact list screen. The toolbar with the drawer button, the title and
 * search; the list; the FAB; the drawer (My info, Contacts, the labels, the accounts, Settings); search; selection
 * mode (share, delete); a label's own list with its menu; and the empty states.
 *
 * It is the window's bottom page, so its Back is asked before the window closes: it closes the drawer, leaves
 * selection or search, or goes from a label or an account back to all contacts, and says FALSE when there is
 * nothing left to leave. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_people_new  (GtkWidget *window);
/* Back on the list: TRUE when it dealt with it */
gboolean   ct_people_back (GtkWidget *people);
/* the list as the launcher opens it: no drawer, search, selection or label */
void       ct_people_reset (GtkWidget *people);

G_END_DECLS
