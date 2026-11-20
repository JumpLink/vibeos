/* Time zones (cal-tz-picker.h): the time zone dialog, on ICU (ucal, uloc) and the system's zone.tab.
 *
 * The zones: zone.tab's, grouped by country (uloc_getDisplayCountry; PS is "Palestine"), a zone left out when its
 * country already has one with the same rules (the same raw offset, and for a zone with daylight saving time the
 * same transitions, here since 1970); the zone being edited goes in at its zone.tab line (an old id found through
 * ICU's IANA id) ahead of an identical one; then the Etc/GMT zones, which have no country. Sorted by the offset now
 * (east first), the country (none last), the standard name. Each row: the name at the event's time (or one of the
 * picker's own renames, "Mountain Standard - Arizona"), the time now in the zone with its offset and the sun of a
 * zone with DST, the country.
 *
 * The dialog: nearly the screen's width, as tall as its rows up to the window. On top the search field (the hint a
 * search icon, then "Type country name"; an underline, thicker while focused; a clear button), under it the rows. With
 * no search the list is the zone being edited, then the three zones chosen last (newest first). Typing offers countries
 * and offsets in a drop-down under the field ("+5", "gmt-3", a country's start, a word in it, its initials, "usa"); one
 * chosen lists that country's zones or that hour's. A search with nothing to offer lists "No results found". A row
 * chosen is the zone, and the dialog closes. */
#include "cal-tz-picker.h"
#include "cal-colors.h"
#include "cal-format.h"
#include "cal-text.h"

#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>
#include <string.h>
#include <unicode/ucal.h>
#include <unicode/uenum.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>

#define DIALOG_WIDTH 310                /* 95 % of a 360 px screen, less the insets */
#define FIELD_HEIGHT 54.0
#define FIELD_TEXT_TOP 14.0             /* the text line's top: the field's inset and padding */
#define FIELD_INSET 4.0                 /* the underline's inset at the sides */
#define FIELD_UNDERLINE_BOTTOM 7.0      /* the underline's distance from the bottom */
#define FIELD_PADDING_END 30.0          /* the search box's padding at the end */
#define FIELD_TEXT_SIZE 18.0
#define HINT_ICON_SCALE 1.25            /* the hint's search icon, to the text's size */
#define CLEAR_SIZE 40                   /* the clear button: its icon and padding */
#define CLEAR_MARGIN_END 8
#define DROPDOWN_MARGIN 8               /* the drop-down stops short of the window's bottom */
#define GMT_COLOR "#888888"             /* the offset's grey, the same in every theme */
#define DST_COLOR "#BFBFBF"             /* the sun's span */
/* ☀ (U+2600) from the colour emoji font, a yellow sun that DST_COLOR's span cannot tint; VS16 asks Pango for the
 * emoji presentation (else a grey text glyph) */
#define DST_SYMBOL "\u2600\ufe0f"
#define MAX_RECENTS 3                   /* the recent zones kept */
#define OFFSET_BASE 20                  /* the offset slots: hours −20 … +19 */
#define OFFSET_SLOTS 40
#define HOUR_MS 3600000
#define MONTH_MS (30.44 * 24 * HOUR_MS)
#define SINCE_MS 0.0                    /* same_rules compares the transitions from 1970 … */
#define UNTIL_MS 2145916800000.0        /* … to 2038, the span zic writes out */

/* the search icon, in a 24 × 24 viewport */
#define SEARCH_PATH "M15.5,14h-0.79l-0.28,-0.27C15.41,12.59 16,11.11 16,9.5 16,5.91 13.09,3 9.5,3S3,5.91 3,9.5 " \
                    "5.91,16 9.5,16c1.61,0 3.09,-0.59 4.23,-1.57l0.27,0.28v0.79l5,4.99L20.49,19l-4.99,-5z" \
                    "M9.5,14C7.01,14 5,11.99 5,9.5S7.01,5 9.5,5 14,7.01 14,9.5 11.99,14 9.5,14z"

/* --- ICU */

static char *
utf8 (const UChar *text, int32_t length)
{
  UErrorCode status = U_ZERO_ERROR;
  int32_t size = 0;
  char *out;

  u_strToUTF8 (NULL, 0, &size, text, length, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE (status))
    return NULL;
  status = U_ZERO_ERROR;
  out = g_malloc (size + 1);
  u_strToUTF8 (out, size + 1, NULL, text, length, &status);
  if (U_FAILURE (status))
    g_clear_pointer (&out, g_free);
  return out;
}

static UCalendar *
calendar_new (void)
{
  UErrorCode status = U_ZERO_ERROR;
  UCalendar *calendar = ucal_open (NULL, 0, uloc_getDefault (), UCAL_GREGORIAN, &status);

  return U_SUCCESS (status) ? calendar : NULL;
}

/* @id into @calendar; FALSE for an id ICU does not know (it would quietly be "Etc/Unknown", GMT) */
static gboolean
set_zone (UCalendar *calendar, const char *id)
{
  UChar zone[128], canonical[128];
  UErrorCode status = U_ZERO_ERROR;
  UBool system = FALSE;

  if (!id || strlen (id) >= G_N_ELEMENTS (zone) || !g_str_is_ascii (id))
    return FALSE;
  u_uastrcpy (zone, id);
  ucal_getCanonicalTimeZoneID (zone, -1, canonical, G_N_ELEMENTS (canonical), &system, &status);
  if (U_FAILURE (status) || !system)
    return FALSE;
  ucal_setTimeZone (calendar, zone, -1, &status);
  return U_SUCCESS (status);
}

/* the zone's offset from UTC at @ms, and whether it is daylight time then */
static int
offset_at (UCalendar *calendar, UDate ms, gboolean *dst)
{
  UErrorCode status = U_ZERO_ERROR;
  int zone, daylight;

  ucal_setMillis (calendar, ms, &status);
  zone = ucal_get (calendar, UCAL_ZONE_OFFSET, &status);
  daylight = ucal_get (calendar, UCAL_DST_OFFSET, &status);
  if (dst)
    *dst = daylight != 0;
  return U_SUCCESS (status) ? zone + daylight : 0;
}

static UDate
now_ms (void)
{
  return (UDate) (g_get_real_time () / 1000);
}

static UDate
year_start_ms (void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_utc ();
  g_autoptr(GDateTime) start = g_date_time_new_utc (g_date_time_get_year (now), 1, 1, 0, 0, 0);

  return (UDate) g_date_time_to_unix (start) * 1000;
}

/* daylight time this year or next */
static gboolean
uses_daylight (UCalendar *calendar, UDate year_start)
{
  for (int i = 0; i < 24; i++)
    {
      gboolean dst;

      offset_at (calendar, year_start + i * MONTH_MS, &dst);
      if (dst)
        return TRUE;
    }
  return FALSE;
}

