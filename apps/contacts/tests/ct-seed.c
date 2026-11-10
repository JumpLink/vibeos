/* ct-seed: fills the test rig's private address book with made-up people, so that every screen has something to
 * show: favourites, every section letter, Greek and Cyrillic names, labels, a contact with every kind of field, a
 * photo, a legacy IM and SIP address, a birthday without a year.
 *
 * It only ever writes to the address book of the evolution-data-server on its own session bus, and it refuses to
 * run unless tests/try-contacts.sh started it (CT_RIG_DIR set, XDG_DATA_HOME inside it) and that book is empty:
 * it cannot fill a real address book by mistake. The numbers are in Berlin's 030 23125 range, which the Federal
 * Network Agency keeps for fiction; the addresses are example.com / example.org. */
#include "ct-contact.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libebook/libebook.h>
#include <locale.h>
#include <string.h>

typedef struct {
  const char *given, *family;
  const char *phones;           /* "type:number;type:number" (CtType numbers: 2 mobile, 1 home, 3 work) */
  const char *email;
  gboolean starred;
  const char *labels;           /* "Family,Friends" */
} Person;

static const Person people[] = {
  { "Anna", "Albrecht", "2:+49 30 23125 101", "anna@example.com", TRUE, "Family" },
  { "Ben", "Bauer", "2:+49 30 23125 102;3:+49 30 23125 202", NULL, FALSE, "Friends" },
  { "Clara", "Conrad", "2:+49 30 23125 103", "clara@example.org", TRUE, NULL },
  { "David", "Dietrich", NULL, "david@example.com", FALSE, "Work" },
  { "Emma", "Engel", "1:+49 30 23125 105", NULL, FALSE, "Family,Friends" },
  { "Felix", "Fischer", "2:+49 30 23125 106", NULL, FALSE, NULL },
  { "Greta", "Graf", "2:+49 30 23125 107", "greta@example.org", FALSE, NULL },
  { "Hannah", "Hoffmann", "2:+49 30 23125 108", NULL, TRUE, "Friends" },
  { "Ida", "Imhof", "3:+49 30 23125 109", NULL, FALSE, NULL },
  { "Jonas", "Jäger", "2:+49 30 23125 110", "jonas@example.com", FALSE, "Work" },
  { "Karl", "König", "2:+49 30 23125 111", NULL, FALSE, NULL },
  { "Lena", "Lange", "2:+49 30 23125 112", NULL, FALSE, NULL },
  { "Max", "Maier", "2:+49 30 23125 113;1:+49 30 23125 213", NULL, FALSE, "Family" },
  { "Nora", "Neumann", "2:+49 30 23125 114", NULL, FALSE, NULL },
  { "Otto", "Olbrich", NULL, "otto@example.org", FALSE, NULL },
  { "Paula", "Peters", "2:+49 30 23125 116", NULL, FALSE, NULL },
  { "Quentin", "Quast", "2:+49 30 23125 117", NULL, FALSE, NULL },
  { "Rosa", "Richter", "2:+49 30 23125 118", NULL, FALSE, NULL },
  { "Simon", "Schmidt", "2:+49 30 23125 119", "simon@example.com", FALSE, "Work" },
  { "Tara", "Thiel", "2:+49 30 23125 120", NULL, FALSE, NULL },
  { "Ulrich", "Ulmer", "1:+49 30 23125 121", NULL, FALSE, NULL },
  { "Vera", "Vogel", "2:+49 30 23125 122", NULL, FALSE, NULL },
  { "Walter", "Weber", "2:+49 30 23125 123", NULL, FALSE, NULL },
  { "Xenia", "Xander", "2:+49 30 23125 124", NULL, FALSE, NULL },
  { "Yusuf", "Yilmaz", "2:+49 30 23125 125", NULL, FALSE, NULL },
  { "Zoe", "Zimmermann", "2:+49 30 23125 126", NULL, FALSE, NULL },
  { "Alexandra", "Ahrens", "2:+49 30 23125 127", NULL, FALSE, NULL },
  { "Bruno", "Brandt", "2:+49 30 23125 128", NULL, FALSE, NULL },
  { "Martha", "Möller", "2:+49 30 23125 129", NULL, FALSE, NULL },
  { "Sophie", "Sommer", "2:+49 30 23125 130", NULL, FALSE, NULL },
  { "Νίκος", "Παπαδόπουλος", "2:+49 30 23125 131", NULL, FALSE, NULL },
  { "Ольга", "Иванова", "2:+49 30 23125 132", NULL, FALSE, NULL },
  { "", "", "2:+49 30 23125 199", NULL, FALSE, NULL },      /* no name: shows its number, under "#" */
  { "4 Seasons", "", "3:+49 30 23125 133", NULL, FALSE, NULL },
};

