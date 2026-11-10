/* ct-vcard: reading .vcf files (Android's 2.1 export, 3.0, several cards, a nested card) and the round trip
 * through export and import (spec §5.4, §10.8). */
#include "ct-contact.h"
#include "ct-vcard.h"

#include <locale.h>
#include <string.h>

/* what Android 16's Contacts exports for one contact: vCard 2.1, QUOTED-PRINTABLE for the non-ASCII name, a
 * year-less birthday and a relation in X-ANDROID-CUSTOM */
static const char android_21[] =
  "BEGIN:VCARD\r\n"
  "VERSION:2.1\r\n"
  "N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:M=C3=BCller;J=C3=BCrgen;;;\r\n"
  "FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:J=C3=BCrgen M=C3=BCller\r\n"
  "TEL;CELL:+49 170 1234567\r\n"
  "TEL;WORK;PREF:030 123456\r\n"
  "EMAIL;HOME:juergen@example.org\r\n"
  "BDAY:--03-15\r\n"
  "X-ANDROID-CUSTOM:vnd.android.cursor.item/relation;Anna;14;;;;;;;;;;;;;\r\n"
  "X-SKYPE-USERNAME:jmueller\r\n"
  "END:VCARD\r\n";

static const char two_30[] =
  "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:abc-1\r\nN:One;Ann;;;\r\nFN:Ann One\r\nEND:VCARD\r\n"
  "garbage between cards\r\n"
  "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Two;Ben;;;\r\nFN:Ben Two\r\n"
  "AGENT:BEGIN:VCARD\\nFN:Agent\\nEND:VCARD\r\n"
  "END:VCARD\r\n";

static const char nested_21[] =
  "BEGIN:VCARD\nVERSION:2.1\nN:Boss;Big;;;\nAGENT:\nBEGIN:VCARD\nVERSION:2.1\nN:Helper;Hal;;;\nEND:VCARD\n"
  "TEL;WORK:1\nEND:VCARD\n";

static void
test_android_21 (void)
{
  g_autoptr(GPtrArray) cards = ct_vcard_parse (android_21, -1);
  EContact *contact;
  g_autofree char *name = NULL;
  g_autoptr(GPtrArray) phones = NULL, events = NULL, relations = NULL;
  g_autoptr(EContact) imported = NULL;
  g_autofree char *skype = NULL;

  g_assert_cmpuint (cards->len, ==, 1);
  contact = cards->pdata[0];
  name = ct_contact_display_name (contact, CT_ORDER_PRIMARY);
  g_assert_cmpstr (name, ==, "Jürgen Müller");
  phones = ct_contact_get_items (contact, CT_KIND_PHONE);
  g_assert_cmpuint (phones->len, ==, 2);
  g_assert_cmpint (((CtItem *) phones->pdata[0])->type, ==, 2);
  g_assert_cmpint (((CtItem *) phones->pdata[1])->type, ==, 3);
  g_assert_true (((CtItem *) phones->pdata[1])->primary);
  events = ct_contact_get_items (contact, CT_KIND_EVENT);
  g_assert_cmpuint (events->len, ==, 1);
  g_assert_cmpstr (((CtItem *) events->pdata[0])->value, ==, "--03-15");
  relations = ct_contact_get_items (contact, CT_KIND_RELATION);
  g_assert_cmpuint (relations->len, ==, 1);
  g_assert_cmpstr (((CtItem *) relations->pdata[0])->value, ==, "Anna");
  g_assert_cmpint (((CtItem *) relations->pdata[0])->type, ==, 14);

  imported = ct_vcard_for_import (contact);
  skype = e_vcard_util_dup_x_attribute (E_VCARD (imported), "X-SKYPE");
  g_assert_cmpstr (skype, ==, "jmueller");
  g_assert_null (e_vcard_get_attribute (E_VCARD (imported), "X-SKYPE-USERNAME"));
}