/* the zone's long name, standard or daylight, in the default locale */
static char *
display_name (UCalendar *calendar, gboolean dst)
{
  UChar name[256];
  UErrorCode status = U_ZERO_ERROR;
  int32_t length = ucal_getTimeZoneDisplayName (calendar, dst ? UCAL_DST : UCAL_STANDARD, uloc_getDefault (), name,
                                                G_N_ELEMENTS (name), &status);

  return U_SUCCESS (status) ? utf8 (name, length) : NULL;
}

/* an IANA id from what an event may carry: libical's prefixed ids, a Windows name (an Outlook invitation's) */
static char *
normalize_id (const char *tzid)
{
  static const char *prefixes[] = { "/freeassociation.sourceforge.net/Tzfile/", "/freeassociation.sourceforge.net/" };
  UCalendar *calendar;
  UChar windows[128], iana[128];
  UErrorCode status = U_ZERO_ERROR;
  gboolean known;
  int32_t length;

  if (!tzid || !*tzid)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (prefixes); i++)
    if (g_str_has_prefix (tzid, prefixes[i]))
      return g_strdup (tzid + strlen (prefixes[i]));
  calendar = calendar_new ();
  known = !calendar || set_zone (calendar, tzid);
  g_clear_pointer (&calendar, ucal_close);
  if (known || strlen (tzid) >= G_N_ELEMENTS (windows) || !g_str_is_ascii (tzid))
    return g_strdup (tzid);
  u_uastrcpy (windows, tzid);
  length = ucal_getTimeZoneIDForWindowsID (windows, -1, NULL, iana, G_N_ELEMENTS (iana), &status);
  return U_SUCCESS (status) && length > 0 ? utf8 (iana, length) : g_strdup (tzid);
}

/* the picker's own renames: names ICU gives several zones alike */
static const struct {
  const char *id, *label;
} renames[] = {
  { "Australia/Darwin", N_("Australian Central - NT") },
  { "Australia/Adelaide", N_("Australian Central - SA") },
  { "Australia/Brisbane", N_("Australian Eastern - QLD") },
  { "Australia/Sydney", N_("Australian Eastern - NSW") },
  { "America/Dawson_Creek", N_("Mountain Standard - Dawson Creek") },
  { "America/Regina", N_("Central Standard - Saskatchewan") },
  { "America/Atikokan", N_("Eastern Standard - Atikokan") },
  { "America/Blanc-Sablon", N_("Atlantic Standard - Quebec") },
  { "Pacific/Honolulu", N_("Hawaii-Aleutian - Hawaii") },
  { "America/Adak", N_("Hawaii-Aleutian - Alaska") },
  { "America/Metlakatla", N_("Pacific Standard - Metlakatla") },
  { "America/Phoenix", N_("Mountain Standard - Arizona") },
  { "America/Tijuana", N_("Pacific - Baja California") },
  { "America/Ojinaga", N_("Mountain - Chihuahua") },
  { "America/Matamoros", N_("Central - Tamaulipas") },
  { "America/Hermosillo", N_("Mountain - Sonora") },
  { "Africa/Juba", N_("East Africa Time") },
  { "America/Kralendijk", N_("Atlantic Standard Time") },
  { "America/Lower_Princes", N_("Atlantic Standard Time") },
  { "Europe/Minsk", N_("Eastern European Time") },
  { "Europe/Kaliningrad", N_("Kaliningrad Time") },
  { "America/Porto_Velho", N_("Amazon - Rondônia") },
  { "America/Belem", N_("Brasilia - Pará") },
};

static const char *
renamed (const char *id)
{
  for (guint i = 0; i < G_N_ELEMENTS (renames); i++)
    if (g_str_equal (id, renames[i].id))
      return _(renames[i].label);
  return NULL;
}

/* "GMT+1", "GMT-3:30", "GMT+0" */
static void
append_gmt_offset (GString *text, int offset)
{
  int p = ABS (offset), minutes = (p / 60000) % 60;

  g_string_append_printf (text, "GMT%c%d", offset < 0 ? '-' : '+', p / HOUR_MS);
  if (minutes)
    g_string_append_printf (text, ":%02d", minutes);
}

/* the name, two spaces, the offset and the sun (cal_tz_label) */
static char *
gmt_display_name (const char *tzid, gint64 at, gboolean markup, gboolean gray_gmt)
{
  g_autofree char *id = normalize_id (tzid);
  g_autofree char *name = NULL;
  UCalendar *calendar = calendar_new ();
  GString *text;
  gboolean dst, daylight;
  int offset;

  if (!id || !calendar || !set_zone (calendar, id))
    {
      g_clear_pointer (&calendar, ucal_close);
      return markup ? g_markup_escape_text (tzid ? tzid : "", -1) : g_strdup (tzid ? tzid : "");
    }
  offset = offset_at (calendar, at ? (UDate) at * 1000 : now_ms (), &dst);
  name = renamed (id) ? g_strdup (renamed (id)) : display_name (calendar, dst);
  daylight = uses_daylight (calendar, year_start_ms ());
  ucal_close (calendar);
  text = g_string_new (NULL);
  if (markup)
    {
      g_autofree char *escaped = g_markup_escape_text (name ? name : id, -1);

      g_string_append (text, escaped);
    }
  else
    g_string_append (text, name ? name : id);
  g_string_append (text, "  ");
  if (markup && gray_gmt)
    g_string_append (text, "<span foreground=\"" GMT_COLOR "\">");
  append_gmt_offset (text, offset);
  if (markup && gray_gmt)
    g_string_append (text, "</span>");
  if (daylight)
    g_string_append (text, markup ? " <span foreground=\"" DST_COLOR "\">" DST_SYMBOL "</span>" : " " DST_SYMBOL);
  return g_string_free (text, FALSE);
}

char *
cal_tz_label (const char *tzid, gint64 at)
{
  return gmt_display_name (tzid, at, FALSE, FALSE);
}

char *
cal_tz_label_markup (const char *tzid, gint64 at, gboolean gray_gmt)
{
  return gmt_display_name (tzid, at, TRUE, gray_gmt);
}

/* --- the zones (TimeZoneData) */

typedef struct {
  char *id;
  char *country;                /* NULL: an Etc/GMT zone */
  int raw;                      /* the standard offset */
  int offset;                   /* the offset now */
  gboolean dst;                 /* has daylight saving time */
  GArray *transitions;          /* since 1970: the time and the offset after it; a DST zone's, when asked for */
  char *sort_name;              /* the standard name, the sort's last key */
  char *name;                   /* the name the row shows */
} Zone;

static void
zone_free (gpointer data)
{
  Zone *zone = data;

  g_free (zone->id);
  g_free (zone->country);
  g_clear_pointer (&zone->transitions, g_array_unref);
  g_free (zone->sort_name);
  g_free (zone->name);
  g_free (zone);
}

typedef struct {
  UCalendar *calendar;
  GPtrArray *zones;             /* Zone, sorted */
  char *current_id;             /* the zone being edited */
  Zone *current;                /* its Zone, or NULL */
  int current_index;
  GPtrArray *countries;         /* the countries in list order (the zones' strings) */
  GHashTable *by_country;       /* country → GArray of indices */
  GArray *by_offset[OFFSET_SLOTS];
} Zones;

