/* ns_ip_pick_v6 on the lists NetworkManager hands a Wi-Fi page: a network with IPv6 only (the case that showed no
 * address at all), global before unique local, never link-local, and lists with nothing to show. Documentation
 * prefixes only. */
#include "../src/ns-ip.h"

static void
test_global_first (void)
{
  const char *addrs[] = { "fe80::1", "fd00::2", "2001:db8::3", "2001:db8::4", NULL };
  g_assert_cmpstr (ns_ip_pick_v6 (addrs), ==, "2001:db8::3");
}

static void
test_unique_local (void)
{
  const char *addrs[] = { "fe80::1", "fd12:3456::2", NULL };
  g_assert_cmpstr (ns_ip_pick_v6 (addrs), ==, "fd12:3456::2");
}

static void
test_nothing (void)
{
  const char *link_local[] = { "fe80::1", NULL };
  const char *none[] = { NULL };
  const char *junk[] = { "", "not an address", "192.0.2.1", "::1", "::", "ff02::1", NULL };
  g_assert_null (ns_ip_pick_v6 (link_local));
  g_assert_null (ns_ip_pick_v6 (none));
  g_assert_null (ns_ip_pick_v6 (NULL));
  g_assert_null (ns_ip_pick_v6 (junk));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ip/global-first", test_global_first);
  g_test_add_func ("/ip/unique-local", test_unique_local);
  g_test_add_func ("/ip/nothing", test_nothing);
  return g_test_run ();
}
