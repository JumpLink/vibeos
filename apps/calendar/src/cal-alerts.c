/* When reminders are due (cal-alerts.h). */
#include "cal-alerts.h"
#include "cal-format.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>

static char *
digest (const char *text)
{
  /* 16 hex digits of SHA-1: unique enough among one phone's reminders, and any UID becomes a plain key */
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, text, -1);

  return g_strndup (sum, 16);
}

char *
cal_alert_occurrence_key (const CalEvent *event)
{
  g_autofree char *text = g_strdup_printf ("%s\n%s\n%s\n%" G_GINT64_FORMAT, event->calendar ? event->calendar : "",
                                           event->uid ? event->uid : "", event->rid ? event->rid : "", event->start);

  return digest (text);
}

char *
cal_alert_key (const CalEvent *event, int minutes)
{
  g_autofree char *occurrence = cal_alert_occurrence_key (event);
  g_autofree char *text = g_strdup_printf ("%s/%d", occurrence, minutes);

  return digest (text);
}

gboolean
cal_alert_notifies (const CalReminder *reminder)
{
  return reminder->method == CAL_METHOD_NOTIFICATION || reminder->method == CAL_METHOD_ALARM;
}

gboolean
cal_alert_skipped (const CalEvent *event, const char *skip_reminders)
{
  gboolean own = event->attendees->len == 0 && !event->organizer;
  gboolean responded = own || (event->self_status != CAL_STATUS_NONE && event->self_status != CAL_STATUS_INVITED);

  if (event->self_status == CAL_STATUS_DECLINED)
    return TRUE;
  return g_strcmp0 (skip_reminders, "not_responded") == 0 && !responded;
}

void
cal_alert_free (CalAlert *alert)
{
  cal_event_unref (alert->event);
  g_ptr_array_unref (alert->keys);
  g_free (alert);
}

void
cal_alerts_compute (GPtrArray *events, GHashTable *fired, gint64 now, GPtrArray *due, gint64 *next)
{
  g_autoptr(GHashTable) latest = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  *next = 0;
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];

      for (guint r = 0; event->reminders && r < event->reminders->len; r++)
        {
          const CalReminder *reminder = &g_array_index (event->reminders, CalReminder, r);
          gint64 time = event->start - (gint64) reminder->minutes * 60;
          g_autofree char *key = NULL;

          if (!cal_alert_notifies (reminder) || reminder->minutes < 0)
            continue;
          if (time > now)
            {
              if (!*next || time < *next)
                *next = time;
              continue;
            }
          key = cal_alert_key (event, reminder->minutes);
          /* notified already; or too late: the event is over */
          if (g_hash_table_contains (fired, key) || MAX (event->end, event->start) <= now)
            continue;
          {
            char *occurrence = cal_alert_occurrence_key (event);
            CalAlert *before = g_hash_table_lookup (latest, occurrence);

            if (before)
              {
                /* the same occurrence: one notification, for its latest reminder */
                if (time > before->time)
                  {
                    before->minutes = reminder->minutes;
                    before->time = time;
                  }
                g_ptr_array_add (before->keys, g_steal_pointer (&key));
                g_free (occurrence);
                continue;
              }
            before = g_new0 (CalAlert, 1);
            before->event = cal_event_ref (event);
            before->minutes = reminder->minutes;
            before->time = time;
            before->keys = g_ptr_array_new_with_free_func (g_free);
            g_ptr_array_add (before->keys, g_steal_pointer (&key));
            g_hash_table_insert (latest, occurrence, before);
            g_ptr_array_add (due, before);
          }
        }
    }
}

/* --- the notification's buttons */

/* web and mail addresses, near enough: a scheme or "www." and what follows, a host with a dotted top-level name of
 * letters (two or more: no list of the top-level domains), a mail address */
#define WEB_PATTERN   "(?i)(?:\\b(?:https?|rtsp)://|\\bwww\\.)[^\\s]+" \
                      "|\\b[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*" \
                      "\\.[a-z]{2,}\\b(?:/[^\\s]*)?"
#define MAIL_PATTERN  "[a-zA-Z0-9+._%-]{1,256}@[a-zA-Z0-9][a-zA-Z0-9-]{0,64}(?:\\.[a-zA-Z0-9][a-zA-Z0-9-]{0,25})+"
/* COORD_PATTERN: degrees ("37°25'19.49\"N, 122°5'4.47\"W") or decimals ("37.42, -122.08") */
#define COORD_DMS     "°\\s*(?:[1-5]?[0-9]')?\\s*(?:[1-5]?[0-9](?:\\.[0-9]+)?\")?"
#define COORD_LAT_DEG "(?:[-+NnSs]\\s*)?[1-9]?[0-9]" COORD_DMS "(?:\\s*[NnSs])?"
#define COORD_LON_DEG "(?:[-+EeWw]\\s*)?1?[0-9]?[0-9]" COORD_DMS "(?:\\s*[EeWw])?"
#define COORD_PATTERN COORD_LAT_DEG "\\s*,\\s*" COORD_LON_DEG \
                      "|[+-]?[1-9]?[0-9]\\.[0-9]+°?\\s*,\\s*[+-]?1?[0-9]?[0-9]\\.[0-9]+°?"
