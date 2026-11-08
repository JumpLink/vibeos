/* Who a conversation is with, as the screens name and picture them (ParticipantData): each destination looked up in
 * the contacts, else shown as its number. */
#pragma once

#include <gio/gio.h>

#include "contacts.h"

typedef struct {
  char *destination;            /* as stored: E.164, or a sender's name ("Vodafone") */
  char *name;                   /* the contact's full name (DISPLAY_NAME), else the number formatted */
  char *identifier;             /* what its avatar's colour comes from: the contact's lookup key, else the name */
  GBytes *photo;                /* the contact's, or NULL */
  gboolean is_contact;
} MsgPerson;

MsgPerson *msg_person_new (Contacts *contacts, const char *destination, const char *country);
void       msg_person_free (MsgPerson *person);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MsgPerson, msg_person_free)

/* the people of @destinations: MsgPerson */
GPtrArray *msg_people_new (Contacts *contacts, const char * const *destinations, const char *country);
/* the names joined, "Anna, Bob" */
char      *msg_people_name (GPtrArray *people);
