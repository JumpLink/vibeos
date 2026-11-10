/* ct-rows: the main list's order and sections, and a search's rows. */
#include "ct-rows.h"

static CtEntry *
entry (const char *uid, const char *name, gboolean starred)
{
  g_autofree char *text = g_strdup_printf ("BEGIN:VCARD\r\nVERSION:3.0\r\nUID:%s\r\nFN:%s\r\n%sEND:VCARD\r\n", uid,
                                           name, starred ? "CATEGORIES:Starred in Android\r\n" : "");
  g_autoptr(EContact) contact = e_contact_new_from_vcard (text);

  return ct_entry_new (contact, "system-address-book");
}

static GPtrArray *
people (void)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) ct_entry_unref);

  g_ptr_array_add (entries, entry ("1", "Bob Baker", FALSE));
  g_ptr_array_add (entries, entry ("2", "Anna Adams", TRUE));
  g_ptr_array_add (entries, entry ("3", "?", FALSE));
  g_ptr_array_add (entries, entry ("4", "Alex Brown", FALSE));
  g_ptr_array_add (entries, entry ("5", "שרה", FALSE));
  g_ptr_array_add (entries, entry ("6", "+49 170 5550101", FALSE));
  g_ptr_array_add (entries, entry ("7", "$ Money", FALSE));
  return entries;
}

static const char *
name_at (CtRows *rows, guint i)
{
  return g_array_index (rows->rows, CtRow, i).name;
}

static void
test_main_list (void)
{
  g_autoptr(GPtrArray) entries = people ();
  g_autoptr(CtRows) rows = ct_rows_new (entries, CT_ORDER_PRIMARY, CT_ORDER_PRIMARY, NULL, TRUE);
  const char *expected[] = { "Anna Adams", "$ Money", "?", "Alex Brown", "Anna Adams", "Bob Baker",
                             "+49 170 5550101", "שרה" };
  /* the favourites, the symbols' "…", A, B, "#", the other scripts' "…" */
  const char *labels[] = { "", "…", "A", "B", "#", "…" };
  const guint firsts[] = { 0, 1, 3, 5, 6, 7 };

  g_assert_cmpuint (rows->rows->len, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++)
    g_assert_cmpstr (name_at (rows, i), ==, expected[i]);
  g_assert_cmpuint (rows->sections->len, ==, G_N_ELEMENTS (labels));
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      CtSection *section = &g_array_index (rows->sections, CtSection, i);

      g_assert_cmpstr (section->label, ==, labels[i]);
      g_assert_cmpuint (section->first_row, ==, firsts[i]);
      g_assert_true (g_array_index (rows->rows, CtRow, firsts[i]).first);
    }
  g_assert_false (g_array_index (rows->rows, CtRow, 2).first);
  g_assert_cmpstr (ct_rows_section_at (rows, 4)->label, ==, "A");
}

static void
test_no_favourites (void)
{
  g_autoptr(GPtrArray) entries = people ();
  g_autoptr(CtRows) rows = ct_rows_new (entries, CT_ORDER_PRIMARY, CT_ORDER_PRIMARY, "", FALSE);

  /* a label's list: no star block, so everyone once */
  g_assert_cmpuint (rows->rows->len, ==, entries->len);
  g_assert_cmpstr (g_array_index (rows->sections, CtSection, 0).label, ==, "…");
}

static void
test_alternative_order (void)
{
  g_autoptr(GPtrArray) entries = people ();
  g_autoptr(CtRows) rows = ct_rows_new (entries, CT_ORDER_ALTERNATIVE, CT_ORDER_ALTERNATIVE, NULL, FALSE);

  /* by family name: Adams, Baker, Brown, shown family name first; a name-only card is split as CP2's NameSplitter
   * splits one, so "$ Money" is Money's */
  g_assert_cmpstr (name_at (rows, 0), ==, "?");
  g_assert_cmpstr (name_at (rows, 1), ==, "Adams, Anna");
  g_assert_cmpstr (name_at (rows, 2), ==, "Baker, Bob");
  g_assert_cmpstr (name_at (rows, 3), ==, "Brown, Alex");
  g_assert_cmpstr (name_at (rows, 4), ==, "Money, $");
  g_assert_cmpstr (ct_rows_section_at (rows, 2)->label, ==, "B");
  g_assert_cmpstr (ct_rows_section_at (rows, 4)->label, ==, "M");
}

static void
test_search (void)
{
  g_autoptr(GPtrArray) entries = people ();
  g_autoptr(CtRows) rows = ct_rows_new (entries, CT_ORDER_PRIMARY, CT_ORDER_PRIMARY, "a", TRUE);

  /* "a" starts a word of Anna Adams and Alex: no favourites block, no sections */
  g_assert_cmpuint (rows->rows->len, ==, 2);
  g_assert_cmpstr (name_at (rows, 0), ==, "Alex Brown");
  g_assert_cmpstr (name_at (rows, 1), ==, "Anna Adams");
  g_assert_cmpuint (rows->sections->len, ==, 0);
  g_assert_null (ct_rows_section_at (rows, 0));
  g_assert_cmpint (g_array_index (rows->rows, CtRow, 0).match.name_start, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/rows/main-list", test_main_list);
  g_test_add_func ("/rows/no-favourites", test_no_favourites);
  g_test_add_func ("/rows/alternative-order", test_alternative_order);
  g_test_add_func ("/rows/search", test_search);
  return g_test_run ();
}
