/* The world-clock cities: see cities.h. */
#include "cities.h"

#include <stdlib.h>
#include <string.h>

#include "cities-data.h"

/* --- time zones */

/* tzdata's links, "L TARGET NAME" lines in tzdata.zi: the old names ("Asia/Calcutta" → "Asia/Kolkata"). Debian ships
 * them as files only in tzdata-legacy, which the phone does not have, but DeskClock's list (like ICU) still uses
 * them, so we follow them ourselves. Read once. */
static GHashTable *
links (void)
{
  static GHashTable *table;
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;

  if (table)
    return table;
  table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (!g_file_get_contents ("/usr/share/zoneinfo/tzdata.zi", &contents, NULL, NULL))
    return table;
  lines = g_strsplit (contents, "\n", -1);
  for (char **line = lines; *line; line++)
    {
      g_auto(GStrv) parts = NULL;

      if (!g_str_has_prefix (*line, "L "))
        continue;
      parts = g_strsplit (*line, " ", 3);
      if (g_strv_length (parts) == 3)
        g_hash_table_insert (table, g_strdup (parts[2]), g_strdup (parts[1]));
    }
  return table;
}

GTimeZone *
clock_zone_new (const char *id)
{
  GTimeZone *tz;
  const char *target;

  if (!id || !*id)
    return NULL;
  tz = g_time_zone_new_identifier (id);
  if (tz)
    return tz;
  target = g_hash_table_lookup (links (), id);
  return target ? g_time_zone_new_identifier (target) : NULL;
}

int
clock_zone_offset (GTimeZone *tz, gint64 time)
{
  return g_time_zone_get_offset (tz, g_time_zone_find_interval (tz, G_TIME_TYPE_UNIVERSAL, time));
}

/* TimeZone.getRawOffset: the offset without daylight saving time. GLib has no such call, so this looks for an
 * interval without DST now or half a year either side of now (Europe's summer is Australia's winter). */
static int
raw_offset (GTimeZone *tz, gint64 time)
{
  const gint64 half_year = 182 * 24 * 3600;
  const gint64 times[] = { time, time - half_year, time + half_year };

  for (guint i = 0; i < G_N_ELEMENTS (times); i++)
    {
      int interval = g_time_zone_find_interval (tz, G_TIME_TYPE_UNIVERSAL, times[i]);

      if (!g_time_zone_is_dst (tz, interval))
        return g_time_zone_get_offset (tz, interval);
    }
  return clock_zone_offset (tz, time);
}

char *
clock_zone_gmt_label (GTimeZone *tz, gint64 time)
{
  int offset = raw_offset (tz, time);

  /* "GMT %+d:%02d" with the hours truncated towards zero, as Java's division does: -3:30 is "GMT -3:30" */
  return g_strdup_printf ("GMT %+d:%02d", offset / 3600, ABS (offset) % 3600 / 60);
}

/* --- the cities */

/* City.removeSpecialCharacters: the regex [ -.'] is the range from space to '.' (" !\"#$%&'()*+,-.") plus "'" */
static char *
search_key (const char *text)
{
  g_autofree char *upper = g_utf8_strup (text, -1);
  GString *key = g_string_new (NULL);

  for (const char *c = upper; *c; c++)
    if (!(*c >= ' ' && *c <= '.') && *c != '\'')
      g_string_append_c (key, *c);
  return g_string_free (key, FALSE);
}

char *
cities_search_key (const char *query)
{
  return search_key (query);
}

gboolean
cities_matches (const City *city, const char *query)
{
  return g_str_has_prefix (city->search_key, query);
}

GPtrArray *
cities_get_all (void)
{
  static GPtrArray *all;

  if (all)
    return all;
  all = g_ptr_array_new ();
  for (guint i = 0; i < G_N_ELEMENTS (city_data); i++)
    {
      const CityData *data = &city_data[i];
      GTimeZone *tz = clock_zone_new (data->zone);
      City *city;

      /* CityDAO.createCity: a zone the system does not know (Java gives GMT for it) drops the city */
      if (!tz)
        {
          g_debug ("no time zone %s for %s", data->zone, data->name);
          continue;
        }
      city = g_new0 (City, 1);
      city->data = data;
      city->tz = tz;
      city->number = g_ascii_isdigit (data->index[0]) ? atoi (data->index) : -1;
      city->search_key = search_key (data->name);
      g_ptr_array_add (all, city);
    }
  return all;
}