static void
test_several (void)
{
  g_autoptr(GPtrArray) cards = ct_vcard_parse (two_30, -1);
  g_autoptr(GPtrArray) nested = ct_vcard_parse (nested_21, -1);
  g_autoptr(EContact) imported = NULL;
  g_autofree char *b = NULL, *boss = NULL;
  g_autoptr(GPtrArray) phones = NULL;

  g_assert_cmpuint (cards->len, ==, 2);
  b = ct_contact_display_name (cards->pdata[1], CT_ORDER_PRIMARY);
  g_assert_cmpstr (b, ==, "Ben Two");
  /* the import copy loses the UID: the address book gives it its own */
  imported = ct_vcard_for_import (cards->pdata[0]);
  g_assert_null (e_vcard_get_attribute (E_VCARD (imported), EVC_UID));

  /* a 2.1 AGENT card inside a card does not end it */
  g_assert_cmpuint (nested->len, ==, 1);
  boss = ct_contact_display_name (nested->pdata[0], CT_ORDER_PRIMARY);
  g_assert_cmpstr (boss, ==, "Big Boss");
  phones = ct_contact_get_items (nested->pdata[0], CT_KIND_PHONE);
  g_assert_cmpuint (phones->len, ==, 1);
}

/* contacts written out and read back have the same names, rows, labels and star */
static void
test_round_trip (void)
{
  g_autoptr(GPtrArray) originals = ct_vcard_parse (android_21, -1);
  g_autoptr(GPtrArray) contacts = g_ptr_array_new_with_free_func (g_object_unref);
  g_autofree char *text = NULL;
  g_autoptr(GPtrArray) back = NULL;

  /* the 2.1 card as it would be in the address book, plus one made here */
  g_ptr_array_add (contacts, ct_vcard_for_import (originals->pdata[0]));
  {
    EContact *made = e_contact_new ();
    g_autoptr(CtName) name = ct_name_new ();
    g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_item_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func (g_free);

    g_free (name->given);
    name->given = g_strdup ("Zoë");
    g_free (name->family);
    name->family = g_strdup ("Ångström; Jr");               /* a ; that must be escaped */
    ct_contact_set_name (made, name);
    g_ptr_array_add (items, ct_item_new (CT_KIND_ADDRESS, 1, "Line one\nLine, two"));
    ct_contact_set_items (made, CT_KIND_ADDRESS, items);
    g_ptr_array_add (labels, g_strdup ("Climbing, Club"));
    ct_contact_set_labels (made, labels);
    ct_contact_set_starred (made, TRUE);
    g_ptr_array_add (contacts, made);
  }
  text = ct_vcard_export (contacts);
  g_assert_true (g_str_has_prefix (text, "BEGIN:VCARD\r\nVERSION:3.0\r\n"));
  /* Android reads Skype only as X-SKYPE-USERNAME */
  g_assert_nonnull (strstr (text, "X-SKYPE-USERNAME:jmueller"));
  back = ct_vcard_parse (text, -1);
  g_assert_cmpuint (back->len, ==, contacts->len);
  for (guint c = 0; c < contacts->len; c++)
    {
      EContact *a = contacts->pdata[c], *b = back->pdata[c];
      g_autoptr(CtName) na = ct_contact_get_name (a), nb = ct_contact_get_name (b);
      g_autoptr(GPtrArray) la = ct_contact_dup_labels (a), lb = ct_contact_dup_labels (b);

      g_assert_true (ct_name_equal (na, nb));
      g_assert_cmpint (ct_contact_is_starred (a), ==, ct_contact_is_starred (b));
      g_assert_cmpuint (la->len, ==, lb->len);
      for (guint i = 0; i < la->len; i++)
        g_assert_cmpstr (la->pdata[i], ==, lb->pdata[i]);
      for (CtKind kind = 0; kind < CT_N_KINDS; kind++)
        {
          g_autoptr(GPtrArray) ia = ct_contact_get_items (a, kind), ib = ct_contact_get_items (b, kind);

          g_assert_cmpuint (ia->len, ==, ib->len);
          for (guint i = 0; i < ia->len; i++)
            g_assert_true (ct_item_equal (ia->pdata[i], ib->pdata[i]));
        }
    }
}

static void
test_file_name (void)
{
  g_autofree char *a = ct_vcard_file_name ("Anna / Bob");
  g_autofree char *b = ct_vcard_file_name (NULL);

  g_assert_cmpstr (a, ==, "Anna _ Bob.vcf");
  g_assert_cmpstr (b, ==, "contacts.vcf");
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/vcard/android-2.1", test_android_21);
  g_test_add_func ("/vcard/several", test_several);
  g_test_add_func ("/vcard/round-trip", test_round_trip);
  g_test_add_func ("/vcard/file-name", test_file_name);
  return g_test_run ();
}
