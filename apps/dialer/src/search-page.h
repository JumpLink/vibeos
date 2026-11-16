/* The search results over the tabs.
 *
 * Two modes. Regular search, from the search bar, finds the contacts whose name has a word starting with the
 * query, or whose number has its digits. Dialpad search, from the dialpad's digits, is smart dial: a contact whose
 * name the digits spell on the keypad from the start of a word (across words too, or by initials), or whose number
 * holds them; at most 20, favourites first. Under "All contacts" each result is a number, the part that matched in
 * bold; a tap calls it. Below them, for a query that is a number, the rows that act on it ("Call", "Send SMS").
 *
 * The window says when the page is touched ("touched": the dialpad goes away, or the keyboard), and a call placed
 * from it closes the search ("call-placed"). */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

#include "contacts.h"

G_BEGIN_DECLS

#define SEARCH_TYPE_PAGE (search_page_get_type ())
G_DECLARE_FINAL_TYPE (SearchPage, search_page, SEARCH, PAGE, GtkWidget)

/* @settings: the favourites, which smart dial ranks first */
GtkWidget *search_page_new (Contacts *contacts, GSettings *settings, VibeosSnackbar *snackbar);
/* searches again after 300 ms of no change (an empty query clears the list at once) */
void       search_page_set_query (SearchPage *self, const char *query, gboolean dialpad);

/* Signals: "touched" (), "call-placed" () */

G_END_DECLS
