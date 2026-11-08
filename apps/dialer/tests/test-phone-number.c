/* phone-number against PhoneNumberUtils and CallerInfoUtils: formatting by country, matching, the network's words
 * for a withheld number */
#include "../src/phone-number.h"

#include <libebook-contacts/libebook-contacts.h>

static void
test_format (void)
{
  g_autofree char *national = NULL, *international = NULL, *short_code = NULL, *empty = NULL, *unfit = NULL;

  if (!e_phone_number_is_supported ())
    {
      g_test_skip ("EDS built without libphonenumber");
      return;
    }
  /* in its own country without the country code, from elsewhere with it */
  national = phone_number_format ("+493023575124", "DE");
  g_assert_cmpstr (national, ==, "030 23575124");
  international = phone_number_format ("+493023575124", "GB");
  g_assert_cmpstr (international, ==, "+49 30 23575124");
  /* a number too short for its plan, which libphonenumber would strip to "17012345", stays as it came */
  unfit = phone_number_format ("+4917012345", "DE");
  g_assert_cmpstr (unfit, ==, "+4917012345");
  /* service codes stay as typed */
  short_code = phone_number_format ("*100#", "DE");
  g_assert_cmpstr (short_code, ==, "*100#");
  empty = phone_number_format ("", "DE");
  g_assert_cmpstr (empty, ==, "");
}

static void
test_equal (void)
{
  if (!e_phone_number_is_supported ())
    {
      g_test_skip ("EDS built without libphonenumber");
      return;
    }
  g_assert_true (phone_number_equal ("030 23575124", "+49 30 23575124", "DE"));
  g_assert_true (phone_number_equal ("+49 (30) 2357-5124", "+493023575124", "DE"));
  g_assert_false (phone_number_equal ("030 23575124", "030 23575125", "DE"));
  g_assert_false (phone_number_equal ("", "", "DE"));
}

static void
test_normalize (void)
{
  g_autofree char *a = phone_number_normalize ("+49 (30) 2357-5124");
  g_autofree char *b = phone_number_normalize ("*#06#");
  g_autofree char *c = phone_number_normalize ("0301,234;5+6");

  g_assert_cmpstr (a, ==, "+493023575124");
  g_assert_cmpstr (b, ==, "*#06#");
  /* a + only leads */
  g_assert_cmpstr (c, ==, "0301,234;56");
}

static void
test_canonical (void)
{
  g_autofree char *national = phone_number_canonical ("0170 1234567", "DE");
  g_autofree char *international = phone_number_canonical ("+49 (170) 123-4567", "GB");
  g_autofree char *other = phone_number_canonical ("+1 415 555 2671", "DE");
  g_autofree char *short_code = phone_number_canonical ("22000", "DE");
  g_autofree char *name = phone_number_canonical ("Vodafone ", "DE");

  /* one key for a person however the number is written */
  g_assert_cmpstr (national, ==, "+491701234567");
  g_assert_cmpstr (international, ==, "+491701234567");
  g_assert_cmpstr (other, ==, "+14155552671");
  /* not phone numbers: as they came */
  g_assert_cmpstr (short_code, ==, "22000");
  g_assert_cmpstr (name, ==, "Vodafone");
}

static void
test_presentation (void)
{
  g_assert_cmpint (phone_number_presentation ("", PRESENTATION_ALLOWED), ==, PRESENTATION_UNKNOWN);
  g_assert_cmpint (phone_number_presentation ("PRIVATE", PRESENTATION_ALLOWED), ==, PRESENTATION_RESTRICTED);
  g_assert_cmpint (phone_number_presentation ("unknown", PRESENTATION_ALLOWED), ==, PRESENTATION_UNKNOWN);
  g_assert_cmpint (phone_number_presentation ("+4930", PRESENTATION_ALLOWED), ==, PRESENTATION_ALLOWED);
  g_assert_cmpint (phone_number_presentation ("+4930", PRESENTATION_PAYPHONE), ==, PRESENTATION_PAYPHONE);
  g_assert_null (phone_number_presentation_name (PRESENTATION_ALLOWED));
  g_assert_cmpstr (phone_number_presentation_name (PRESENTATION_RESTRICTED), ==, "Private number");
}

static void
test_mcc (void)
{
  g_assert_cmpstr (phone_number_country_for_mcc (262), ==, "DE");
  g_assert_cmpstr (phone_number_country_for_mcc (234), ==, "GB");
  g_assert_cmpstr (phone_number_country_for_mcc (310), ==, "US");
  g_assert_cmpstr (phone_number_country_for_mcc (202), ==, "GR");
  g_assert_cmpstr (phone_number_country_for_mcc (750), ==, "FK");
  g_assert_null (phone_number_country_for_mcc (999));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/phone-number/format", test_format);
  g_test_add_func ("/phone-number/equal", test_equal);
  g_test_add_func ("/phone-number/normalize", test_normalize);
  g_test_add_func ("/phone-number/canonical", test_canonical);
  g_test_add_func ("/phone-number/presentation", test_presentation);
  g_test_add_func ("/phone-number/mcc", test_mcc);
  return g_test_run ();
}
