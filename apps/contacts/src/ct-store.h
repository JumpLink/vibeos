/* The contacts, from evolution-data-server: every enabled address book is an "account", each vCard a
 * contact. CtStore keeps a live view of all of them and writes through to them.
 *
 * EDS does not join the cards of several accounts into one person, so each vCard is a contact of its own here, and the
 * Link / Unlink / "View linked contacts" features that rest on aggregation are left out (see ct-quick.c, ct-people.c).
 *
 * Labels are vCard CATEGORIES, so they belong to no single account: one label "Family" covers the Family contacts
 * of every address book. A label with no members has nothing in any vCard to keep it, so the names of the labels
 * made here are kept in GSettings ("labels") until they are deleted.
 *
 * "My info" (the profile) is EDS's self contact: the vCard e_book_client_set_self names (in the GSettings key
 * org.gnome.evolution-data-server.addressbook self-contact-uid, which Evolution and GNOME Contacts read too). A new
 * one goes to EDS's own personal address book. The profile is kept out of the list:
 * the self contact is not among the entries, ct_store_get_profile has it, and the screens name it CT_PROFILE_UID. */
#pragma once

#include "ct-contact.h"

#include <gio/gio.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

/* --- a contact as the screens see it */

typedef struct {
  char *uid;                    /* the vCard UID */
  char *book_uid;               /* the address book (ESource UID) it is in */
  EContact *contact;
  char *name[3];                /* the display name by CtNameOrder (index 1 primary, 2 alternative) */
  char *sort_key[3];            /* ct_sort_key of the sort name, by CtNameOrder */
  const char *section[3];       /* the list's section label, by CtNameOrder */
  int section_rank[3];          /* and where that section goes (ct_section_rank) */
  gboolean starred;
  GPtrArray *labels;
  gboolean has_phone;
  GBytes *photo;                /* the PHOTO's bytes, or NULL */
} CtEntry;

CtEntry *ct_entry_new (EContact *contact, const char *book_uid);
CtEntry *ct_entry_ref (CtEntry *entry);
void     ct_entry_unref (CtEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CtEntry, ct_entry_unref)

/* --- an address book */

typedef struct {
  char *uid;
  char *name;                   /* its display name ("Personal" is the phone's own, shown as "Device") */
  gboolean is_local;            /* the built-in "system-address-book" */
  gboolean writable;
} CtBook;

#define CT_TYPE_STORE (ct_store_get_type ())
G_DECLARE_FINAL_TYPE (CtStore, ct_store, CT, STORE, GObject)

/* Signals:
 *   "changed"          a contact was added, changed or removed (coalesced over 100 ms)
 *   "books-changed"    an address book came or went
 *   "ready"            every address book has delivered its contacts once (the list stops saying "Loading")
 *   "profile-changed"  "My info" was set up, changed or removed */

CtStore    *ct_store_new (GSettings *settings);
gboolean    ct_store_is_ready (CtStore *self);

/* every contact (CtEntry, refs owned by the array) in no particular order */
GPtrArray  *ct_store_dup_entries (CtStore *self);
CtEntry    *ct_store_lookup (CtStore *self, const char *uid);   /* borrowed, or NULL */
guint       ct_store_count (CtStore *self);

/* the address books (CtBook, borrowed) and the one new contacts go to (Settings' default account, else EDS's
 * default address book) */
GPtrArray  *ct_store_get_books (CtStore *self);
CtBook     *ct_store_get_book (CtStore *self, const char *uid);
const char *ct_store_default_book (CtStore *self);
/* the name to show for a book: "Device" for the phone's own, else its own name */
const char *ct_book_label (CtBook *book);

typedef void (*CtStoreDone) (const char *uid, const GError *error, gpointer data);

/* adds @contact to @book_uid (new) or writes it over its old self; @done gets its UID */
void        ct_store_save (CtStore *self, const char *book_uid, EContact *contact, gboolean is_new,
                           CtStoreDone done, gpointer data);
/* writes changed contacts (EContact, each with its UID) back, each to the book it is in */
void        ct_store_modify (CtStore *self, GPtrArray *contacts, CtStoreDone done, gpointer data);
void        ct_store_delete (CtStore *self, GPtrArray *uids, CtStoreDone done, gpointer data);
void        ct_store_set_starred (CtStore *self, const char *uid, gboolean starred);
/* adds the cards of a .vcf file to @book_uid; @done gets the number added in its uid ("3") */
void        ct_store_import (CtStore *self, const char *book_uid, GPtrArray *contacts, CtStoreDone done,
                             gpointer data);

/* --- labels */

/* every label: the ones made here and every category a contact has, sorted (strings, owned) */
GPtrArray  *ct_store_dup_labels (CtStore *self);
gboolean    ct_store_has_label (CtStore *self, const char *label);
/* the members of @label (uids, owned) */
GPtrArray  *ct_store_label_members (CtStore *self, const char *label);
void        ct_store_create_label (CtStore *self, const char *label);
void        ct_store_rename_label (CtStore *self, const char *from, const char *to);
/* deletes the label and takes it off its members; their uids are returned so that UNDO can put it back */
GPtrArray  *ct_store_delete_label (CtStore *self, const char *label);
void        ct_store_add_to_label (CtStore *self, GPtrArray *uids, const char *label);
void        ct_store_remove_from_label (CtStore *self, GPtrArray *uids, const char *label);

/* --- "My info" */

EContact   *ct_store_get_profile (CtStore *self);              /* borrowed, NULL when not set up */
/* FALSE when EDS's settings schema is not installed: then there is no self contact to have */
gboolean    ct_store_can_have_profile (CtStore *self);
/* writes @profile over the self contact, or adds it and makes it the self contact; @done gets CT_PROFILE_UID */
void        ct_store_save_profile (CtStore *self, EContact *profile, CtStoreDone done, gpointer data);
/* what the screens call the profile (its real UID is EDS's) */
#define CT_PROFILE_UID "vibeos-contacts-profile"

G_END_DECLS
