/* The home page's other tabs.
 *
 * Highlights: the favourites as a grid of pictures with "Add", the birthdays coming up, and the contacts opened
 * lately. Organize: the address books with how many contacts each has, Import and Export, and the labels. Each has
 * the search pill at its top, which opens the Contacts tab's search. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *ct_highlights_new (GtkWidget *window);
GtkWidget *ct_organize_new   (GtkWidget *window);

/* the contact @uid goes to the front of the recently opened */
void       ct_recents_add    (GSettings *settings, const char *uid);

G_END_DECLS
