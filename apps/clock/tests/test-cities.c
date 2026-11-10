/* cities against DeskClock's CityDAO, City and Utils.getGMTHourOffset, on the system's tzdata */
#include "../src/cities.h"

/* 2026-07-01 12:00 UTC: summer in the north, so a zone's DST shows */
#define SUMMER 1782907200
/* 2026-01-15 12:00 UTC */
#define WINTER 1768478400

static City *
find_by_name (const char *name)
{
  GPtrArray *all = cities_get_all ();

  for (guint i = 0; i < all->len; i++)
    {
      City *city = all->pdata[i];

      if (g_str_equal (city->data->name, name))
        return city;
    }
  return NULL;
}

static void
test_list (void)
{
  GPtrArray *all = cities_get_all ();

  /* DeskClock's 340, less any zone this system's tzdata lacks (none on Debian) */
  g_assert_cmpuint (all->len, >, 330);
  g_assert_nonnull (cities_find ("C164"));
  g_assert_cmpstr (cities_find ("C164")->data->name, ==, "Berlin");
  g_assert_null (cities_find ("C99999"));
}

static void
test_zones (void)
{
  g_autoptr(GTimeZone) calcutta = clock_zone_new ("Asia/Calcutta");
  g_autoptr(GTimeZone) berlin = clock_zone_new ("Europe/Berlin");
  g_autoptr(GTimeZone) st_johns = clock_zone_new ("America/St_Johns");
  g_autoptr(GTimeZone) honolulu = clock_zone_new ("Pacific/Honolulu");
  g_autofree char *india = NULL, *berlin_summer = NULL, *newfoundland = NULL, *hawaii = NULL;

  /* an old name, followed through tzdata's link */
  g_assert_nonnull (calcutta);
  g_assert_cmpint (clock_zone_offset (calcutta, SUMMER), ==, 19800);
  g_assert_null (clock_zone_new ("Nowhere/Atlantis"));

  /* the offset now includes DST; the GMT label is the standard one, hours truncated towards zero */
  g_assert_cmpint (clock_zone_offset (berlin, SUMMER), ==, 7200);
  g_assert_cmpint (clock_zone_offset (berlin, WINTER), ==, 3600);
  india = clock_zone_gmt_label (calcutta, SUMMER);
  berlin_summer = clock_zone_gmt_label (berlin, SUMMER);
  newfoundland = clock_zone_gmt_label (st_johns, WINTER);
  hawaii = clock_zone_gmt_label (honolulu, SUMMER);
  g_assert_cmpstr (india, ==, "GMT +5:30");
  g_assert_cmpstr (berlin_summer, ==, "GMT +1:00");
  g_assert_cmpstr (newfoundland, ==, "GMT -3:30");
  g_assert_cmpstr (hawaii, ==, "GMT -10:00");
}

static void
test_search (void)
{
  g_autofree char *key = cities_search_key ("st. l");
  g_autofree char *apostrophe = cities_search_key ("St. John's");

  /* upper-cased, without the characters from ' ' to '.' and without "'" */
  g_assert_cmpstr (key, ==, "STL");
  g_assert_cmpstr (apostrophe, ==, "STJOHNS");
  /* a prefix of the name, not a substring */
  g_assert_true (cities_matches (find_by_name ("St. Lucia"), key));
  g_assert_false (cities_matches (find_by_name ("Berlin"), key));
  g_assert_true (cities_matches (find_by_name ("St. Johns"), apostrophe));
  g_assert_false (cities_matches (find_by_name ("Berlin"), "ERLIN"));
}

static void
test_order (void)
{
  const char *const ids[] = { "C164", "C209", "C146", "C29", NULL };     /* Berlin, Honolulu, Adelaide, Anchorage */
  g_autoptr(GPtrArray) selected = cities_get_selected (ids);
  g_autoptr(GPtrArray) by_name = g_ptr_array_new ();

  /* by the offset now: Honolulu (-10), Anchorage (-9 or -8), Berlin, Adelaide */
  g_assert_cmpuint (selected->len, ==, 4);
  g_assert_cmpstr (((City *) selected->pdata[0])->data->name, ==, "Honolulu");
  g_assert_cmpstr (((City *) selected->pdata[1])->data->name, ==, "Anchorage");
  g_assert_cmpstr (((City *) selected->pdata[2])->data->name, ==, "Berlin");
  g_assert_cmpstr (((City *) selected->pdata[3])->data->name, ==, "Adelaide");

  /* by name: the index letter first ("Adelaide" and "Anchorage" under A), then the name */
  for (guint i = 0; i < selected->len; i++)
    g_ptr_array_add (by_name, selected->pdata[i]);
  cities_sort (by_name, CITY_SORT_NAME);
  g_assert_cmpstr (((City *) by_name->pdata[0])->data->name, ==, "Adelaide");
  g_assert_cmpstr (((City *) by_name->pdata[1])->data->name, ==, "Anchorage");
  g_assert_cmpstr (((City *) by_name->pdata[2])->data->name, ==, "Berlin");
  g_assert_cmpstr (((City *) by_name->pdata[3])->data->name, ==, "Honolulu");
}

static void
test_home_zones (void)
{
  /* timezone_values: 85 choices; Amsterdam is one, Berlin is not (it is under "Amsterdam, Berlin") */
  g_assert_cmpuint (home_zones_count (), ==, 85);
  g_assert_true (home_zones_contains ("Europe/Amsterdam"));
  g_assert_false (home_zones_contains ("Europe/Berlin"));
  g_assert_cmpstr (home_zones_get (0)->zone, !=, NULL);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cities/list", test_list);
  g_test_add_func ("/cities/zones", test_zones);
  g_test_add_func ("/cities/search", test_search);
  g_test_add_func ("/cities/order", test_order);
  g_test_add_func ("/cities/home-zones", test_home_zones);
  return g_test_run ();
}
