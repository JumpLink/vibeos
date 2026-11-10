/* .vcf files (ct-vcard.h). */
#include "ct-vcard.h"

#include <string.h>

/* Splits the text at its BEGIN:VCARD / END:VCARD lines. A card inside a card (vCard 2.1 lets an AGENT property hold
 * one) is left out of its parent: EVCard would take its END:VCARD for the parent's and lose the rest of the parent,
 * and Android's importer ignores AGENT too. */
GPtrArray *
ct_vcard_parse (const char *text, gssize length)
{
  GPtrArray *contacts = g_ptr_array_new_with_free_func (g_object_unref);
  g_autofree char *copy = g_strndup (text, length < 0 ? strlen (text) : (gsize) length);
  g_autoptr(GString) card = g_string_new (NULL);
  int depth = 0;

  for (char *line = copy, *next; line && *line; line = next)
    {
      char *end = strchr (line, '\n');
      gsize line_length = end ? (gsize) (end - line) + 1 : strlen (line);
      g_autofree char *stripped = g_strstrip (g_strndup (line, line_length));

      next = end ? end + 1 : NULL;
      if (g_ascii_strcasecmp (stripped, "BEGIN:VCARD") == 0)
        {
          if (depth++ == 0)
            g_string_truncate (card, 0);
          else
            continue;
        }
      else if (g_ascii_strcasecmp (stripped, "END:VCARD") == 0 && depth > 0)
        {
          if (--depth > 0)
            continue;
          g_string_append_len (card, line, line_length);
          {
            EContact *contact = e_contact_new_from_vcard (card->str);

            if (contact)
              g_ptr_array_add (contacts, contact);
          }
          continue;
        }
      if (depth == 1)
        g_string_append_len (card, line, line_length);
    }
  return contacts;
}

static void
rename_property (EVCard *vcard, const char *from, const char *to, gboolean keep_old)
{
  g_autoptr(GPtrArray) found = g_ptr_array_new ();

  for (GList *l = e_vcard_get_attributes (vcard); l; l = l->next)
    if (g_ascii_strcasecmp (e_vcard_attribute_get_name (l->data), from) == 0)
      g_ptr_array_add (found, l->data);
  for (guint i = 0; i < found->len; i++)
    {
      EVCardAttribute *old = found->pdata[i];
      EVCardAttribute *renamed = e_vcard_attribute_new (e_vcard_attribute_get_group (old), to);

      for (GList *p = e_vcard_attribute_get_params (old); p; p = p->next)
        e_vcard_attribute_add_param (renamed, e_vcard_attribute_param_copy (p->data));
      for (GList *v = e_vcard_attribute_get_values (old); v; v = v->next)
        e_vcard_attribute_add_value (renamed, v->data);
      if (!keep_old)
        e_vcard_remove_attribute (vcard, old);
      e_vcard_append_attribute (vcard, renamed);
    }
}

EContact *
ct_vcard_for_import (EContact *contact)
{
  EContact *copy = e_contact_duplicate (contact);
  EVCard *vcard = E_VCARD (copy);

  e_vcard_remove_attributes (vcard, NULL, EVC_UID);
  e_vcard_remove_attributes (vcard, NULL, EVC_REV);
  /* which book the card came from, written by EDS on export: the book it goes into says that itself */
  e_vcard_remove_attributes (vcard, NULL, EVC_X_BOOK_UID);
  if (!e_vcard_get_attribute (vcard, EVC_X_SKYPE))
    rename_property (vcard, "X-SKYPE-USERNAME", EVC_X_SKYPE, FALSE);
  else
    e_vcard_remove_attributes (vcard, NULL, "X-SKYPE-USERNAME");
  return copy;
}

char *
ct_vcard_export (GPtrArray *contacts)
{
  GString *out = g_string_new (NULL);

  for (guint i = 0; contacts && i < contacts->len; i++)
    {
      g_autoptr(EContact) copy = e_contact_duplicate (contacts->pdata[i]);
      g_autofree char *text = NULL;

      /* Android reads Skype only as X-SKYPE-USERNAME; EDS reads X-SKYPE: both are written */
      if (!e_vcard_get_attribute (E_VCARD (copy), "X-SKYPE-USERNAME"))
        rename_property (E_VCARD (copy), EVC_X_SKYPE, "X-SKYPE-USERNAME", TRUE);
      e_vcard_remove_attributes (E_VCARD (copy), NULL, EVC_X_BOOK_UID);
      text = e_vcard_to_string (E_VCARD (copy), EVC_FORMAT_VCARD_30);
      g_string_append (out, text);
      /* EVCard ends a card without a line break: the next BEGIN must start a line */
      if (!g_str_has_suffix (text, "\r\n"))
        g_string_append (out, "\r\n");
    }
  return g_string_free (out, FALSE);
}

char *
ct_vcard_file_name (const char *display_name)
{
  g_autofree char *base = g_strdup (display_name && *display_name ? display_name : "contacts");

  g_strdelimit (base, "/\\:*?\"<>|\n\r\t", '_');
  return g_strdup_printf ("%s.vcf", g_strstrip (base));
}
