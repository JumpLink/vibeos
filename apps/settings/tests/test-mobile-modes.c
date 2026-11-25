/* ModemManager's SupportedModes/CurrentModes and the SetCurrentModes argument. The fixtures use MMModemMode's
 * values: CS=1, 2G=2, 3G=4, 4G=8, 5G=16. */
#include "../src/ns-mobile-modes.h"

static NsMobileModes *
parse (const char *text, GVariant *current)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) supported = g_variant_parse (G_VARIANT_TYPE (NS_MOBILE_MODES_TYPE), text, NULL, NULL, &err);
  g_assert_no_error (err);
  g_assert_nonnull (supported);
  NsMobileModes *m = ns_mobile_modes_new (supported, current, &err);
  g_assert_no_error (err);
  g_assert_nonnull (m);
  return m;
}

static void
assert_labels (NsMobileModes *m, const char *const *want)
{
  g_auto (GStrv) labels = ns_mobile_modes_labels (m);
  guint n = 0;
  while (want[n])
    n++;
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr (labels[i], ==, want[i]);
  g_assert_null (labels[n]);
}

static void
test_modes (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 14, uint32 0)");
  NsMobileModes *m = parse ("[(uint32 30, uint32 0), (uint32 2, uint32 0), (uint32 4, uint32 0), "
                            "(uint32 8, uint32 0), (uint32 16, uint32 0), (uint32 6, uint32 0), "
                            "(uint32 14, uint32 0), (uint32 30, uint32 8)]", current);
  const char *labels[] = { "Automatic", "2G only", "3G only", "4G only", "5G only", "2G/3G", "2G/3G/4G",
                           "2G/3G/4G/5G, prefer 4G", NULL };
  assert_labels (m, labels);
  g_assert_cmpuint (ns_mobile_modes_current (m, current), ==, 6);

  NsMobileMode *mode = g_ptr_array_index (m, 1);
  g_assert_cmpuint (mode->allowed, ==, 2);
  g_assert_cmpuint (mode->preferred, ==, 0);
  g_autoptr (GVariant) value = ns_mobile_mode_value (mode);
  g_autofree char *text = g_variant_print (value, FALSE);
  g_assert_cmpstr (text, ==, "(2, 0)");
  ns_mobile_modes_free (m);
}

static void
test_invalid (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 2, uint32 0)");
  NsMobileModes *m = parse ("[(uint32 0, uint32 0), (uint32 2, uint32 4), (uint32 2, uint32 6), "
                            "(uint32 2, uint32 0), (uint32 2, uint32 0)]", current);
  const char *labels[] = { "2G only", NULL };
  assert_labels (m, labels);
  g_assert_cmpuint (ns_mobile_modes_current (m, current), ==, 0);
  ns_mobile_modes_free (m);
}

static void
test_unknown_current (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 8, uint32 0)");
  NsMobileModes *m = parse ("[(uint32 2, uint32 0), (uint32 4, uint32 0)]", current);
  const char *labels[] = { "2G only", "3G only", NULL };
  assert_labels (m, labels);
  g_assert_cmpuint (ns_mobile_modes_current (m, current), ==, 0);
  ns_mobile_modes_free (m);
}

static void
test_preferred_automatic (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 14, uint32 8)");
  NsMobileModes *m = parse ("[(uint32 14, uint32 8), (uint32 2, uint32 0), (uint32 4, uint32 0), "
                            "(uint32 8, uint32 0)]", current);
  const char *labels[] = { "Automatic", "2G only", "3G only", "4G only", NULL };
  assert_labels (m, labels);
  g_assert_cmpuint (ns_mobile_modes_current (m, current), ==, 0);
  ns_mobile_modes_free (m);
}

static void
test_fajita (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 14, uint32 8)");
  NsMobileModes *m = parse ("[(uint32 2, uint32 0), (uint32 4, uint32 0), (uint32 6, uint32 4), "
                            "(uint32 6, uint32 2), (uint32 10, uint32 8), (uint32 10, uint32 2), "
                            "(uint32 12, uint32 8), (uint32 12, uint32 4), (uint32 14, uint32 8)]", current);
  const char *labels[] = { "2G only", "3G only", "2G/3G, prefer 3G", "2G/3G, prefer 2G",
                           "2G/4G, prefer 4G", "2G/4G, prefer 2G", "3G/4G, prefer 4G",
                           "3G/4G, prefer 3G", "Automatic", NULL };
  assert_labels (m, labels);
  g_assert_cmpuint (ns_mobile_modes_current (m, current), ==, 8);
  ns_mobile_modes_free (m);
}

static void
test_any (void)
{
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 4294967295, uint32 0)");
  NsMobileModes *m = parse ("[(uint32 4294967295, uint32 0), (uint32 30, uint32 0)]", current);
  const char *labels[] = { "Automatic", "2G/3G/4G/5G", NULL };
  assert_labels (m, labels);
  ns_mobile_modes_free (m);
}

static void
test_wrong_type (void)
{
  g_autoptr (GVariant) supported = g_variant_new_parsed ("@au []");
  g_autoptr (GVariant) current = g_variant_new_parsed ("(uint32 2, uint32 0)");
  g_autoptr (GError) err = NULL;
  g_assert_null (ns_mobile_modes_new (supported, current, &err));
  g_assert_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mobile-modes/supported", test_modes);
  g_test_add_func ("/mobile-modes/invalid", test_invalid);
  g_test_add_func ("/mobile-modes/unknown-current", test_unknown_current);
  g_test_add_func ("/mobile-modes/preferred-automatic", test_preferred_automatic);
  g_test_add_func ("/mobile-modes/fajita", test_fajita);
  g_test_add_func ("/mobile-modes/any", test_any);
  g_test_add_func ("/mobile-modes/wrong-type", test_wrong_type);
  return g_test_run ();
}
