/* Who is on a call, as the in-call screens show them (CallCardPresenter.getPrimaryInfo, ContactInfoCache): the name,
 * the line under it, and what the avatar is drawn from. */
#pragma once

#include "contacts.h"
#include "call-types.h"

typedef struct
{
  char *name;                   /* a contact's name, "Private number" or "Unknown", else the number as it reads here */
  char *number;                 /* the number as it reads here; "" when withheld */
  char *label;                  /* the kind of the contact's number ("Mobile"), or NULL */
  gboolean name_is_number;      /* not a contact: the name is the number (then it is not shown twice) */
  gboolean is_contact;
  char *identifier;             /* the avatar's colour: the contact's uid, else the name */
  GBytes *photo;                /* the contact's picture, or NULL */
} Caller;

/* @contacts may be NULL (no address book yet): the caller is then the number. @country: the network's */
Caller *caller_new (Contacts *contacts, const char *number, Presentation presentation, const char *country);
void    caller_free (Caller *caller);
/* BottomRow.getLabelForPhoneNumber: "Mobile 030 1234567" for a contact, nothing when the name is the number */
char   *caller_label_and_number (const Caller *caller);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Caller, caller_free)