/* a phone number outside the US: a run of digits and the separators people write ("+49 (30) 123 45-67",
 * "030/1234567") with 6 to 15 digits; shorter than that is a house number or a postcode, longer is no phone number
 * anywhere (E.164) */
#define PHONE_PATTERN "(?:\\+|\\b)[0-9][0-9 ()./-]*[0-9]\\b"
#define PHONE_MIN_DIGITS 6
#define PHONE_MAX_DIGITS 15

typedef struct {
  int start, end;               /* bytes */
} Span;

static gboolean
overlaps (GArray *spans, int start, int end)
{
  for (guint i = 0; i < spans->len; i++)
    {
      Span *span = &g_array_index (spans, Span, i);

      if (start < span->end && end > span->start)
        return TRUE;
    }
  return FALSE;
}

static gboolean
find_links (const char *pattern, const char *text, GArray *spans, GPtrArray *uris, const char *scheme)
{
  g_autoptr(GRegex) regex = g_regex_new (pattern, G_REGEX_OPTIMIZE, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  gboolean found = FALSE;

  for (g_regex_match (regex, text, 0, &match); g_match_info_matches (match); g_match_info_next (match, NULL))
    {
      int start, end;

      g_match_info_fetch_pos (match, 0, &start, &end);
      if (overlaps (spans, start, end))
        continue;
      g_array_append_val (spans, ((Span) { start, end }));
      if (uris)
        g_ptr_array_add (uris, scheme ? g_strconcat (scheme, g_match_info_fetch (match, 0), NULL) : NULL);
      found = TRUE;
    }
  return found;
}

/* what a dialer gets: the digits and '+' (Linkify strips the rest) */
static char *
tel_uri (const char *text, int start, int end)
{
  GString *uri = g_string_new ("tel:");

  for (int i = start; i < end; i++)
    if (g_ascii_isdigit (text[i]) || text[i] == '+')
      g_string_append_c (uri, text[i]);
  return g_string_free (uri, FALSE);
}

static int
count_digits (const char *text, int start, int end)
{
  int digits = 0;

  for (int i = start; i < end; i++)
    digits += g_ascii_isdigit (text[i]);
  return digits;
}

/* from @start (not a space), the end of a North American number, or −1 */
static int
nanp_match_end (const char *text, int start)
{
  int length = strlen (text), pos, digits = 0;
  char first = 'x';
  gboolean space_after_area = FALSE;

  if (length > start + 4 && g_ascii_strncasecmp (text + start, "tel:", 4) == 0)
    start += 4;
  for (pos = start; pos <= length; pos++)
    {
      char ch = pos < length ? text[pos] : 27;

      if (g_ascii_isdigit (ch))
        {
          if (digits == 0)
            first = ch;
          if (++digits > 11)
            return -1;
        }
      else if (g_ascii_isspace (ch))
        {
          if ((first == '1' && digits == 4) || digits == 3)
            space_after_area = TRUE;
          else if (first == '1' && digits == 1)
            ;
          else if (space_after_area && ((first == '1' && digits == 7) || digits == 6))
            ;
          else
            break;
        }
      else if (!strchr ("()+-*#.", ch) || ch == '\0')
        break;
    }
  if ((first != '1' && (digits == 7 || digits == 10)) || (first == '1' && digits == 11))
    return pos;
  return -1;
}

static char *
geo_uri (const char *text)
{
  g_autofree char *query = g_uri_escape_string (text, NULL, FALSE);

  return g_strconcat ("geo:0,0?q=", query, NULL);
}

void
cal_alert_location_links (const char *location, const char *region, char **geo, char **tel)
{
  g_autoptr(GArray) spans = g_array_new (FALSE, FALSE, sizeof (Span));
  g_autofree char *text = g_strdup (location ? location : "");
  const char *trimmed = g_strstrip (text);
  gboolean linked;

  *geo = *tel = NULL;
  if (!*trimmed)
    return;
  if (g_strcmp0 (region, "US") != 0)
    {
      /* Linkify.ALL; its result only when it is one link over the whole text, else the whole text on the map */
      g_autoptr(GArray) phones = g_array_new (FALSE, FALSE, sizeof (Span));

      find_links (MAIL_PATTERN, trimmed, spans, NULL, NULL);
      find_links (WEB_PATTERN, trimmed, spans, NULL, NULL);
      find_links (PHONE_PATTERN, trimmed, phones, NULL, NULL);
      for (guint i = 0; i < phones->len; i++)
        {
          Span *phone = &g_array_index (phones, Span, i);
          int digits = count_digits (trimmed, phone->start, phone->end);

          if (digits >= PHONE_MIN_DIGITS && digits <= PHONE_MAX_DIGITS && !overlaps (spans, phone->start, phone->end))
            {
              if (spans->len == 0 && phones->len == 1 && phone->start == 0 && phone->end == (int) strlen (trimmed))
                {
                  *tel = tel_uri (trimmed, phone->start, phone->end);
                  return;
                }
              g_array_append_val (spans, *phone);
            }
        }
      if (spans->len == 1 && g_array_index (spans, Span, 0).start == 0
          && g_array_index (spans, Span, 0).end == (int) strlen (trimmed))
        return;                 /* one web or mail address: neither map nor call */
      *geo = geo_uri (trimmed);
      return;
    }
  /* the US: Linkify without phone numbers, then coordinates, then NANP numbers; the map for none of them */
  linked = find_links (MAIL_PATTERN, trimmed, spans, NULL, NULL);
  linked |= find_links (WEB_PATTERN, trimmed, spans, NULL, NULL);
  {
    g_autoptr(GPtrArray) coords = g_ptr_array_new_with_free_func (g_free);
    gboolean found = find_links (COORD_PATTERN, trimmed, spans, coords, "");
    int length = strlen (trimmed), pos = 0, end = length - 7 + 1;

    if (found)
      *geo = geo_uri (coords->pdata[0]);
    while (pos < end)
      {
        int match;

        while (pos < end && g_ascii_isspace (trimmed[pos]))
          pos++;
        if (pos >= end)
          break;
        match = nanp_match_end (trimmed, pos);
        if (match > pos)
          {
            if (!overlaps (spans, pos, match))
              {
                if (!*tel)
                  *tel = tel_uri (trimmed, pos, match);
                g_array_append_val (spans, ((Span) { pos, match }));
              }
            pos = match;
          }
        else
          while (pos < end && !g_ascii_isspace (trimmed[pos]))
            pos++;
      }
    if (!linked && !found && !*tel)
      *geo = geo_uri (trimmed);
  }
}

char *
cal_alert_region (void)
{
  const char *locale = setlocale (LC_TELEPHONE, NULL);
  const char *underscore = locale ? strchr (locale, '_') : NULL;

  if (underscore && g_ascii_isalpha (underscore[1]) && g_ascii_isalpha (underscore[2]))
    return g_ascii_strup (underscore + 1, 2);
  return g_strdup ("US");
}

int
cal_alert_actions (gboolean map, gboolean call, gboolean email, gboolean snooze,
                   CalAlertAction actions[CAL_ALERT_MAX_ACTIONS])
{
  const gboolean applies[] = { map, call, email, snooze };
  int n = 0;

  for (int i = 0; i < (int) G_N_ELEMENTS (applies) && n < CAL_ALERT_MAX_ACTIONS; i++)
    if (applies[i])
      actions[n++] = (CalAlertAction) i;
  return n;
}

static char *
when_date (int day, gboolean weekday, int this_year)
{
  g_autoptr(GDateTime) time = NULL;
  g_autofree char *date = NULL;
  GDate d;

  g_date_clear (&d, 1);
  g_date_set_julian (&d, day);
  date = g_date_get_year (&d) == this_year ? cal_format_month_day (day) : cal_format_full_date (day);
  if (!weekday)
    return g_steal_pointer (&date);
  {
    g_autofree char *name = cal_format_weekday (day);

    /* TRANSLATORS: the weekday and the date, "Sunday, November 8" */
    return g_strdup_printf (C_("alert", "%s, %s"), name, date);
  }
}

char *
cal_alert_when (gint64 start, gint64 end, gboolean all_day, gint64 now)
{
  GDate today;
  int this_year, first = cal_day_number (start), last;
  g_autofree char *from = NULL, *to = NULL, *text = NULL;

  g_date_clear (&today, 1);
  g_date_set_julian (&today, cal_day_number (now));
  this_year = g_date_get_year (&today);
  if (all_day)
    {
      /* the end is exclusive: the day before it is the last */
      last = MAX (first, cal_day_number (MAX (end, start + 1) - 1));
      from = when_date (first, TRUE, this_year);
      if (last == first)
        return g_steal_pointer (&from);
      to = when_date (last, TRUE, this_year);
      return g_strdup_printf ("%s – %s", from, to);
    }
  last = cal_day_number (MAX (end, start));
  from = when_date (first, FALSE, this_year);
  if (last == first)
    {
      g_autofree char *times = cal_format_time_range (start, end);

      /* TRANSLATORS: a date and a time range, "November 8, 10:00 – 11:00" */
      text = g_strdup_printf (C_("alert", "%s, %s"), from, times);
    }
  else
    {
      g_autofree char *t0 = cal_format_time (start), *t1 = cal_format_time (end);

      to = when_date (last, FALSE, this_year);
      text = g_strdup_printf ("%s, %s – %s, %s", from, t0, to, t1);
    }
  /* shown in the home time zone (not the phone's): its short name after it */
  {
    const char *tz = g_getenv ("TZ");

    if (tz && *tz && g_strcmp0 (tz, cal_phone_zone ()) != 0)
      {
        g_autoptr(GDateTime) at = g_date_time_new_from_unix_local (start);

        return g_strdup_printf ("%s %s", text, g_date_time_get_timezone_abbreviation (at));
      }
  }
  return g_steal_pointer (&text);
}

/* --- sound and vibration */

gboolean
cal_alerts_can_vibrate (void)
{
  static int found = -1;
  g_autoptr(GDir) dir = NULL;
  const char *name;

  if (found >= 0)
    return found;
  found = FALSE;
  dir = g_dir_open ("/sys/class/input", 0, NULL);
  while (dir && !found && (name = g_dir_read_name (dir)))
    {
      g_autofree char *path = g_build_filename ("/sys/class/input", name, "capabilities/ff", NULL);
      g_autofree char *ff = NULL;

      found = g_str_has_prefix (name, "input") && g_file_get_contents (path, &ff, NULL, NULL)
              && !g_str_equal (g_strstrip (ff), "0");
    }
  return found;
}

void
cal_sound_free (CalSound *sound)
{
  g_free (sound->name);
  g_free (sound->path);
  g_free (sound);
}

/* "message-new-instant.oga" -> "Message new instant" */
static char *
sound_name (const char *file)
{
  const char *dot = strrchr (file, '.');
  char *name = g_strndup (file, dot ? (gsize) (dot - file) : strlen (file));

  g_strdelimit (name, "-_", ' ');
  if (*name)
    name[0] = g_ascii_toupper (name[0]);
  return name;
}

static int
compare_sounds (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate ((*(CalSound * const *) a)->name, (*(CalSound * const *) b)->name);
}

GPtrArray *
cal_alerts_sounds (void)
{
  GPtrArray *sounds = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_sound_free);
  g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *const *dirs = g_get_system_data_dirs ();
  g_autoptr(GPtrArray) all = g_ptr_array_new ();

  g_ptr_array_add (all, (gpointer) g_get_user_data_dir ());
  for (int i = 0; dirs[i]; i++)
    g_ptr_array_add (all, (gpointer) dirs[i]);
  for (guint i = 0; i < all->len; i++)
    {
      g_autofree char *base = g_build_filename (all->pdata[i], "sounds", NULL);
      g_autoptr(GDir) themes = g_dir_open (base, 0, NULL);
      const char *theme;

      while (themes && (theme = g_dir_read_name (themes)))
        {
          g_autofree char *stereo = g_build_filename (base, theme, "stereo", NULL);
          g_autoptr(GDir) files = g_dir_open (stereo, 0, NULL);
          const char *file;

          while (files && (file = g_dir_read_name (files)))
            {
              CalSound *sound;

              if (!g_str_has_suffix (file, ".oga") && !g_str_has_suffix (file, ".ogg")
                  && !g_str_has_suffix (file, ".wav"))
                continue;
              sound = g_new0 (CalSound, 1);
              sound->name = sound_name (file);
              if (g_hash_table_contains (seen, sound->name))
                {
                  cal_sound_free (sound);
                  continue;
                }
              sound->path = g_build_filename (stereo, file, NULL);
              g_hash_table_add (seen, g_strdup (sound->name));
              g_ptr_array_add (sounds, sound);
            }
        }
    }
  g_ptr_array_sort (sounds, compare_sounds);
  return sounds;
}

char *
cal_alerts_sound_label (const char *value)
{
  if (!value || !*value)
    return g_strdup (_("None"));
  if (g_str_equal (value, "default"))
    return g_strdup (_("Default notification sound"));
  {
    g_autofree char *base = g_path_get_basename (value);

    return sound_name (base);
  }
}