static void
zones_free (Zones *zones)
{
  g_clear_pointer (&zones->calendar, ucal_close);
  g_free (zones->current_id);
  g_ptr_array_unref (zones->zones);
  g_ptr_array_unref (zones->countries);
  g_hash_table_unref (zones->by_country);
  for (int i = 0; i < OFFSET_SLOTS; i++)
    g_clear_pointer (&zones->by_offset[i], g_array_unref);
  g_free (zones);
}

static Zone *
zone_new (Zones *zones, const char *id, const char *country, UDate now, UDate year_start)
{
  UErrorCode status = U_ZERO_ERROR;
  Zone *zone;

  if (!set_zone (zones->calendar, id))
    return NULL;
  zone = g_new0 (Zone, 1);
  zone->id = g_strdup (id);
  zone->country = g_strdup (country);
  ucal_setMillis (zones->calendar, now, &status);
  zone->raw = ucal_get (zones->calendar, UCAL_ZONE_OFFSET, &status);
  zone->offset = zone->raw + ucal_get (zones->calendar, UCAL_DST_OFFSET, &status);
  zone->dst = uses_daylight (zones->calendar, year_start);
  return zone;
}

/* the zone's transitions, as far as the list needs them: each change of offset from 1970 to 2038 */
static GArray *
transitions (Zones *zones, Zone *zone)
{
  UErrorCode status = U_ZERO_ERROR;
  UDate next;

  if (zone->transitions)
    return zone->transitions;
  zone->transitions = g_array_new (FALSE, FALSE, sizeof (double));
  if (!set_zone (zones->calendar, zone->id))
    return zone->transitions;
  ucal_setMillis (zones->calendar, SINCE_MS, &status);
  for (int i = 0; i < 1000; i++)
    {
      double offset;

      if (!ucal_getTimeZoneTransitionDate (zones->calendar, UCAL_TZ_TRANSITION_NEXT, &next, &status)
          || U_FAILURE (status) || next >= UNTIL_MS)
        break;
      offset = offset_at (zones->calendar, next, NULL);
      g_array_append_val (zone->transitions, next);
      g_array_append_val (zone->transitions, offset);
    }
  return zone->transitions;
}

/* the same raw offset and the same transitions */
static gboolean
same_rules (Zones *zones, Zone *a, Zone *b)
{
  GArray *ta, *tb;

  if (a->raw != b->raw || a->dst != b->dst)
    return FALSE;
  if (!a->dst)
    return TRUE;
  ta = transitions (zones, a);
  tb = transitions (zones, b);
  return ta->len == tb->len && memcmp (ta->data, tb->data, ta->len * sizeof (double)) == 0;
}

/* a zone of the same country with the same rules, or -1 */
static int
identical_in_country (Zones *zones, Zone *zone)
{
  for (guint i = 0; i < zones->zones->len; i++)
    {
      Zone *other = zones->zones->pdata[i];

      if (g_strcmp0 (other->country, zone->country) == 0 && same_rules (zones, other, zone))
        return (int) i;
    }
  return -1;
}

/* the country in the default locale; Palestine by the library's own name */
static const char *
country_name (const char *code)
{
  static GHashTable *names;
  char *name;

  if (!names)
    names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (g_hash_table_lookup_extended (names, code, NULL, (gpointer *) &name))
    return name;
  if (g_ascii_strcasecmp (code, "PS") == 0)
    name = g_strdup (_("Palestine"));
  else
    {
      g_autofree char *locale = g_strdup_printf ("_%s", code);
      UChar text[128];
      UErrorCode status = U_ZERO_ERROR;
      int32_t length = uloc_getDisplayCountry (locale, uloc_getDefault (), text, G_N_ELEMENTS (text), &status);

      name = U_SUCCESS (status) && length > 0 ? utf8 (text, length) : NULL;
      if (g_strcmp0 (name, code) == 0)
        g_clear_pointer (&name, g_free);
    }
  g_hash_table_insert (names, g_strdup (code), name);
  return name;
}

typedef struct {
  Zones *zones;
  GHashTable *done;             /* the zones listed so far */
  const char *current, *current_iana;
  UDate now, year_start;
} Loader;

/* one zone.tab line: the zone being edited first when this is its line, then this one unless the country has its
 * like */
static void
add_location (Loader *loader, const char *code, const char *id)
{
  const char *country = country_name (code);
  Zone *zone;

  if (loader->current && !loader->zones->current && g_str_equal (id, loader->current_iana))
    {
      Zone *current = zone_new (loader->zones, loader->current, country, loader->now, loader->year_start);

      if (current)
        {
          int same = identical_in_country (loader->zones, current);

          if (same < 0)
            g_ptr_array_add (loader->zones->zones, current);
          else
            g_ptr_array_insert (loader->zones->zones, same, current);
          loader->zones->current = current;
        }
    }
  zone = zone_new (loader->zones, id, country, loader->now, loader->year_start);
  if (!zone)
    return;
  g_hash_table_add (loader->done, g_strdup (id));
  if (identical_in_country (loader->zones, zone) < 0)
    g_ptr_array_add (loader->zones->zones, zone);
  else
    zone_free (zone);
}

/* zone.tab, or without tzdata ICU's own zones by country */
static void
load_locations (Loader *loader)
{
  g_autofree char *contents = NULL;

  if (g_file_get_contents ("/usr/share/zoneinfo/zone.tab", &contents, NULL, NULL))
    {
      g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);

      for (char **line = lines; *line; line++)
        {
          g_auto(GStrv) fields = NULL;

          if (**line == '#' || !**line)
            continue;
          fields = g_strsplit (*line, "\t", 4);
          if (g_strv_length (fields) >= 3)
            add_location (loader, fields[0], fields[2]);
        }
      return;
    }
  for (const char *const *code = uloc_getISOCountries (); *code; code++)
    {
      UErrorCode status = U_ZERO_ERROR;
      UEnumeration *ids = ucal_openTimeZoneIDEnumeration (UCAL_ZONE_TYPE_CANONICAL_LOCATION, *code, NULL, &status);
      const char *id;

      while (U_SUCCESS (status) && (id = uenum_next (ids, NULL, &status)))
        add_location (loader, *code, id);
      uenum_close (ids);
    }
}

/* the list's order: the offset now (east first), the country (none last), the name */
static int
zone_compare (gconstpointer pa, gconstpointer pb)
{
  const Zone *a = *(Zone **) pa, *b = *(Zone **) pb;
  int diff;

  if (a->offset != b->offset)
    return b->offset < a->offset ? -1 : 1;
  if (!a->country && b->country)
    return 1;
  if (a->country && !b->country)
    return -1;
  if (a->country && (diff = strcmp (a->country, b->country)))
    return diff;
  return g_strcmp0 (a->sort_name, b->sort_name);
}

