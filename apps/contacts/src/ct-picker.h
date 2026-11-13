/* Picking contacts from the list: the label's "Add contact", which
 * returns one contact on a tap or several through "Select contacts", and another app's "pick a contact".
 *
 * It is the list with its toolbar ("Select" and the back arrow, search, the ⋮ menu), without the favourites, the
 * drawer or the FAB. */
#pragma once

#include "ct-contact.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @uids: the uids picked (strings); called once, after the picker has gone. Not called when it is cancelled. */
typedef void (*CtPicked) (GPtrArray *uids, gpointer data);

/* @exclude: uids not to offer (a label's members), or NULL; @multiple: whether "Select contacts" is offered */
GtkWidget *ct_picker_new (GtkWidget *window, const char *title, GPtrArray *exclude, gboolean multiple,
                          CtPicked picked, gpointer data);

/* insert-or-edit's first row, "Create new contact": a tap closes the
 * picker and runs @create */
typedef void (*CtPickerCreate) (gpointer data);
void       ct_picker_offer_new (GtkWidget *picker, CtPickerCreate create, gpointer data);

/* only the contacts that have a row of @kind (a phone number or an email address), checked again whenever the contacts
 * change */
void       ct_picker_require (GtkWidget *picker, CtKind kind);

G_END_DECLS
