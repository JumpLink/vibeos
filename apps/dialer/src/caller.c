/* The caller: see caller.h. */
#include "caller.h"
#include "phone-number.h"

Caller *
caller_new (Contacts *contacts, const char *number, Presentation presentation, const char *country)
{
  Caller *caller = g_new0 (Caller, 1);
  const Contact *contact = contacts && number && *number ? contacts_lookup (contacts, number, country) : NULL;
  const char *withheld = phone_number_presentation_name (presentation);

  caller->number = number && *number && !withheld ? phone_number_format (number, country) : g_strdup ("");
  if (contact && contact->name && *contact->name)
    {
      caller->is_contact = TRUE;
      caller->name = g_strdup (contact->name);
      caller->label = g_strdup (contact_number_label (contact, number, country));
      caller->identifier = g_strdup (contact->uid);
      caller->photo = contact->photo ? g_bytes_ref (contact->photo) : NULL;
    }
  else
    {
      /* no name, no number: what the network said; a number alone: it is the name */
      caller->name = g_strdup (withheld ? withheld : *caller->number ? caller->number : phone_number_presentation_name (PRESENTATION_UNKNOWN));
      caller->name_is_number = !withheld && *caller->number;
      caller->identifier = g_strdup (caller->name);
    }
  return caller;
}

void
caller_free (Caller *caller)
{
  if (!caller)
    return;
  g_free (caller->name);
  g_free (caller->number);
  g_free (caller->label);
  g_free (caller->identifier);
  g_clear_pointer (&caller->photo, g_bytes_unref);
  g_free (caller);
}

char *
caller_label_and_number (const Caller *caller)
{
  if (caller->name_is_number || !*caller->number)
    return NULL;
  if (!caller->label)
    return g_strdup (caller->number);
  return g_strconcat (caller->label, " ", caller->number, NULL);
}
