/* The world-clock cities (DeskClock's CityDAO, City and CityModel; spec/clock.md §6).
 *
 * The 340 cities come from DeskClock's own list (cities-data.h, made by tools/cities.py): an id ("C61"), the
 * letter the picker files it under, an English name and an IANA time zone. The time zones are the system's (tzdata);
 * the few old names in the list ("Asia/Calcutta") are followed to their new ones through tzdata's links, as ICU does
 * on Android. The user's choice is the "selected-cities" setting, a list of ids.
 *
 * Two orders, as the cities picker sorts: by name (the index letter, then the name, in the locale's collation) or
 * by the time (the UTC offset now, then the name). The Clock tab always shows the chosen cities by time. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct
{
  const char *id;               /* "C61" */
  const char *index;            /* the picker's section letter, "B" */
  const char *name;             /* "Berlin" */
  const char *zone;             /* "Europe/Berlin" */
} CityData;

typedef struct
{
  const CityData *data;
  GTimeZone *tz;
  int number;                   /* a number in the index, or -1 (CityDAO's NUMERIC_INDEX_REGEX) */
  char *search_key;             /* the name upper-cased without " -.'" (City.removeSpecialCharacters) */
} City;

typedef enum {
  CITY_SORT_NAME,
  CITY_SORT_OFFSET,             /* "Sort by time" */
} CitySort;

/* every city whose zone this system knows, by the list's order; the array and cities live as long as the program */
GPtrArray  *cities_get_all (void);
City       *cities_find (const char *id);

/* the cities in @ids that exist, by time (CityModel.getSelectedCities: UtcOffsetComparator); free the array only */
GPtrArray  *cities_get_selected (const char * const *ids);

/* sorts @cities (City *) by name or by time, with one "now" for the whole sort */
void        cities_sort (GPtrArray *cities, CitySort how);
/* GCompareDataFunc for g_ptr_array_sort_values_with_data (City *), @data a CitySort: by name, or by time */
int         cities_compare (gconstpointer a, gconstpointer b, gpointer data);
/* only the section part of that order: the index letters, or the offsets (Name/UtcOffsetIndexComparator) */
int         cities_compare_index (const City *a, const City *b, CitySort sort);

/* whether @city's name starts with @query, both upper-cased and without " -.'" ("StL" finds "St. Lucia") */
gboolean    cities_matches (const City *city, const char *query);
/* @query as cities_matches wants it */
char       *cities_search_key (const char *query);

/* a time zone by its IANA id, old names included; NULL if the system has no such zone */
GTimeZone  *clock_zone_new (const char *id);
/* the zone's UTC offset at @time (Unix seconds), DST included, in seconds */
int         clock_zone_offset (GTimeZone *tz, gint64 time);
/* "GMT +1:00": the zone's standard offset, without DST (Utils.getGMTHourOffset) */
char       *clock_zone_gmt_label (GTimeZone *tz, gint64 time);

/* The "Home time zone" setting's 85 choices, each a zone and an English place name ("Amsterdam, Berlin"), in
 * DeskClock's order (timezone_values, timezone_labels) */
typedef struct
{
  const char *zone;
  const char *name;
} HomeZoneData;

guint               home_zones_count (void);
const HomeZoneData *home_zones_get (guint i);
/* SettingsDAO.getHomeTimeZone takes a stored zone only if it is one of these */
gboolean            home_zones_contains (const char *zone);

/* SettingsDAO.getHomeTimeZone: the "home-time-zone" setting if it is one of the choices; else the phone's own zone,
 * which is stored as home if it is one of them (the first time the app sees itself in one) */
GTimeZone          *clock_home_zone (GSettings *settings);
/* SettingsModel.getShowHomeClock: "Automatic home clock" is on and home's UTC offset differs from here, now */
gboolean            clock_show_home_clock (GSettings *settings);

G_END_DECLS
