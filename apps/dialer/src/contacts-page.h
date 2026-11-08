/* The Contacts tab (dialer/contactsfragment: ContactsFragment with Header.ADD_CONTACT; spec §7).
 *
 * "Create new contact" on top, then every contact by name, each a 56 dp row with its 40 dp picture. The first row of
 * each letter has the letter beside it, in the accent colour; while the rows of one letter scroll past, that letter
 * stays pinned at the top (the anchored header) until the next one pushes in.
 *
 * Android lists contacts without a number too; this phone's contacts (contacts.h) are the ones with a number, which
 * are all the Phone can do anything with. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

#include "contacts.h"

G_BEGIN_DECLS

#define CONTACTS_TYPE_PAGE (contacts_page_get_type ())
G_DECLARE_FINAL_TYPE (ContactsPage, contacts_page, CONTACTS, PAGE, GtkWidget)

/* @snackbar: where "No contacts app available" shows */
GtkWidget *contacts_page_new (Contacts *contacts, NeoSnackbar *snackbar);
/* the same list as a picker (Favorites' "Add"): no "Create new contact", and a tap emits "picked" (s uid) */
GtkWidget *contacts_page_new_picker (Contacts *contacts, NeoSnackbar *snackbar);
/* the top of the list (the tab is shown afresh) */
void       contacts_page_reset (ContactsPage *self);

G_END_DECLS
