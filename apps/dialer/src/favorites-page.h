/* The Favorites tab: the starred contacts.
 *
 * A "Favorites" header with "Add", then the favourites three to a row, each a large picture with the name and the
 * kind of number under it. A tap calls the contact's number, or asks which one when it has several and none has
 * been chosen to remember; a long press opens its menu (call, message, remove, contact info).
 *
 * The star is the Phone's own (the "favorites" setting): Evolution's address books have none. There are
 * no "Frequents" rows below the favourites: nothing here counts how often a contact is called. */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

#include "contacts.h"

G_BEGIN_DECLS

#define FAVORITES_TYPE_PAGE (favorites_page_get_type ())
G_DECLARE_FINAL_TYPE (FavoritesPage, favorites_page, FAVORITES, PAGE, GtkWidget)

GtkWidget *favorites_page_new (Contacts *contacts, GSettings *settings, VibeosSnackbar *snackbar);

G_END_DECLS
