/* The Contacts tab: the contact list with its head (the search pill with the account picture, the filter row, its
 * chips and "Your info"), the FAB, selection mode (share, delete), a label's own list with its menu, the labels
 * page, search, and the empty states.
 *
 * Its Back is asked before the window closes: it leaves selection, a label, the filters or an address book, and
 * says FALSE when there is nothing left to leave. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_people_new  (GtkWidget *window);
/* Back on the list: TRUE when it dealt with it */
gboolean   ct_people_back (GtkWidget *people);
/* the list as the launcher opens it: no search, selection, label or filter */
void       ct_people_reset (GtkWidget *people);

/* another search pill like the list's, for the other tabs: it opens the same search and the same account sheet */
GtkWidget *ct_people_new_pill (GtkWidget *people);
void       ct_people_open_search (GtkWidget *people);
/* every label with its count, and "Create label" */
void       ct_people_show_labels (GtkWidget *people);
/* the members of @label */
void       ct_people_show_label (GtkWidget *people, const char *label);
/* only the contacts of the address book @book_uid (NULL: all) */
void       ct_people_show_account (GtkWidget *people, const char *book_uid);
/* the list's own FAB: off where the window has one elsewhere (a tablet's rail) */
void       ct_people_set_fab_shown (GtkWidget *people, gboolean shown);
/* the contact open beside the list, marked in it (NULL: none) */
void       ct_people_set_current (GtkWidget *people, const char *uid);

/* the FAB, a plus on a rounded square in the theme's colours; ct_fab_update after the theme changed */
GtkWidget *ct_fab_new    (void);
void       ct_fab_update (GtkWidget *fab);

G_END_DECLS