static Zones *
zones_new (const char *current, gint64 at)
{
  Zones *zones = g_new0 (Zones, 1);
  g_autofree char *iana = NULL;
  Loader loader = { zones, g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), current, NULL,
                    now_ms (), year_start_ms () };
  UDate when = at ? (UDate) at * 1000 : loader.now;
  UErrorCode status = U_ZERO_ERROR;
  UEnumeration *all;
  const char *id;

  zones->calendar = calendar_new ();
  zones->current_id = g_strdup (current);
  zones->zones = g_ptr_array_new_with_free_func (zone_free);
  zones->countries = g_ptr_array_new ();
  zones->by_country = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_array_unref);
  zones->current_index = -1;
  if (!zones->calendar)
    {
      g_hash_table_unref (loader.done);
      return zones;
    }

  /* the zone being edited under its zone.tab id: "Asia/Calcutta" is listed as "Asia/Kolkata" */
  if (current && g_str_is_ascii (current) && strlen (current) < 128)
    {
      UChar zone[128], found[128];
      int32_t length;

      u_uastrcpy (zone, current);
      length = ucal_getIanaTimeZoneID (zone, -1, found, G_N_ELEMENTS (found), &status);
      iana = U_SUCCESS (status) && length > 0 ? utf8 (found, length) : NULL;
      status = U_ZERO_ERROR;
    }
  loader.current_iana = iana ? iana : current;
  load_locations (&loader);

  /* the Etc/GMT zones, one for each offset */
  all = ucal_openTimeZones (&status);
  while (U_SUCCESS (status) && (id = uenum_next (all, NULL, &status)))
    {
      Zone *zone;

      if (g_hash_table_contains (loader.done, id) || !g_str_has_prefix (id, "Etc/GMT"))
        continue;
      zone = zone_new (zones, id, NULL, loader.now, loader.year_start);
      if (zone && identical_in_country (zones, zone) < 0)
        g_ptr_array_add (zones->zones, zone);
      else if (zone)
        zone_free (zone);
    }
  uenum_close (all);
  g_hash_table_unref (loader.done);

  for (guint i = 0; i < zones->zones->len; i++)
    {
      Zone *zone = zones->zones->pdata[i];

      set_zone (zones->calendar, zone->id);
      zone->sort_name = display_name (zones->calendar, FALSE);
    }
  g_ptr_array_sort (zones->zones, zone_compare);

  for (guint i = 0; i < zones->zones->len; i++)
    {
      Zone *zone = zones->zones->pdata[i];
      int slot = OFFSET_BASE + zone->offset / HOUR_MS;
      gboolean dst;

      set_zone (zones->calendar, zone->id);
      offset_at (zones->calendar, when, &dst);
      zone->name = renamed (zone->id) ? g_strdup (renamed (zone->id)) : display_name (zones->calendar, dst);
      if (!zone->name)
        zone->name = g_strdup (zone->id);
      if (zone == zones->current)
        zones->current_index = (int) i;
      if (zone->country)
        {
          GArray *group = g_hash_table_lookup (zones->by_country, zone->country);

          if (!group)
            {
              group = g_array_new (FALSE, FALSE, sizeof (int));
              g_hash_table_insert (zones->by_country, zone->country, group);
              g_ptr_array_add (zones->countries, zone->country);
            }
          g_array_append_val (group, i);
        }
      if (slot >= 0 && slot < OFFSET_SLOTS)
        {
          if (!zones->by_offset[slot])
            zones->by_offset[slot] = g_array_new (FALSE, FALSE, sizeof (int));
          g_array_append_val (zones->by_offset[slot], i);
        }
    }
  return zones;
}

static gboolean
has_hour (Zones *zones, int hours)
{
  int slot = hours + OFFSET_BASE;

  return slot >= 0 && slot < OFFSET_SLOTS && zones->by_offset[slot];
}

static int
find_zone (Zones *zones, const char *id)
{
  for (guint i = 0; i < zones->zones->len; i++)
    if (g_str_equal (((Zone *) zones->zones->pdata[i])->id, id))
      return (int) i;
  return -1;
}

/* --- the zones chosen last (preferences_recent_timezones, oldest first): the app's setting when its schema has
 * the key, else for as long as the app runs */

static GPtrArray *recent_memory;

static GSettings *
recent_settings (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "vibeos.Calendar", TRUE)
                                             : NULL;

  if (!schema || !g_settings_schema_has_key (schema, "recent-timezones"))
    return NULL;
  return g_settings_new_full (schema, NULL, NULL);
}

static char **
recent_zones (void)
{
  g_autoptr(GSettings) settings = recent_settings ();
  GStrvBuilder *builder;

  if (settings)
    return g_settings_get_strv (settings, "recent-timezones");
  builder = g_strv_builder_new ();
  for (guint i = 0; recent_memory && i < recent_memory->len; i++)
    g_strv_builder_add (builder, recent_memory->pdata[i]);
  {
    char **zones = g_strv_builder_end (builder);

    g_strv_builder_unref (builder);
    return zones;
  }
}

/* the recent zones: the zone last, once; the oldest go past three */
static void
save_recent (const char *id)
{
  g_autoptr(GSettings) settings = recent_settings ();
  g_auto(GStrv) old = recent_zones ();
  g_autoptr(GPtrArray) recents = g_ptr_array_new ();
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new ();
  g_auto(GStrv) strv = NULL;

  for (char **zone = old; *zone; zone++)
    if (!g_str_equal (*zone, id) && !g_ptr_array_find_with_equal_func (recents, *zone, g_str_equal, NULL))
      g_ptr_array_add (recents, *zone);
  while (recents->len >= MAX_RECENTS)
    g_ptr_array_remove_index (recents, 0);
  g_ptr_array_add (recents, (gpointer) id);
  for (guint i = 0; i < recents->len; i++)
    g_strv_builder_add (builder, recents->pdata[i]);
  strv = g_strv_builder_end (builder);
  if (settings)
    g_settings_set_strv (settings, "recent-timezones", (const char *const *) strv);
  else
    {
      if (!recent_memory)
        recent_memory = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_set_size (recent_memory, 0);
      for (char **zone = strv; *zone; zone++)
        g_ptr_array_add (recent_memory, g_strdup (*zone));
    }
}

/* --- the search (TimeZoneFilterTypeAdapter) */

enum { FILTER_EMPTY = -1, FILTER_NONE = 0, FILTER_GMT = 2, FILTER_COUNTRY = 4 };

typedef struct {
  int type;
  char *text;
  int hours;
} Suggestion;

static void
suggestion_free (gpointer data)
{
  Suggestion *suggestion = data;

  g_free (suggestion->text);
  g_free (suggestion);
}

static void
suggest (GPtrArray *found, int type, char *text, int hours)
{
  Suggestion *suggestion = g_new0 (Suggestion, 1);

  suggestion->type = type;
  suggestion->text = text;
  suggestion->hours = hours;
  g_ptr_array_add (found, suggestion);
}