static void
add_items (EContact *contact, CtKind kind, const char *spec)
{
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);
  g_auto(GStrv) parts = spec ? g_strsplit (spec, ";", -1) : NULL;

  for (guint i = 0; parts && parts[i]; i++)
    {
      /* "2:+49 …" has a type before its colon; "https://…" does not */
      const char *colon = strchr (parts[i], ':');

      if (colon && colon > parts[i] && strspn (parts[i], "0123456789") == (size_t) (colon - parts[i]))
        g_ptr_array_add (items, ct_item_new (kind, atoi (parts[i]), colon + 1));
      else
        g_ptr_array_add (items, ct_item_new (kind, ct_kind_default_type (kind), parts[i]));
    }
  if (items->len)
    ct_contact_set_items (contact, kind, items);
}

static void
set_name (EContact *contact, const char *given, const char *family)
{
  g_autoptr(CtName) name = ct_name_new ();

  g_free (name->given);
  g_free (name->family);
  name->given = g_strdup (given);
  name->family = g_strdup (family);
  ct_contact_set_name (contact, name);
  if (*given || *family)
    {
      g_autofree char *full = g_strstrip (g_strconcat (given, " ", family, NULL));

      e_contact_set (contact, E_CONTACT_FULL_NAME, full);
    }
}

/* a made-up "photo": a diagonal two-colour gradient with a lighter disc, PNG */
static void
set_photo (EContact *contact, guint32 from, guint32 to)
{
  g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 256, 256);
  guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);
  int stride = gdk_pixbuf_get_rowstride (pixbuf);
  g_autofree char *png = NULL;
  gsize length = 0;
  EContactPhoto photo = { .type = E_CONTACT_PHOTO_TYPE_INLINED };

  for (int y = 0; y < 256; y++)
    for (int x = 0; x < 256; x++)
      {
        double t = (x + y) / 510.0;
        double dx = x - 128, dy = y - 110;
        double light = dx * dx + dy * dy < 60 * 60 ? 1.35 : 1.0;
        guchar *p = pixels + y * stride + x * 3;

        for (int c = 0; c < 3; c++)
          {
            int a = (from >> (16 - 8 * c)) & 0xff, b = (to >> (16 - 8 * c)) & 0xff;

            p[c] = (guchar) MIN (255, (a + (b - a) * t) * light);
          }
      }
  gdk_pixbuf_save_to_buffer (pixbuf, &png, &length, "png", NULL, NULL);
  photo.data.inlined.mime_type = (char *) "image/png";
  photo.data.inlined.data = (guchar *) png;
  photo.data.inlined.length = length;
  e_contact_set (contact, E_CONTACT_PHOTO, &photo);
}

/* the contact with everything: what QuickContact's cards and every editor section show */
static EContact *
everything (void)
{
  EContact *contact = e_contact_new ();
  g_autoptr(CtName) name = ct_name_new ();
  g_autoptr(GPtrArray) org = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);
  CtItem *company = ct_item_new (CT_KIND_ORGANIZATION, 0, "Example GmbH");
  EVCardAttribute *jabber = e_vcard_attribute_new (NULL, EVC_X_JABBER);
  EVCardAttribute *sip = e_vcard_attribute_new (NULL, EVC_X_SIP);

  g_free (name->prefix);
  g_free (name->given);
  g_free (name->middle);
  g_free (name->family);
  g_free (name->phonetic_given);
  g_free (name->phonetic_family);
  name->prefix = g_strdup ("Dr.");
  name->given = g_strdup ("Lukas");
  name->middle = g_strdup ("Maria");
  name->family = g_strdup ("Lindner");
  name->phonetic_given = g_strdup ("Lukas");
  name->phonetic_family = g_strdup ("Lindner");
  ct_contact_set_name (contact, name);
  e_contact_set (contact, E_CONTACT_FULL_NAME, "Dr. Lukas Maria Lindner");
  add_items (contact, CT_KIND_PHONE, "2:+49 30 23125 140;1:+49 30 23125 141;3:+49 30 23125 142");
  add_items (contact, CT_KIND_EMAIL, "1:lukas@example.com;2:l.lindner@example.org");
  add_items (contact, CT_KIND_ADDRESS, "1:Beispielstraße 1\n10115 Berlin\nGermany");
  add_items (contact, CT_KIND_NICKNAME, "Luke");
  add_items (contact, CT_KIND_WEBSITE, "https://example.com/lukas");
  add_items (contact, CT_KIND_EVENT, "3:1990-03-15;1:2015-06-20");
  add_items (contact, CT_KIND_RELATION, "14:Anna Albrecht");
  add_items (contact, CT_KIND_NOTE, "Met at the example conference.\nLikes tea.");
  company->value2 = g_strdup ("Engineer");
  company->value3 = g_strdup ("Research");
  g_ptr_array_add (org, company);
  ct_contact_set_items (contact, CT_KIND_ORGANIZATION, org);
  /* the legacy kinds the editor shows read-only */
  e_vcard_attribute_add_param_with_value (jabber, e_vcard_attribute_param_new (EVC_TYPE), "HOME");
  e_vcard_append_attribute_with_value (E_VCARD (contact), jabber, "lukas@jabber.example.org");
  e_vcard_append_attribute_with_value (E_VCARD (contact), sip, "lukas@sip.example.org");
  ct_contact_set_starred (contact, TRUE);
  ct_contact_add_label (contact, "Work");
  set_photo (contact, 0x5a7d9a, 0xc9a66b);
  return contact;
}