City *
cities_find (const char *id)
{
  GPtrArray *all = cities_get_all ();

  for (guint i = 0; i < all->len; i++)
    {
      City *city = all->pdata[i];

      if (g_str_equal (city->data->id, id))
        return city;
    }
  return NULL;
}

/* --- orders (City's comparators). The offsets are taken at one moment for a whole sort, as the Java one snapshots
 * "now" when it is made, so that a sort running over a DST change still has one order. */

static gint64 sort_time;      /* while cities_sort () runs, else 0 */

int
cities_compare_index (const City *a, const City *b, CitySort sort)
{
  if (sort == CITY_SORT_OFFSET)
    {
      /* UtcOffsetIndexComparator: the offset now, DST included */
      gint64 now = sort_time ? sort_time : g_get_real_time () / G_USEC_PER_SEC;
      int offset_a = clock_zone_offset (a->tz, now), offset_b = clock_zone_offset (b->tz, now);

      return (offset_a > offset_b) - (offset_a < offset_b);
    }
  /* NameIndexComparator: a number in the index first, then the index letters in the locale's collation */
  if (a->number != b->number)
    return a->number < b->number ? -1 : 1;
  return g_utf8_collate (a->data->index, b->data->index);
}

int
cities_compare (gconstpointer pa, gconstpointer pb, gpointer data)
{
  const City *a = pa, *b = pb;
  CitySort sort = GPOINTER_TO_INT (data);
  int result;

  result = cities_compare_index (a, b, sort);
  if (result == 0 && sort == CITY_SORT_OFFSET)
    /* UtcOffsetComparator: then by name, which is NameComparator */
    result = cities_compare_index (a, b, CITY_SORT_NAME);
  if (result == 0)
    result = g_utf8_collate (a->data->name, b->data->name);
  return result;
}

void
cities_sort (GPtrArray *cities, CitySort how)
{
  sort_time = g_get_real_time () / G_USEC_PER_SEC;
  g_ptr_array_sort_values_with_data (cities, cities_compare, GINT_TO_POINTER (how));
  sort_time = 0;
}

GPtrArray *
cities_get_selected (const char *const *ids)
{
  GPtrArray *selected = g_ptr_array_new ();

  for (; ids && *ids; ids++)
    {
      City *city = cities_find (*ids);

      if (city && !g_ptr_array_find (selected, city, NULL))
        g_ptr_array_add (selected, city);
    }
  cities_sort (selected, CITY_SORT_OFFSET);
  return selected;
}

/* --- the home time zones */

guint
home_zones_count (void)
{
  return G_N_ELEMENTS (home_zone_data);
}

const HomeZoneData *
home_zones_get (guint i)
{
  return &home_zone_data[i];
}

gboolean
home_zones_contains (const char *zone)
{
  for (guint i = 0; zone && i < G_N_ELEMENTS (home_zone_data); i++)
    if (g_str_equal (home_zone_data[i].zone, zone))
      return TRUE;
  return FALSE;
}

GTimeZone *
clock_home_zone (GSettings *settings)
{
  g_autofree char *home = g_settings_get_string (settings, "home-time-zone");
  GTimeZone *local = g_time_zone_new_local ();
  const char *id = g_time_zone_get_identifier (local);

  if (home_zones_contains (home))
    {
      GTimeZone *tz = clock_zone_new (home);

      if (tz)
        {
          g_time_zone_unref (local);
          return tz;
        }
    }
  /* no lawful home yet: the zone the phone is in, kept as home if it is a choice ("Europe/Berlin" is not, so on
   * this phone home stays unset, and the setting shows no summary, until one is picked) */
  if (home_zones_contains (id))
    g_settings_set_string (settings, "home-time-zone", id);
  return local;
}

gboolean
clock_show_home_clock (GSettings *settings)
{
  g_autoptr(GTimeZone) home = NULL, local = NULL;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  if (!g_settings_get_boolean (settings, "automatic-home-clock"))
    return FALSE;
  home = clock_home_zone (settings);
  local = g_time_zone_new_local ();
  return clock_zone_offset (home, now) != clock_zone_offset (local, now);
}