/* [+-]?d{1,2} from @start, else G_MININT */
static int
parse_num (const char *text, gsize start)
{
  gsize i = start, length = strlen (text);
  int sign = 1, number;

  if (i >= length)
    return G_MININT;
  if (text[i] == '-' || text[i] == '+')
    {
      sign = text[i] == '-' ? -1 : 1;
      if (++i >= length)
        return G_MININT;
    }
  if (!g_ascii_isdigit (text[i]))
    return G_MININT;
  number = text[i++] - '0';
  if (i < length)
    {
      if (!g_ascii_isdigit (text[i]))
        return G_MININT;
      number = number * 10 + text[i++] - '0';
    }
  return i == length ? sign * number : G_MININT;
}

/* an offset typed: "1" offers GMT+19 … GMT+10 as well, "-1" GMT-10 … GMT-19 */
static void
search_by_gmt (Zones *zones, GPtrArray *found, int number, gboolean positive_only)
{
  if (number >= 0)
    {
      if (number == 1)
        for (int i = 19; i >= 10; i--)
          if (has_hour (zones, i))
            suggest (found, FILTER_GMT, g_strdup_printf ("GMT+%d", i), i);
      if (has_hour (zones, number))
        suggest (found, FILTER_GMT, g_strdup_printf ("GMT+%d", number), number);
      number = -number;
    }
  if (!positive_only && number != 0)
    {
      if (has_hour (zones, number))
        suggest (found, FILTER_GMT, g_strdup_printf ("GMT%d", number), number);
      if (number == -1)
        for (int i = -10; i >= -19; i--)
          if (has_hour (zones, i))
            suggest (found, FILTER_GMT, g_strdup_printf ("GMT%d", i), i);
    }
}

/* initials: "ua" for "united arab emirates", "us" for "u.s. virgin islands" */
static gboolean
starting_initials (const char *prefix, const char *text)
{
  glong count = g_utf8_strlen (prefix, -1), matched = 0;
  gboolean word_break = TRUE;

  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (!g_unichar_isalpha (c))
        {
          word_break = TRUE;
          continue;
        }
      if (word_break)
        {
          if (g_utf8_get_char (g_utf8_offset_to_pointer (prefix, matched++)) != c)
            return FALSE;
          if (matched == count)
            return TRUE;
          word_break = FALSE;
        }
    }
  return g_str_equal (prefix, "usa") && g_str_equal (text, "united states");
}

static gboolean
country_matches (const char *prefix, const char *country)
{
  g_autofree char *lower = g_utf8_strdown (country, -1);

  if (g_str_has_prefix (lower, prefix)
      || (g_utf8_get_char (lower) == g_utf8_get_char (prefix) && starting_initials (prefix, lower)))
    return TRUE;
  /* "Korea" finds "South Korea" */
  if (strchr (lower, ' '))
    {
      g_auto(GStrv) words = g_strsplit (lower, " ", -1);

      for (char **word = words; *word; word++)
        if (g_str_has_prefix (*word, prefix))
          return TRUE;
    }
  return FALSE;
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

/* what the typed text offers */
static GPtrArray *
search (Zones *zones, const char *text)
{
  GPtrArray *found = g_ptr_array_new_with_free_func (suggestion_free);
  g_autofree char *trimmed = g_strstrip (g_strdup (text));
  g_autofree char *prefix = g_utf8_strdown (trimmed, -1);
  g_autoptr(GPtrArray) countries = g_ptr_array_new ();
  gboolean gmt_only;
  gsize start = 0;
  int number;

  if (!*prefix)
    return found;
  gmt_only = *prefix == '+' || *prefix == '-';
  if (g_str_has_prefix (prefix, "gmt"))
    {
      start = 3;
      gmt_only = TRUE;
    }
  number = parse_num (prefix, start);
  if (number != G_MININT)
    search_by_gmt (zones, found, number, prefix[start] == '+');
  if (gmt_only)
    return found;
  for (guint i = 0; i < zones->countries->len; i++)
    if (country_matches (prefix, zones->countries->pdata[i]))
      g_ptr_array_add (countries, zones->countries->pdata[i]);
  g_ptr_array_sort (countries, compare_strings);
  for (guint i = 0; i < countries->len; i++)
    suggest (found, FILTER_COUNTRY, g_strdup (countries->pdata[i]), 0);
  return found;
}

/* --- the search field: the text with its underline and hint, the clear button over its end */

#define CAL_TYPE_TZ_FIELD (cal_tz_field_get_type ())
G_DECLARE_FINAL_TYPE (CalTzField, cal_tz_field, CAL, TZ_FIELD, GtkWidget)

struct _CalTzField {
  GtkWidget parent;
  GtkWidget *text, *clear;
  GskPath *search_icon;
};

G_DEFINE_FINAL_TYPE (CalTzField, cal_tz_field, GTK_TYPE_WIDGET)

static void
cal_tz_field_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                      int *min_baseline, int *nat_baseline)
{
  /* the width is the dialog's; the height the field's */
  *min = *nat = orientation == GTK_ORIENTATION_VERTICAL ? (int) FIELD_HEIGHT : 0;
}

static void
cal_tz_field_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  CalTzField *self = CAL_TZ_FIELD (widget);
  int text_height, ignored;
  /* GtkText's line is the font's ascent and descent; with font padding it starts 0.128 of the size higher */
  double top = FIELD_TEXT_TOP + 0.128 * FIELD_TEXT_SIZE;

  gtk_widget_measure (self->text, GTK_ORIENTATION_VERTICAL, -1, &ignored, &text_height, NULL, NULL);
  gtk_widget_size_allocate (self->text,
                            &(GtkAllocation) { (int) FIELD_INSET, (int) round (top),
                                               MAX (0, width - (int) (FIELD_INSET + FIELD_PADDING_END)),
                                               text_height }, -1);
  gtk_widget_size_allocate (self->clear,
                            &(GtkAllocation) { width - CLEAR_MARGIN_END - CLEAR_SIZE, (height - CLEAR_SIZE) / 2,
                                               CLEAR_SIZE, CLEAR_SIZE }, -1);
}

