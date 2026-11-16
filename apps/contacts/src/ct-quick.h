/* QuickContact: a contact's card over the list. The square header with the photo or the tinted
 * silhouette and the name, shrinking to a bar as the cards scroll up under it; the star, edit and ⋮ in its
 * toolbar; a card of the ways to reach the person (phone numbers with their SMS button, SIP, email, addresses) and
 * an "About" card (nickname, website, organization, events, relations, IM, note). A tap on an entry calls, texts,
 * mails or opens it in the app that takes it; a long press copies it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the card of the contact with vCard UID @uid (or CT_PROFILE_UID for "My info"); NULL, after a toast, when there
 * is no such contact */
GtkWidget *ct_quick_new (GtkWidget *window, const char *uid);
/* the same card filling a detail pane beside the list: it does not cover anything or slide in, and Back closes the
 * pane */
GtkWidget *ct_quick_new_embedded (GtkWidget *window, const char *uid);

G_END_DECLS
