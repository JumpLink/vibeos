/* Who a conversation is with (msg-people.h). */
#include "msg-people.h"
#include "phone-number.h"

MsgPerson *
msg_person_new (Contacts *contacts, const char *destination, const char *country)
{
  MsgPerson *person = g_new0 (MsgPerson, 1);
  const Contact *contact = contacts ? contacts_lookup (contacts, destination, country) : NULL;

  person->destination = g_strdup (destination);
  if (contact)
    {
      person->is_contact = TRUE;
      person->name = g_strdup (contact->name_primary ? contact->name_primary : contact->name);
      person->identifier = g_strdup (contact->uid);
      person->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
    }
  if (!person->name || !*person->name)
    {
      g_free (person->name);
      person->name = phone_number_format (destination, country);
    }
  if (!person->identifier)
    person->identifier = g_strdup (person->name);
  return person;
}

void
msg_person_free (MsgPerson *person)
{
  if (!person)
    return;
  g_free (person->destination);
  g_free (person->name);
  g_free (person->identifier);
  g_clear_pointer (&person->photo, g_bytes_unref);
  g_free (person);
}

GPtrArray *
msg_people_new (Contacts *contacts, const char * const *destinations, const char *country)
{
  GPtrArray *people = g_ptr_array_new_with_free_func ((GDestroyNotify) msg_person_free);

  for (guint i = 0; destinations && destinations[i]; i++)
    g_ptr_array_add (people, msg_person_new (contacts, destinations[i], country));
  return people;
}

char *
msg_people_name (GPtrArray *people)
{
  GString *name = g_string_new (NULL);

  for (guint i = 0; i < people->len; i++)
    {
      if (name->len)
        g_string_append (name, ", ");
      g_string_append (name, ((MsgPerson *) people->pdata[i])->name);
    }
  return g_string_free (name, FALSE);
}