static void
cal_tz_field_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CalTzField *self = CAL_TZ_FIELD (widget);
  int width = gtk_widget_get_width (widget), scale = vibeos_device_scale (widget);
  gboolean focused = gtk_widget_has_focus (self->text);
  double line = focused ? 2 : 1;
  GdkRGBA underline = cal_argb (focused ? CAL_PRIMARY : CAL_ON_SURFACE_VARIANT);

  /* the underline, in from the sides and over the bottom, in the primary colour when focused */
  cal_fill_rect (snapshot, scale, FIELD_INSET, FIELD_HEIGHT - FIELD_UNDERLINE_BOTTOM - line, width - 2 * FIELD_INSET,
                 line, &underline);

  /* the hint: "   " with the search icon on the second space, bottom-aligned on the line, then the words */
  if (!*gtk_editable_get_text (GTK_EDITABLE (self->text)))
    {
      g_autoptr(PangoLayout) space = cal_text_layout (widget, " ", FIELD_TEXT_SIZE, 700);
      g_autoptr(PangoLayout) words = cal_text_layout (widget, _("Type country name"), FIELD_TEXT_SIZE, 700);
      double icon = floor (FIELD_TEXT_SIZE * scale * HINT_ICON_SCALE) / scale, baseline, height, x;
      GdkRGBA hint, tint = cal_argb (CAL_ON_SURFACE_VARIANT);
      PangoRectangle logical;

      gtk_widget_get_color (widget, &hint);
      cal_text_line (FIELD_TEXT_SIZE, scale, &baseline, &height);
      pango_layout_get_extents (space, NULL, &logical);
      x = FIELD_INSET + logical.width / (double) PANGO_SCALE;
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) x, (float) (FIELD_TEXT_TOP + height - icon)));
      gtk_snapshot_scale (snapshot, (float) (icon / 24), (float) (icon / 24));
      gtk_snapshot_append_fill (snapshot, self->search_icon, GSK_FILL_RULE_WINDING, &tint);
      gtk_snapshot_restore (snapshot);
      x += icon + logical.width / (double) PANGO_SCALE;
      cal_text_ellipsize (words, width - FIELD_PADDING_END - x);
      cal_text_draw (snapshot, words, x, FIELD_TEXT_TOP + baseline, 0, &hint);
    }
  gtk_widget_snapshot_child (widget, self->text, snapshot);
  if (gtk_widget_get_visible (self->clear))
    gtk_widget_snapshot_child (widget, self->clear, snapshot);
}

static void
cal_tz_field_dispose (GObject *object)
{
  CalTzField *self = CAL_TZ_FIELD (object);

  g_clear_pointer (&self->text, gtk_widget_unparent);
  g_clear_pointer (&self->clear, gtk_widget_unparent);
  g_clear_pointer (&self->search_icon, gsk_path_unref);
  G_OBJECT_CLASS (cal_tz_field_parent_class)->dispose (object);
}

static void
cal_tz_field_class_init (CalTzFieldClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = cal_tz_field_dispose;
  widget_class->measure = cal_tz_field_measure;
  widget_class->size_allocate = cal_tz_field_allocate;
  widget_class->snapshot = cal_tz_field_snapshot;
}

static void
on_field_focus (GtkEventControllerFocus *focus, CalTzField *self)
{
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
cal_tz_field_init (CalTzField *self)
{
  GtkEventController *focus = gtk_event_controller_focus_new ();
  GtkWidget *icon = gtk_image_new_from_icon_name ("cal-close-symbolic");

  self->search_icon = gsk_path_parse (SEARCH_PATH);
  gtk_widget_add_css_class (GTK_WIDGET (self), "cal-tz-field");
  self->text = gtk_text_new ();
  gtk_text_set_input_purpose (GTK_TEXT (self->text), GTK_INPUT_PURPOSE_FREE_FORM);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->text), GTK_ACCESSIBLE_PROPERTY_PLACEHOLDER,
                                  _("Type country name"), -1);
  gtk_widget_set_parent (self->text, GTK_WIDGET (self));
  g_signal_connect (focus, "enter", G_CALLBACK (on_field_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_field_focus), self);
  gtk_widget_add_controller (self->text, focus);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  self->clear = vibeos_pressable_new (icon);
  gtk_widget_add_css_class (self->clear, "cal-tz-clear");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->clear), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_set_focus_on_click (self->clear, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->clear), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Clear search"), -1);
  gtk_widget_set_visible (self->clear, FALSE);
  gtk_widget_set_parent (self->clear, GTK_WIDGET (self));
}

/* --- a holder as wide as it is told: what a list of labels asks for no longer widens the dialog */

static void
fixed_measure (GtkWidget *holder, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
               int *nat_base)
{
  GtkWidget *child = gtk_widget_get_first_child (holder);
  int width = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (holder), "cal-tz-width"));
  int max_height = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (holder), "cal-tz-max-height"));

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      *min = *nat = width;
      return;
    }
  gtk_widget_measure (child, orientation, for_size < 0 ? width : for_size, min, nat, NULL, NULL);
  if (max_height > 0)
    {
      *nat = MIN (*nat, max_height);
      *min = MIN (*min, *nat);
    }
}

static void
fixed_allocate (GtkWidget *holder, int width, int height, int baseline)
{
  gtk_widget_allocate (gtk_widget_get_first_child (holder), width, height, baseline, NULL);
}

static GtkWidget *
fixed_width (GtkWidget *child, int width)
{
  GtkWidget *holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_set_layout_manager (holder, gtk_custom_layout_new (NULL, fixed_measure, fixed_allocate));
  g_object_set_data (G_OBJECT (holder), "cal-tz-width", GINT_TO_POINTER (width));
  gtk_box_append (GTK_BOX (holder), child);
  return holder;
}

/* --- the dialog (TimeZonePickerView) */

typedef struct {
  VibeosDialog *dialog;
  Zones *zones;
  CalTzChosen chosen;
  gpointer data;
  GtkWidget *content, *field, *rows, *scroll;
  GtkWidget *dropdown, *dropdown_rows;          /* the drop-down: on the window's overlay while it shows */
  GtkOverlay *overlay;
  gboolean setting_text;                         /* a suggestion's text going in: no search */
  gboolean use_24h;
} Picker;

static GtkWidget *
row_label (const char *css, double size, gboolean wrap, int lines)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_widget_add_css_class (label, css);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  if (wrap)
    {
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    }
  if (lines > 0)
    {
      gtk_label_set_lines (GTK_LABEL (label), lines);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    }
  vibeos_label_set_font_padding_sp (label, size);
  return label;
}

/* the row's second line: the time now in the zone, its offset in grey, the sun of a zone with DST */
static char *
row_offset_markup (Picker *picker, Zone *zone)
{
  g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc (g_get_real_time () / G_USEC_PER_SEC
                                                             + zone->offset / 1000);
  g_autofree char *clock = g_date_time_format (time, picker->use_24h ? "%H:%M" : "%-I:%M %p");
  g_autofree char *escaped = g_markup_escape_text (clock ? clock : "", -1);
  GString *text = g_string_new (escaped);

  g_string_append (text, "  <span foreground=\"" GMT_COLOR "\">");
  append_gmt_offset (text, zone->offset);
  g_string_append (text, "</span>");
  if (zone->dst)
    g_string_append (text, " <span foreground=\"" DST_COLOR "\">" DST_SYMBOL "</span>");
  return g_string_free (text, FALSE);
}

/* a row: the name (two lines at most), the time and offset, the country (kept in place when none) */
static GtkWidget *
zone_item (const char *name, const char *offset_markup, const char *country)
{
  GtkWidget *item = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = row_label ("cal-tz-name", 18, TRUE, 2);

  gtk_widget_add_css_class (item, "cal-tz-item");
  gtk_label_set_label (GTK_LABEL (label), name);
  gtk_box_append (GTK_BOX (item), label);
  label = row_label ("cal-tz-offset", 14, FALSE, 1);
  gtk_label_set_markup (GTK_LABEL (label), offset_markup);
  gtk_box_append (GTK_BOX (item), label);
  label = row_label ("cal-tz-location", 14, TRUE, 0);
  gtk_label_set_label (GTK_LABEL (label), country ? country : " ");
  /* View.INVISIBLE */
  if (!country)
    gtk_widget_set_opacity (label, 0);
  gtk_box_append (GTK_BOX (item), label);
  return item;
}