int
main (int argc, char **argv)
{
  const char *rig = g_getenv ("CT_RIG_DIR");
  g_autoptr(GError) error = NULL;
  g_autoptr(ESourceRegistry) registry = NULL;
  g_autoptr(ESource) source = NULL;
  g_autoptr(EClient) client = NULL;
  GSList *uids = NULL, *contacts = NULL;

  setlocale (LC_ALL, "");
  /* the rig's private EDS, nothing else */
  if (!rig || !*rig || !g_str_has_prefix (g_get_user_data_dir (), rig) || !g_getenv ("DBUS_SESSION_BUS_ADDRESS")
      || g_str_has_prefix (g_getenv ("DBUS_SESSION_BUS_ADDRESS"), "unix:path=/run/user/"))
    {
      g_printerr ("ct-seed: only inside tests/try-contacts.sh (its private bus, EDS and XDG dirs)\n");
      return 2;
    }
  registry = e_source_registry_new_sync (NULL, &error);
  if (registry)
    source = e_source_registry_ref_builtin_address_book (registry);
  if (source)
    client = e_book_client_connect_sync (source, 30, NULL, &error);
  if (!client)
    {
      g_printerr ("ct-seed: no address book: %s\n", error ? error->message : "none");
      return 1;
    }
  if (!e_book_client_get_contacts_uids_sync (E_BOOK_CLIENT (client), "", &uids, NULL, &error))
    {
      g_printerr ("ct-seed: %s\n", error->message);
      return 1;
    }
  if (uids)
    {
      g_printerr ("ct-seed: the address book has %u contacts already; it fills only an empty one\n",
                  g_slist_length (uids));
      g_slist_free_full (uids, g_free);
      return 1;
    }
  for (guint i = 0; i < G_N_ELEMENTS (people); i++)
    {
      const Person *person = &people[i];
      EContact *contact = e_contact_new ();

      set_name (contact, person->given, person->family);
      add_items (contact, CT_KIND_PHONE, person->phones);
      add_items (contact, CT_KIND_EMAIL, person->email);
      if (person->starred)
        ct_contact_set_starred (contact, TRUE);
      if (person->labels)
        {
          g_auto(GStrv) labels = g_strsplit (person->labels, ",", -1);

          for (guint l = 0; labels[l]; l++)
            ct_contact_add_label (contact, labels[l]);
        }
      if (i == 2)
        set_photo (contact, 0x33666d, 0xe0c090);
      if (!*person->given && !*person->family)
        e_contact_set (contact, E_CONTACT_FULL_NAME, person->phones ? strchr (person->phones, ':') + 1 : "");
      contacts = g_slist_prepend (contacts, contact);
    }
  contacts = g_slist_prepend (contacts, everything ());
  {
    /* a birthday without a year */
    EContact *contact = e_contact_new ();

    set_name (contact, "Mia", "Martens");
    add_items (contact, CT_KIND_PHONE, "2:+49 30 23125 150");
    add_items (contact, CT_KIND_EVENT, "3:--07-04");
    contacts = g_slist_prepend (contacts, contact);
  }
  if (!e_book_client_add_contacts_sync (E_BOOK_CLIENT (client), contacts, E_BOOK_OPERATION_FLAG_NONE, NULL, NULL,
                                        &error))
    {
      g_printerr ("ct-seed: %s\n", error->message);
      return 1;
    }
  g_print ("ct-seed: %u made-up contacts added to %s\n", g_slist_length (contacts), e_source_get_uid (source));
  g_slist_free_full (contacts, g_object_unref);
  return 0;
}