static void hide_dropdown (Picker *picker);

/* a row chosen: the zone, kept among the recent ones, and the dialog closes */
static void
on_zone_row (VibeosPressable *row, Picker *picker)
{
  Zone *zone = picker->zones->zones->pdata[GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-tz-index"))];
  g_autofree char *id = g_strdup (zone->id);

  hide_dropdown (picker);
  save_recent (id);
  if (picker->chosen)
    picker->chosen (id, picker->data);
  vibeos_dialog_respond (picker->dialog, VIBEOS_RESPONSE_OK);
}

static void
add_zone_row (Picker *picker, int index)
{
  Zone *zone = picker->zones->zones->pdata[index];
  g_autofree char *offset = row_offset_markup (picker, zone);
  GtkWidget *row = vibeos_pressable_new (zone_item (zone->name, offset, zone->country));

  gtk_widget_add_css_class (row, "cal-tz-row");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_set_focus_on_click (row, FALSE);
  g_object_set_data (G_OBJECT (row), "cal-tz-index", GINT_TO_POINTER (index));
  g_signal_connect (row, "clicked", G_CALLBACK (on_zone_row), picker);
  gtk_box_append (GTK_BOX (picker->rows), row);
}

/* "No results found" over an invisible row, so it is as tall as one */
static void
add_empty_row (Picker *picker)
{
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *item = zone_item ("X", "X", "X");
  GtkWidget *label = gtk_label_new (_("No results found"));

  gtk_widget_set_opacity (item, 0);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), item);
  gtk_widget_add_css_class (label, "cal-tz-empty-text");
  vibeos_label_set_font_padding_sp (label, 16);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (label, 8);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), label);
  gtk_widget_add_css_class (overlay, "cal-tz-empty");
  gtk_box_append (GTK_BOX (picker->rows), overlay);
}

/* the rows for a filter: the zone being edited and the recent ones, an offset's zones, or a country's */
static void
show_results (Picker *picker, int type, const char *text, int hours)
{
  Zones *zones = picker->zones;
  GtkWidget *child;
  GArray *indices = NULL;

  while ((child = gtk_widget_get_first_child (picker->rows)))
    gtk_box_remove (GTK_BOX (picker->rows), child);
  switch (type)
    {
    case FILTER_EMPTY:
      add_empty_row (picker);
      break;
    case FILTER_NONE:
      {
        g_auto(GStrv) recents = recent_zones ();
        int n = (int) g_strv_length (recents);

        /* the zone being edited, then the ones chosen last, newest first */
        if (zones->current_index >= 0)
          add_zone_row (picker, zones->current_index);
        for (int i = n - 1; i >= 0; i--)
          {
            int index;

            if (!*recents[i] || g_strcmp0 (recents[i], zones->current_id) == 0)
              continue;
            index = find_zone (zones, recents[i]);
            if (index >= 0)
              add_zone_row (picker, index);
          }
        break;
      }
    case FILTER_GMT:
      if (has_hour (zones, hours))
        indices = zones->by_offset[hours + OFFSET_BASE];
      break;
    case FILTER_COUNTRY:
      indices = text ? g_hash_table_lookup (zones->by_country, text) : NULL;
      break;
    }
  for (guint i = 0; indices && i < indices->len; i++)
    add_zone_row (picker, g_array_index (indices, int, i));
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (picker->scroll), 0);
}

/* --- the drop-down (as wide as the field, under it) */

static GtkWidget *
field_text (Picker *picker)
{
  return CAL_TZ_FIELD (picker->field)->text;
}

static void
hide_dropdown (Picker *picker)
{
  if (!picker->dropdown)
    return;
  if (picker->overlay && gtk_widget_get_parent (picker->dropdown) == GTK_WIDGET (picker->overlay))
    gtk_overlay_remove_overlay (picker->overlay, picker->dropdown);
  g_clear_object (&picker->dropdown);
  picker->dropdown_rows = NULL;
}

/* the drop-down under the field wherever the dialog is (it moves when the keyboard resizes the window) */
static gboolean
follow_field (GtkWidget *dropdown, GdkFrameClock *clock, gpointer data)
{
  Picker *picker = data;
  graphene_rect_t field;
  int top, height;

  if (!picker->overlay || !gtk_widget_compute_bounds (picker->field, GTK_WIDGET (picker->overlay), &field))
    return G_SOURCE_CONTINUE;
  top = (int) round (field.origin.y + field.size.height);
  height = MAX (0, gtk_widget_get_height (GTK_WIDGET (picker->overlay)) - top - DROPDOWN_MARGIN);
  if (gtk_widget_get_margin_start (dropdown) != (int) round (field.origin.x)
      || gtk_widget_get_margin_top (dropdown) != top
      || GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dropdown), "cal-tz-max-height")) != height
      || GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dropdown), "cal-tz-width")) != (int) field.size.width)
    {
      gtk_widget_set_margin_start (dropdown, (int) round (field.origin.x));
      gtk_widget_set_margin_top (dropdown, top);
      g_object_set_data (G_OBJECT (dropdown), "cal-tz-width", GINT_TO_POINTER ((int) field.size.width));
      g_object_set_data (G_OBJECT (dropdown), "cal-tz-max-height", GINT_TO_POINTER (MAX (1, height)));
      gtk_widget_queue_resize (dropdown);
    }
  return G_SOURCE_CONTINUE;
}

/* a country or an offset chosen: its text in the field, the keyboard down, the list filtered by it */
static void
on_suggestion (VibeosPressable *row, Picker *picker)
{
  Suggestion *suggestion = g_object_get_data (G_OBJECT (row), "cal-tz-suggestion");
  g_autofree char *text = g_strdup (suggestion->text);
  int type = suggestion->type, hours = suggestion->hours;
  GtkWidget *entry = field_text (picker);

  picker->setting_text = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (entry), text);
  gtk_editable_set_position (GTK_EDITABLE (entry), -1);
  picker->setting_text = FALSE;
  hide_dropdown (picker);
  /* the keyboard down: the focus to the dialog, where Back still reaches it */
  gtk_widget_grab_focus (picker->content);
  show_results (picker, type, text, hours);
}

static void
show_dropdown (Picker *picker, GPtrArray *found)
{
  GtkWidget *scroll, *card;

  if (!picker->overlay)
    return;
  if (!picker->dropdown)
    {
      picker->dropdown_rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      scroll = spongy_scroll_view_new ();
      spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), picker->dropdown_rows);
      card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (card, "cal-tz-dropdown");
      gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
      gtk_box_append (GTK_BOX (card), scroll);
      picker->dropdown = g_object_ref_sink (fixed_width (card, gtk_widget_get_width (picker->field)));
      gtk_widget_set_halign (picker->dropdown, GTK_ALIGN_START);
      gtk_widget_set_valign (picker->dropdown, GTK_ALIGN_START);
      gtk_overlay_add_overlay (picker->overlay, picker->dropdown);
      gtk_overlay_set_clip_overlay (picker->overlay, picker->dropdown, TRUE);
      gtk_widget_add_tick_callback (picker->dropdown, follow_field, picker, NULL);
      follow_field (picker->dropdown, NULL, picker);
    }
  else
    {
      GtkWidget *child;

      while ((child = gtk_widget_get_first_child (picker->dropdown_rows)))
        gtk_box_remove (GTK_BOX (picker->dropdown_rows), child);
    }
  for (guint i = 0; i < found->len; i++)
    {
      Suggestion *suggestion = found->pdata[i], *copy = g_new0 (Suggestion, 1);
      GtkWidget *label = gtk_label_new (suggestion->text);
      GtkWidget *row;

      /* two lines at most */
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_lines (GTK_LABEL (label), 2);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
      vibeos_label_set_font_padding_sp (label, 18);
      row = vibeos_pressable_new (label);
      gtk_widget_add_css_class (row, "cal-tz-suggestion");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      gtk_widget_set_focus_on_click (row, FALSE);
      *copy = *suggestion;
      copy->text = g_strdup (suggestion->text);
      g_object_set_data_full (G_OBJECT (row), "cal-tz-suggestion", copy, suggestion_free);
      g_signal_connect (row, "clicked", G_CALLBACK (on_suggestion), picker);
      gtk_box_append (GTK_BOX (picker->dropdown_rows), row);
    }
}

/* the filter's results: offered in the drop-down; none lists "No results found", or with the field empty the
 * zone being edited and the recent ones again */
static void
run_search (Picker *picker)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (field_text (picker)));
  g_autoptr(GPtrArray) found = search (picker->zones, text);

  if (found->len == 0)
    {
      hide_dropdown (picker);
      show_results (picker, *text ? FILTER_EMPTY : FILTER_NONE, NULL, 0);
      return;
    }
  if (gtk_widget_has_focus (field_text (picker)))
    show_dropdown (picker, found);
}

static void
on_text_changed (GtkEditable *editable, Picker *picker)
{
  CalTzField *field = CAL_TZ_FIELD (picker->field);

  gtk_widget_set_visible (field->clear, *gtk_editable_get_text (editable) != '\0');
  gtk_widget_queue_draw (picker->field);
  if (!picker->setting_text)
    run_search (picker);
}

static void
on_clear (VibeosPressable *button, Picker *picker)
{
  gtk_editable_set_text (GTK_EDITABLE (field_text (picker)), "");
}

/* a touch: outside the field it closes the drop-down (and goes on to what it touched); on the field, with no
 * drop-down showing, it searches again */
static void
on_content_pressed (GtkGestureClick *click, int n, double x, double y, Picker *picker)
{
  graphene_point_t point = GRAPHENE_POINT_INIT ((float) x, (float) y);
  graphene_rect_t field, clear;
  gboolean on_field = gtk_widget_compute_bounds (picker->field, picker->content, &field)
                      && graphene_rect_contains_point (&field, &point);
  GtkWidget *clear_button = CAL_TZ_FIELD (picker->field)->clear;

  if (gtk_widget_get_visible (clear_button) && gtk_widget_compute_bounds (clear_button, picker->content, &clear)
      && graphene_rect_contains_point (&clear, &point))
    return;
  if (!on_field)
    hide_dropdown (picker);
  else if (!picker->dropdown && *gtk_editable_get_text (GTK_EDITABLE (field_text (picker))))
    {
      gtk_widget_grab_focus (field_text (picker));
      run_search (picker);
    }
}

/* Back with the drop-down showing closes the drop-down only */
static gboolean
on_content_key (GtkEventControllerKey *keys, guint keyval, guint keycode, GdkModifierType state, Picker *picker)
{
  if ((keyval != GDK_KEY_Escape && keyval != GDK_KEY_Back) || !picker->dropdown)
    return FALSE;
  hide_dropdown (picker);
  return TRUE;
}

static void
on_response (VibeosDialog *dialog, int response, Picker *picker)
{
  hide_dropdown (picker);
}

static void
picker_free (gpointer data, GClosure *closure)
{
  Picker *picker = data;

  hide_dropdown (picker);
  g_clear_object (&picker->overlay);
  zones_free (picker->zones);
  g_free (picker);
}

void
cal_tz_picker_show (GtkWidget *parent, const char *current, gint64 at, CalTzChosen chosen, gpointer data)
{
  Picker *picker = g_new0 (Picker, 1);
  g_autofree char *id = normalize_id (current);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkGesture *press = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkOverlay *overlay = vibeos_window_get_overlay (parent);

  picker->dialog = vibeos_dialog_new (NULL);
  picker->zones = zones_new (id, at);
  picker->chosen = chosen;
  picker->data = data;
  picker->use_24h = cal_format_uses_24h ();
  picker->overlay = overlay ? g_object_ref (overlay) : NULL;

  picker->field = g_object_new (CAL_TYPE_TZ_FIELD, NULL);
  g_signal_connect (field_text (picker), "changed", G_CALLBACK (on_text_changed), picker);
  g_signal_connect (CAL_TZ_FIELD (picker->field)->clear, "clicked", G_CALLBACK (on_clear), picker);
  gtk_box_append (GTK_BOX (box), picker->field);

  picker->rows = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  picker->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (picker->scroll), picker->rows);
  gtk_box_append (GTK_BOX (box), picker->scroll);

  picker->content = fixed_width (box, DIALOG_WIDTH);
  gtk_widget_add_css_class (picker->content, "cal-tz-picker");
  /* focusable: where the focus goes when a suggestion takes it from the field */
  gtk_widget_set_focusable (picker->content, TRUE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (press), GTK_PHASE_CAPTURE);
  g_signal_connect (press, "pressed", G_CALLBACK (on_content_pressed), picker);
  gtk_widget_add_controller (picker->content, GTK_EVENT_CONTROLLER (press));
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_content_key), picker);
  gtk_widget_add_controller (picker->content, keys);

  vibeos_dialog_set_flush (picker->dialog, TRUE);
  vibeos_dialog_set_content (picker->dialog, picker->content);
  gtk_widget_add_css_class (gtk_widget_get_parent (picker->content), "cal-tz-card");
  g_signal_connect_data (picker->dialog, "response", G_CALLBACK (on_response), picker, picker_free, 0);
  show_results (picker, FILTER_NONE, NULL, 0);
  vibeos_dialog_present (picker->dialog, parent);
  /* the field first: TimeZonePickerDialog's window resizes for the keyboard, which comes up with it */
  gtk_widget_grab_focus (field_text (picker));
}
