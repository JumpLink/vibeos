/* dictate-tools-phone: the tools themselves (dictate-tools-private.h). Each runs on a thread of its own with its own
 * main context, so everything here is synchronous: D-Bus calls with a timeout, EDS, SQLite opened read-only.
 *
 * What they read comes from where the phone's own apps keep it: Contacts and Calendar in Evolution Data Server, the
 * call log in the Dialer's calls.db, the messages in Messaging's bugle_db, alarms and timers from the Clock's
 * service, Wi-Fi from NetworkManager, Bluetooth from BlueZ, the battery from UPower, the open apps from the shell.
 * They return few fields: a name and the kind of number rather than the number, a message's text only when asked
 * for, at most a few rows. Nothing here logs what it read. */
#define _GNU_SOURCE             /* strcasestr */
#include "dictate-calls.h"
#include "dictate-tools-private.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <libebook/libebook.h>
#include <libecal/libecal.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#define DBUS_TIMEOUT_MS 4000
/* EDS's wait for a book or calendar to go online: none, its cache answers at once. EDS reads (guint32) -1 as no wait
 * and 0 as no limit: a local book never says "connected", so 5 held each lookup 5 s and 0 forever (eds-try, 11-23) */
#define EDS_NO_WAIT     ((guint32) -1)
#define ROWS_MAX        10
#define PEOPLE_MAX      5

/* ---------------- helpers ---------------- */

static JsonNode *
finish (JsonBuilder *b)
{
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

static void
add_string (JsonBuilder *b, const char *name, const char *value)
{
  json_builder_set_member_name (b, name);
  json_builder_add_string_value (b, value);
}

static void
add_int (JsonBuilder *b, const char *name, gint64 value)
{
  json_builder_set_member_name (b, name);
  json_builder_add_int_value (b, value);
}

static void
add_bool (JsonBuilder *b, const char *name, gboolean value)
{
  json_builder_set_member_name (b, name);
  json_builder_add_boolean_value (b, value);
}

static void
add_double1 (JsonBuilder *b, const char *name, double value)
{
  json_builder_set_member_name (b, name);
  json_builder_add_double_value (b, ((gint64) (value * 10 + 0.5)) / 10.0);
}

/* "1 h 20 min", "3 min", "45 s" */
static char *
duration_text (gint64 seconds)
{
  if (seconds >= 3600)
    return seconds % 3600 >= 60 ? g_strdup_printf ("%" G_GINT64_FORMAT " h %" G_GINT64_FORMAT " min", seconds / 3600,
                                                   seconds % 3600 / 60)
                                : g_strdup_printf ("%" G_GINT64_FORMAT " h", seconds / 3600);
  if (seconds >= 60)
    return g_strdup_printf ("%" G_GINT64_FORMAT " min", (seconds + 30) / 60);
  return g_strdup_printf ("%" G_GINT64_FORMAT " s", seconds);
}

static GVariant *
dbus_call (GBusType type, const char *name, const char *path, const char *interface, const char *method,
           GVariant *parameters, const char *reply, ToolRun *run, GError **error)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (type, tool_run_cancellable (run), error);

  if (!bus)
    {
      if (parameters)
        g_variant_unref (g_variant_ref_sink (parameters));
      return NULL;
    }
  return g_dbus_connection_call_sync (bus, name, path, interface, method, parameters,
                                      reply ? G_VARIANT_TYPE (reply) : NULL, G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS,
                                      tool_run_cancellable (run), error);
}

/* a D-Bus object's properties, a{sv} */
static GVariant *
dbus_properties (GBusType type, const char *name, const char *path, const char *interface, ToolRun *run,
                 GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (type, name, path, "org.freedesktop.DBus.Properties", "GetAll",
                                          g_variant_new ("(s)", interface), "(a{sv})", run, error);

  return reply ? g_variant_get_child_value (reply, 0) : NULL;
}

static gboolean
dbus_set (GBusType type, const char *name, const char *path, const char *interface, const char *property,
          GVariant *value, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (type, name, path, "org.freedesktop.DBus.Properties", "Set",
                                          g_variant_new ("(ssv)", interface, property, value), NULL, run, error);
  return reply != NULL;
}

/* a name for the model from a D-Bus error, without its D-Bus prefix */
static const char *
plain (GError *error)
{
  g_dbus_error_strip_remote_error (error);
  return error->message;
}

/* only the digits; the last 9 of two numbers being the same makes them the same number ("+49 176 1234567" and
 * "0176 1234567"), shorter ones must match whole */
static char *
digits_of (const char *number)
{
  GString *d = g_string_new (NULL);

  for (const char *p = number; p && *p; p++)
    if (g_ascii_isdigit (*p))
      g_string_append_c (d, *p);
  return g_string_free (d, FALSE);
}

static gboolean
same_number (const char *digits_a, const char *digits_b)
{
  gsize a = strlen (digits_a), b = strlen (digits_b);

  if (a < 3 || b < 3)
    return FALSE;
  if (a >= 9 && b >= 9)
    return !strcmp (digits_a + a - 9, digits_b + b - 9);
  return !strcmp (digits_a, digits_b);
}

/* whether the model gave a number rather than a name */
static gboolean
looks_like_number (const char *s)
{
  guint digits = 0;

  for (const char *p = s; *p; p++)
    if (g_ascii_isdigit (*p))
      digits++;
    else if (!strchr ("+-() /.", *p))
      return FALSE;
  return digits >= 3;
}

/* ---------------- people (Evolution Data Server) ---------------- */

typedef struct {
  char *type;                   /* "mobile", "home", "work", "fax", "other" */
  char *number;
  char *digits;
} Tel;

typedef struct {
  char *name;
  char *nickname;               /* "Mama", or NULL: what the user may call them */
  GPtrArray *tels;              /* Tel */
  GPtrArray *emails;            /* char * */
  char *note;                   /* the contact's note, or NULL: what the user wrote, or whoever sent the card */
} Person;

static void
tel_free (Tel *tel)
{
  g_free (tel->type);
  g_free (tel->number);
  g_free (tel->digits);
  g_free (tel);
}

static void
person_free (Person *p)
{
  g_free (p->name);
  g_free (p->nickname);
  g_ptr_array_unref (p->tels);
  g_ptr_array_unref (p->emails);
  g_free (p->note);
  g_free (p);
}

static ESourceRegistry *
registry (ToolRun *run, GError **error)
{
  static GMutex lock;
  static ESourceRegistry *cached;
  ESourceRegistry *r;

  /* one for all calls: it keeps its own thread for EDS's signals */
  g_mutex_lock (&lock);
  if (!cached)
    cached = e_source_registry_new_sync (tool_run_cancellable (run), error);
  r = cached ? g_object_ref (cached) : NULL;
  g_mutex_unlock (&lock);
  return r;
}

static const char *
tel_type (EVCardAttribute *attr)
{
  GList *types = e_vcard_attribute_get_param (attr, EVC_TYPE);
  gboolean home = FALSE, work = FALSE;

  for (GList *l = types; l; l = l->next)
    {
      if (!g_ascii_strcasecmp (l->data, "CELL"))
        return "mobile";
      if (!g_ascii_strcasecmp (l->data, "FAX"))
        return "fax";
      home = home || !g_ascii_strcasecmp (l->data, "HOME");
      work = work || !g_ascii_strcasecmp (l->data, "WORK");
    }
  return work ? "work" : home ? "home" : "other";
}

static Person *
person_from_contact (EContact *contact)
{
  Person *p = g_new0 (Person, 1);
  const char *name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
  GList *attrs;

  if (!name || !*name)
    name = e_contact_get_const (contact, E_CONTACT_NICKNAME);
  if (!name || !*name)
    name = e_contact_get_const (contact, E_CONTACT_ORG);
  p->name = g_strdup (name && *name ? name : "(no name)");
  {
    const char *nickname = e_contact_get_const (contact, E_CONTACT_NICKNAME);
    if (nickname && *nickname && g_strcmp0 (nickname, p->name))
      p->nickname = g_strdup (nickname);
  }
  p->tels = g_ptr_array_new_with_free_func ((GDestroyNotify) tel_free);
  p->emails = g_ptr_array_new_with_free_func (g_free);
  attrs = e_contact_get_attributes (contact, E_CONTACT_TEL);
  for (GList *l = attrs; l; l = l->next)
    {
      g_autofree char *value = e_vcard_attribute_get_value (l->data);
      Tel *tel;

      if (!value || !*value)
        continue;
      tel = g_new0 (Tel, 1);
      tel->type = g_strdup (tel_type (l->data));
      tel->number = g_strdup (value);
      tel->digits = digits_of (value);
      g_ptr_array_add (p->tels, tel);
    }
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
  attrs = e_contact_get_attributes (contact, E_CONTACT_EMAIL);
  for (GList *l = attrs; l; l = l->next)
    {
      char *value = e_vcard_attribute_get_value (l->data);
      if (value && *value)
        g_ptr_array_add (p->emails, value);
      else
        g_free (value);
    }
  g_list_free_full (attrs, (GDestroyNotify) e_vcard_attribute_free);
  {
    const char *note = e_contact_get_const (contact, E_CONTACT_NOTE);
    if (note && *note)
      p->note = g_strdup (note);
  }
  return p;
}

/* the contacts matching `query` in every address book that is on, or NULL with the error */
static GPtrArray *
people (EBookQuery *query, ToolRun *run, GError **error)
{
  g_autoptr (ESourceRegistry) r = registry (run, error);
  g_autofree char *sexp = e_book_query_to_string (query);
  GPtrArray *found;
  GList *sources;

  if (!r)
    return NULL;
  found = g_ptr_array_new_with_free_func ((GDestroyNotify) person_free);
  sources = e_source_registry_list_enabled (r, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  for (GList *l = sources; l; l = l->next)
    {
      g_autoptr (GError) failure = NULL;
      g_autoptr (EClient) client = e_book_client_connect_sync (l->data, EDS_NO_WAIT, tool_run_cancellable (run),
                                                               &failure);
      GSList *contacts = NULL;

      if (!client || !e_book_client_get_contacts_sync (E_BOOK_CLIENT (client), sexp, &contacts,
                                                       tool_run_cancellable (run), &failure))
        {
          /* one address book that does not answer (an account offline) does not stop the others */
          g_debug ("tools: an address book: %s", failure ? failure->message : "?");
          continue;
        }
      for (GSList *c = contacts; c; c = c->next)
        g_ptr_array_add (found, person_from_contact (c->data));
      g_slist_free_full (contacts, g_object_unref);
    }
  g_list_free_full (sources, g_object_unref);
  return found;
}

static GPtrArray *
people_named (const char *name, ToolRun *run, GError **error)
{
  EBookQuery *q[4] = {
    e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, name),
    e_book_query_field_test (E_CONTACT_NICKNAME, E_BOOK_QUERY_CONTAINS, name),
    e_book_query_field_test (E_CONTACT_GIVEN_NAME, E_BOOK_QUERY_CONTAINS, name),
    e_book_query_field_test (E_CONTACT_FAMILY_NAME, E_BOOK_QUERY_CONTAINS, name),
  };
  EBookQuery *any = e_book_query_or (4, q, TRUE);
  GPtrArray *found = people (any, run, error);
  g_auto (GStrv) words = NULL;

  e_book_query_unref (any);
  if (!found || found->len)
    return found;
  /* "Anna from work", "Mama Test": every word of it in the full name */
  words = g_strsplit_set (name, " ,", -1);
  if (g_strv_length (words) > 1)
    {
      g_autoptr (GPtrArray) parts = g_ptr_array_new ();

      for (guint i = 0; words[i]; i++)
        if (strlen (words[i]) >= 3)
          g_ptr_array_add (parts, e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, words[i]));
      if (parts->len > 1)
        {
          EBookQuery *all = e_book_query_and (parts->len, (EBookQuery **) parts->pdata, TRUE);
          g_ptr_array_unref (found);
          found = people (all, run, error);
          e_book_query_unref (all);
        }
      else
        for (guint i = 0; i < parts->len; i++)
          e_book_query_unref (parts->pdata[i]);
    }
  return found;
}

/* the contacts with a number, to put names to the numbers of calls and messages; NULL when Contacts is off */
static GPtrArray *
people_with_numbers (ToolRun *run)
{
  g_autoptr (GError) error = NULL;
  EBookQuery *query;
  GPtrArray *found;

  if (!tool_run_area_on (run, "access-contacts"))
    return NULL;
  query = e_book_query_field_exists (E_CONTACT_TEL);
  found = people (query, run, &error);
  e_book_query_unref (query);
  if (!found)
    g_debug ("tools: no names for numbers: %s", error->message);
  return found;
}

/* the contact with that number, and the kind of number it is to them */
static Person *
person_for (GPtrArray *all, const char *number, const char **type)
{
  g_autofree char *digits = digits_of (number);

  for (guint i = 0; all && i < all->len; i++)
    {
      Person *p = all->pdata[i];
      for (guint j = 0; j < p->tels->len; j++)
        {
          Tel *tel = p->tels->pdata[j];
          if (same_number (tel->digits, digits))
            {
              if (type)
                *type = tel->type;
              return p;
            }
        }
    }
  return NULL;
}

/* the one contact meant by `name`: the only one found, or the one whose name is exactly that; else NULL with an
 * error that tells the model what to ask */
static Person *
one_person (GPtrArray *found, const char *name, GError **error)
{
  Person *exact = NULL;

  for (guint i = 0; i < found->len; i++)
    {
      Person *p = found->pdata[i];
      if (!g_utf8_collate (p->name, name) || !g_ascii_strcasecmp (p->name, name))
        exact = exact ? exact : p;
    }
  if (found->len == 1)
    return found->pdata[0];
  if (exact)
    return exact;
  if (found->len == 0)
    {
      tool_error (error, "There is no contact called \"%s\". Ask the user who they mean, or for the number.", name);
      return NULL;
    }
  {
    GString *names = g_string_new (NULL);
    for (guint i = 0; i < found->len && i < PEOPLE_MAX; i++)
      g_string_append_printf (names, "%s%s", i ? ", " : "", ((Person *) found->pdata[i])->name);
    tool_error (error, "Several contacts match \"%s\": %s. Ask the user which one.", name, names->str);
    g_string_free (names, TRUE);
  }
  return NULL;
}

/* the person's number of that type, or the mobile one, or the first */
static Tel *
pick_tel (Person *p, const char *type)
{
  Tel *mobile = NULL;

  for (guint i = 0; i < p->tels->len; i++)
    {
      Tel *tel = p->tels->pdata[i];
      if (type && !g_strcmp0 (tel->type, type))
        return tel;
      if (!mobile && !g_strcmp0 (tel->type, "mobile"))
        mobile = tel;
    }
  return mobile ? mobile : p->tels->len ? p->tels->pdata[0] : NULL;
}

/* ---------------- the time, the battery, the storage ---------------- */

static JsonNode *
get_time (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GTimeZone) zone = g_time_zone_new_local ();
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *time = g_date_time_format (now, "%H:%M");
  g_autofree char *date = g_date_time_format (now, "%Y-%m-%d");
  g_autofree char *offset = g_date_time_format (now, "%:z");
  static const char *const days[] = { NULL, "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
                                      "Sunday" };
  const char *id = g_time_zone_get_identifier (zone);

  json_builder_begin_object (b);
  add_string (b, "time", time);
  add_string (b, "date", date);
  add_string (b, "weekday", days[g_date_time_get_day_of_week (now)]);
  /* "Europe/Berlin"; a zone read from a file has no such name */
  add_string (b, "time_zone", id && *id != '/' && *id != ':' ? id : g_date_time_get_timezone_abbreviation (now));
  add_string (b, "utc_offset", offset);
  return finish (b);
}

static JsonNode *
get_battery (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) props = dbus_properties (G_BUS_TYPE_SYSTEM, "org.freedesktop.UPower",
                                                "/org/freedesktop/UPower/devices/DisplayDevice",
                                                "org.freedesktop.UPower.Device", run, error);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  double percent = 0;
  guint32 state = 0;
  gint64 to_empty = 0, to_full = 0;
  gboolean present = FALSE;

  if (!props)
    return NULL;
  g_variant_lookup (props, "Percentage", "d", &percent);
  g_variant_lookup (props, "State", "u", &state);
  g_variant_lookup (props, "TimeToEmpty", "x", &to_empty);
  g_variant_lookup (props, "TimeToFull", "x", &to_full);
  g_variant_lookup (props, "IsPresent", "b", &present);
  if (!present)
    {
      tool_error (error, "This device has no battery.");
      return NULL;
    }
  json_builder_begin_object (b);
  add_int (b, "percent", (gint64) (percent + 0.5));
  /* UPower: 1 charging, 2 discharging, 4 full, 5 pending charge */
  add_bool (b, "charging", state == 1);
  if (state == 4)
    add_bool (b, "full", TRUE);
  if (state == 1 && to_full > 0)
    {
      g_autofree char *t = duration_text (to_full);
      add_string (b, "time_to_full", t);
    }
  else if (state == 2 && to_empty > 0)
    {
      g_autofree char *t = duration_text (to_empty);
      add_string (b, "time_left", t);
    }
  return finish (b);
}

static void
add_space (JsonBuilder *b, const char *name, struct statvfs *s)
{
  double block = s->f_frsize ? s->f_frsize : s->f_bsize;
  double total = s->f_blocks * block / 1e9, free = s->f_bavail * block / 1e9;

  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  add_double1 (b, "free_gb", free);
  add_double1 (b, "total_gb", total);
  add_int (b, "used_percent", total > 0 ? (gint64) ((total - free) * 100 / total + 0.5) : 0);
  json_builder_end_object (b);
}

static JsonNode *
get_storage (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  struct statvfs home, root;
  struct stat home_st, root_st;
  const char *dir = g_get_home_dir ();

  if (statvfs (dir, &home) != 0 || statvfs ("/", &root) != 0 || stat (dir, &home_st) != 0
      || stat ("/", &root_st) != 0)
    {
      tool_error (error, "The storage could not be read.");
      return NULL;
    }
  json_builder_begin_object (b);
  if (home_st.st_dev == root_st.st_dev)
    add_space (b, "phone", &home);
  else
    {
      /* the user's files on a partition of their own */
      add_space (b, "user_files", &home);
      add_space (b, "system", &root);
    }
  return finish (b);
}

/* ---------------- contacts ---------------- */

static JsonNode *
find_contacts (JsonObject *args, ToolRun *run, GError **error)
{
  const char *name = tool_arg_string (args, "name");
  gboolean numbers = tool_arg_bool (args, "include_numbers", FALSE);
  gboolean emails = tool_arg_bool (args, "include_emails", FALSE);
  gboolean notes = tool_arg_bool (args, "include_note", FALSE);
  g_autoptr (GPtrArray) found = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();

  if (!name)
    {
      tool_error (error, "Whom to look for is missing: ask the user.");
      return NULL;
    }
  if (!(found = people_named (name, run, error)))
    return NULL;
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "contacts");
  json_builder_begin_array (b);
  for (guint i = 0; i < found->len && i < PEOPLE_MAX; i++)
    {
      Person *p = found->pdata[i];

      json_builder_begin_object (b);
      add_string (b, "name", p->name);
      if (p->nickname)
        add_string (b, "nickname", p->nickname);
      json_builder_set_member_name (b, numbers ? "numbers" : "number_kinds");
      json_builder_begin_array (b);
      for (guint j = 0; j < p->tels->len; j++)
        {
          Tel *tel = p->tels->pdata[j];
          if (!numbers)
            {
              json_builder_add_string_value (b, tel->type);
              continue;
            }
          json_builder_begin_object (b);
          add_string (b, "kind", tel->type);
          add_string (b, "number", tel->number);
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
      if (emails)
        {
          json_builder_set_member_name (b, "emails");
          json_builder_begin_array (b);
          for (guint j = 0; j < p->emails->len; j++)
            json_builder_add_string_value (b, p->emails->pdata[j]);
          json_builder_end_array (b);
        }
      else if (p->emails->len)
        add_bool (b, "has_email", TRUE);
      if (p->note && notes)
        add_string (b, "note", p->note);
      else if (p->note)
        add_bool (b, "has_note", TRUE);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  if (found->len > PEOPLE_MAX)
    add_int (b, "more_not_shown", found->len - PEOPLE_MAX);
  if (!found->len)
    add_string (b, "note", "No contact has that name.");
  return finish (b);
}

/* ---------------- calls ---------------- */

static sqlite3 *
open_store (const char *app, const char *file, GError **error)
{
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), app, file, NULL);
  sqlite3 *db = NULL;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return NULL;
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
      tool_error (error, "It could not be read.");
      g_debug ("tools: %s: %s", file, db ? sqlite3_errmsg (db) : "?");
      sqlite3_close (db);
      return NULL;
    }
  sqlite3_busy_timeout (db, 2000);
  return db;
}

static const char *
call_direction (int type)
{
  switch (type)
    {
    case 1: return "incoming";
    case 2: return "outgoing";
    case 3: return "missed";
    case 4: return "voicemail";
    case 5: return "rejected";
    case 6: return "blocked";
    case 7: return "answered on another device";
    default: return "unknown";
    }
}

static JsonNode *
get_recent_calls (JsonObject *args, ToolRun *run, GError **error)
{
  gint64 count = CLAMP (tool_arg_int (args, "count", 5), 1, ROWS_MAX);
  gboolean missed_only = tool_arg_bool (args, "missed_only", FALSE);
  const char *contact = tool_arg_string (args, "contact");
  g_autoptr (GPtrArray) all = people_with_numbers (run);
  g_autoptr (GPtrArray) wanted = NULL;          /* the digits of the numbers of `contact` */
  g_autoptr (JsonBuilder) b = json_builder_new ();
  GError *failure = NULL;
  sqlite3 *db = open_store ("vibeos-dialer", "calls.db", &failure);
  sqlite3_stmt *rows = NULL;
  guint n = 0;

  if (failure)
    {
      g_propagate_error (error, failure);
      return NULL;
    }
  if (contact)
    {
      wanted = g_ptr_array_new_with_free_func (g_free);
      if (looks_like_number (contact))
        g_ptr_array_add (wanted, digits_of (contact));
      else if (!all)
        {
          sqlite3_close (db);
          tool_error (error, "Names cannot be looked up: Contacts is turned off in Agent's settings.");
          return NULL;
        }
      else
        for (guint i = 0; i < all->len; i++)
          {
            Person *p = all->pdata[i];
            /* by name or nickname: "Mama" is often what someone is called, and saved as */
            if (!strcasestr (p->name, contact) && !(p->nickname && strcasestr (p->nickname, contact)))
              continue;
            for (guint j = 0; j < p->tels->len; j++)
              g_ptr_array_add (wanted, g_strdup (((Tel *) p->tels->pdata[j])->digits));
          }
      if (!wanted->len)
        {
          sqlite3_close (db);
          tool_error (error, "There is no contact called \"%s\".", contact);
          return NULL;
        }
    }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "calls");
  json_builder_begin_array (b);
  if (db && sqlite3_prepare_v2 (db, "SELECT number, date, duration, type, presentation FROM calls"
                                    " ORDER BY date DESC LIMIT 500", -1, &rows, NULL) == SQLITE_OK)
    while (n < count && sqlite3_step (rows) == SQLITE_ROW)
      {
        const char *number = (const char *) sqlite3_column_text (rows, 0);
        gint64 date = sqlite3_column_int64 (rows, 1), duration = sqlite3_column_int64 (rows, 2);
        int type = sqlite3_column_int (rows, 3), presentation = sqlite3_column_int (rows, 4);
        const char *number_type = NULL;
        Person *p;
        g_autofree char *when = NULL;
        g_autofree char *digits = digits_of (number);

        if (missed_only && type != 3)
          continue;
        if (wanted)
          {
            gboolean match = FALSE;
            for (guint i = 0; i < wanted->len && !match; i++)
              match = same_number (wanted->pdata[i], digits);
            if (!match)
              continue;
          }
        p = number && *number ? person_for (all, number, &number_type) : NULL;
        json_builder_begin_object (b);
        if (p)
          {
            add_string (b, "who", p->name);
            if (p->nickname)
              add_string (b, "nickname", p->nickname);
            add_string (b, "number_type", number_type);
          }
        else if (number && *number && presentation == 1)
          {
            /* no name to give: the number is what there is */
            add_string (b, "who", "a number not in the contacts");
            add_string (b, "number", number);
          }
        else
          add_string (b, "who", "a hidden number");
        add_string (b, "direction", call_direction (type));
        when = tool_when (date, TRUE);
        add_string (b, "when", when);
        if (duration > 0)
          {
            g_autofree char *d = duration_text (duration);
            add_string (b, "duration", d);
          }
        json_builder_end_object (b);
        n++;
      }
  sqlite3_finalize (rows);
  sqlite3_close (db);
  json_builder_end_array (b);
  if (!n)
    add_string (b, "note", missed_only ? "No missed calls." : "No calls.");
  return finish (b);
}

static gboolean
place_call (const char *number, const char *unused, GError **error)
{
  /* the Dialer's "call" action: it opens its in-call screen and dials */
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  g_autoptr (GVariant) reply = NULL;
  GVariantBuilder args;

  if (!bus)
    return FALSE;
  g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args, "v", g_variant_new_string (number));
  reply = g_dbus_connection_call_sync (bus, "vibeos.Dialer", "/vibeos/Dialer", "org.freedesktop.Application",
                                       "ActivateAction", g_variant_new ("(sava{sv})", "call", &args, NULL), NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 20000, NULL, error);
  return reply != NULL;
}

static gboolean
send_message (const char *destination, const char *text, GError **error)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  g_autoptr (GVariant) conversation = NULL, sent = NULL;
  const char *destinations[] = { destination, NULL };
  gint64 id;

  if (!bus)
    return FALSE;
  /* the service puts the number in its canonical form */
  conversation = g_dbus_connection_call_sync (bus, "vibeos.Messaging.Sms", "/vibeos/Messaging/Sms",
                                              "vibeos.Messaging.Sms", "GetConversation",
                                              g_variant_new ("(^as)", destinations), G_VARIANT_TYPE ("(x)"),
                                              G_DBUS_CALL_FLAGS_NONE, 20000, NULL, error);
  if (!conversation)
    return FALSE;
  g_variant_get (conversation, "(x)", &id);
  if (id <= 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("That number cannot get messages"));
      return FALSE;
    }
  sent = g_dbus_connection_call_sync (bus, "vibeos.Messaging.Sms", "/vibeos/Messaging/Sms", "vibeos.Messaging.Sms",
                                      "Send", g_variant_new ("(xs)", id, text), G_VARIANT_TYPE ("(x)"),
                                      G_DBUS_CALL_FLAGS_NONE, 20000, NULL, error);
  return sent != NULL;
}

/* whom `to` means: a contact's number (of `type`, else the mobile one) or the number given. Fills the card's line
 * ("Anna Test, mobile") and the model's name for them */
static char *
resolve_recipient (const char *to, const char *type, ToolRun *run, char **line, char **named, GError **error)
{
  g_autoptr (GPtrArray) found = NULL;
  Person *p;
  Tel *tel;

  if (looks_like_number (to))
    {
      *line = g_strdup (to);
      *named = g_strdup (to);
      return g_strdup (to);
    }
  if (!tool_run_area_on (run, "access-contacts"))
    {
      tool_error (error, "No access to the contacts (turned off in Agent's settings), so \"%s\" cannot be looked up. "
                  "Ask the user for the number.", to);
      return NULL;
    }
  found = people_named (to, run, error);
  if (!found || !(p = one_person (found, to, error)))
    return NULL;
  tel = pick_tel (p, type);
  if (!tel)
    {
      tool_error (error, "%s has no phone number in the contacts.", p->name);
      return NULL;
    }
  if (type && g_strcmp0 (tel->type, type))
    {
      tool_error (error, "%s has no %s number. Their numbers are of these kinds: %s%s%s.", p->name, type,
                  ((Tel *) p->tels->pdata[0])->type, p->tels->len > 1 ? ", " : "",
                  p->tels->len > 1 ? ((Tel *) p->tels->pdata[1])->type : "");
      return NULL;
    }
  /* the card shows the number too: the user sees exactly where it goes */
  *line = g_strdup_printf ("%s, %s · %s", p->name, _(tel->type), tel->number);
  *named = g_strdup_printf ("%s (%s)", p->name, tel->type);
  return g_strdup (tel->number);
}

static JsonNode *
propose_call (JsonObject *args, ToolRun *run, GError **error)
{
  const char *to = tool_arg_string (args, "to"), *type = tool_arg_string (args, "number_type");
  g_autofree char *line = NULL, *named = NULL, *number = NULL, *about = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (DictateAction) action = NULL;

  if (!to)
    {
      tool_error (error, "Whom to call is missing: ask the user.");
      return NULL;
    }
  if (!(number = resolve_recipient (to, type, run, &line, &named, error)))
    return NULL;
  {
    const char *lines[] = { line, NULL };
    about = g_strdup_printf ("the call to %s", named);
    action = dictate_action_new ("place a call", _("Call"), lines, _("Call"), _("Calling…"), about, place_call,
                                 number, NULL);
    dictate_action_set_log (action, "propose_call", "a phone call, over the mobile network");
  }
  tool_run_propose (run, action);
  json_builder_begin_object (b);
  add_string (b, "shown", "A card on the screen to call this person, with a Call button.");
  add_string (b, "to", named);
  add_string (b, "status", "Not called yet: the call starts only when the user taps Call.");
  return finish (b);
}

/* ---------------- messages ---------------- */

typedef struct {
  gint64 id;
  char *destination;            /* a 1:1 conversation's other participant, or NULL */
  char *name;                   /* as stored: the destinations joined */
} Conversation;

static const char *
conversation_who (Conversation *c, GPtrArray *all, char **owned)
{
  Person *p = c->destination ? person_for (all, c->destination, NULL) : NULL;

  if (p)
    return p->name;
  if (c->destination)
    return c->destination;
  *owned = g_strdup_printf ("a group (%s)", c->name ? c->name : "");
  return *owned;
}

static JsonNode *
get_messages (JsonObject *args, ToolRun *run, GError **error)
{
  const char *contact = tool_arg_string (args, "contact");
  gboolean with_text = tool_arg_bool (args, "include_text", FALSE) && !tool_run_private (run);
  gboolean unread_only = tool_arg_bool (args, "unread_only", FALSE);
  gint64 count = CLAMP (tool_arg_int (args, "count", contact ? 3 : 5), 1, ROWS_MAX);
  g_autoptr (GPtrArray) all = people_with_numbers (run);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  GError *failure = NULL;
  sqlite3 *db = open_store ("vibeos-messaging", "bugle_db", &failure);
  sqlite3_stmt *rows = NULL;
  guint n = 0;

  if (failure)
    {
      g_propagate_error (error, failure);
      return NULL;
    }
  json_builder_begin_object (b);
  if (!with_text && tool_arg_bool (args, "include_text", FALSE))
    add_string (b, "private_mode", "What they say is not given: private mode is on and the answer plays out loud on "
                                   "the speaker. Say that the user can hear it with headphones on.");
  if (!contact)
    {
      /* the latest conversations */
      json_builder_set_member_name (b, "conversations");
      json_builder_begin_array (b);
      if (db && sqlite3_prepare_v2 (db,
            "SELECT c._id, c.participant_normalized_destination, c.name, c.sort_timestamp, c.snippet_text,"
            " (SELECT COUNT(*) FROM messages m WHERE m.conversation_id = c._id AND m.read = 0"
            "   AND m.message_status >= 100),"
            " (SELECT message_status FROM messages WHERE _id = c.latest_message_id)"
            " FROM conversations c WHERE c.archive_status = 0 AND c.latest_message_id IS NOT NULL"
            " ORDER BY c.sort_timestamp DESC LIMIT 100", -1, &rows, NULL) == SQLITE_OK)
        while (n < count && sqlite3_step (rows) == SQLITE_ROW)
          {
            Conversation c = { sqlite3_column_int64 (rows, 0), (char *) sqlite3_column_text (rows, 1),
                               (char *) sqlite3_column_text (rows, 2) };
            g_autofree char *owned = NULL, *when = NULL;
            int unread = sqlite3_column_int (rows, 5), status = sqlite3_column_int (rows, 6);

            if (unread_only && !unread)
              continue;
            json_builder_begin_object (b);
            add_string (b, "with", conversation_who (&c, all, &owned));
            when = tool_when (sqlite3_column_int64 (rows, 3), TRUE);
            add_string (b, "last_message", when);
            add_string (b, "last_from", status >= 100 ? "them" : "the user");
            add_int (b, "unread", unread);
            if (with_text && sqlite3_column_text (rows, 4))
              add_string (b, "last_text", (const char *) sqlite3_column_text (rows, 4));
            json_builder_end_object (b);
            n++;
          }
      sqlite3_finalize (rows);
      json_builder_end_array (b);
      if (!n)
        add_string (b, "note", unread_only ? "No unread messages." : "No messages.");
      sqlite3_close (db);
      return finish (b);
    }

  /* one person's: their conversation's latest messages */
  {
    g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
    g_autoptr (GPtrArray) wanted = g_ptr_array_new_with_free_func (g_free);
    g_autofree char *name_found = NULL;   /* the contact's name: a copy, `found` goes before it is used */
    const char *who = contact;
    gboolean by_name = FALSE;

    if (looks_like_number (contact))
      g_ptr_array_add (wanted, digits_of (contact));
    else if (all)
      {
        g_autoptr (GPtrArray) found = people_named (contact, run, NULL);
        Person *p = found ? one_person (found, contact, NULL) : NULL;

        if (found && found->len > 1 && !p)
          {
            sqlite3_close (db);
            one_person (found, contact, error);
            return NULL;
          }
        if (p)
          {
            who = name_found = g_strdup (p->name);
            for (guint j = 0; j < p->tels->len; j++)
              g_ptr_array_add (wanted, g_strdup (((Tel *) p->tels->pdata[j])->digits));
          }
      }
    /* a sender that is a name, not a number ("DHL"), or a name with Contacts off */
    by_name = !wanted->len;
    if (db && sqlite3_prepare_v2 (db, "SELECT _id, participant_normalized_destination, name FROM conversations"
                                      " WHERE latest_message_id IS NOT NULL", -1, &rows, NULL) == SQLITE_OK)
      while (sqlite3_step (rows) == SQLITE_ROW)
        {
          const char *dest = (const char *) sqlite3_column_text (rows, 1);
          const char *name = (const char *) sqlite3_column_text (rows, 2);
          gboolean match = FALSE;

          if (by_name)
            match = (dest && strcasestr (dest, contact)) || (name && strcasestr (name, contact));
          else if (dest)
            {
              g_autofree char *digits = digits_of (dest);
              for (guint i = 0; i < wanted->len && !match; i++)
                match = same_number (wanted->pdata[i], digits);
            }
          if (match)
            {
              gint64 id = sqlite3_column_int64 (rows, 0);
              g_array_append_val (ids, id);
            }
        }
    sqlite3_finalize (rows);
    rows = NULL;
    add_string (b, "with", who);
    json_builder_set_member_name (b, "messages");
    json_builder_begin_array (b);
    if (ids->len)
      {
        /* the newest, then given oldest first, as they were written */
        g_autoptr (GPtrArray) picked = g_ptr_array_new_with_free_func ((GDestroyNotify) json_node_unref);
        GString *in = g_string_new (NULL);

        for (guint i = 0; i < ids->len; i++)
          g_string_append_printf (in, "%s%" G_GINT64_FORMAT, i ? "," : "", g_array_index (ids, gint64, i));
        {
          g_autofree char *sql = g_strdup_printf (
            "SELECT m.message_status, m.received_timestamp, m.read,"
            " (SELECT group_concat(p.text, '') FROM parts p WHERE p.message_id = m._id)"
            " FROM messages m WHERE m.conversation_id IN (%s) AND m.message_status NOT IN (3)"
            " ORDER BY m.received_timestamp DESC LIMIT 200", in->str);
          g_string_free (in, TRUE);
          if (sqlite3_prepare_v2 (db, sql, -1, &rows, NULL) == SQLITE_OK)
            while (picked->len < count && sqlite3_step (rows) == SQLITE_ROW)
              {
                int status = sqlite3_column_int (rows, 0);
                gboolean incoming = status >= 100, unread = incoming && !sqlite3_column_int (rows, 2);
                g_autoptr (JsonBuilder) m = json_builder_new ();
                g_autofree char *when = tool_when (sqlite3_column_int64 (rows, 1), TRUE);

                if (unread_only && !unread)
                  continue;
                json_builder_begin_object (m);
                add_string (m, "from", incoming ? who : "the user");
                add_string (m, "when", when);
                if (unread)
                  add_bool (m, "unread", TRUE);
                if (with_text && sqlite3_column_text (rows, 3))
                  add_string (m, "text", (const char *) sqlite3_column_text (rows, 3));
                g_ptr_array_add (picked, finish (m));
              }
        }
        for (guint i = picked->len; i > 0; i--)
          json_builder_add_value (b, json_node_ref (picked->pdata[i - 1]));
        n = picked->len;
      }
    sqlite3_finalize (rows);
    json_builder_end_array (b);
    if (!n)
      add_string (b, "note", ids->len ? "No such messages." : "No messages with them.");
  }
  sqlite3_close (db);
  return finish (b);
}

static JsonNode *
propose_message (JsonObject *args, ToolRun *run, GError **error)
{
  const char *to = tool_arg_string (args, "to"), *text = tool_arg_string (args, "text");
  g_autofree char *line = NULL, *named = NULL, *number = NULL, *about = NULL, *quoted = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (DictateAction) action = NULL;

  if (!to)
    {
      tool_error (error, "Whom to send it to is missing: ask the user.");
      return NULL;
    }
  if (!text)
    {
      tool_error (error, "What the message should say is missing: ask the user.");
      return NULL;
    }
  if (!(number = resolve_recipient (to, "mobile", run, &line, &named, NULL))
      && !(number = resolve_recipient (to, NULL, run, &line, &named, error)))
    return NULL;
  quoted = g_strdup_printf ("“%s”", text);
  {
    const char *lines[] = { line, quoted, NULL };
    about = g_strdup_printf ("the message to %s", named);
    action = dictate_action_new ("send a message", _("Send a message"), lines, _("Send"), _("Sent"), about,
                                 send_message, number, text);
    dictate_action_set_log (action, "propose_message", "the message, by SMS to its recipient");
  }
  tool_run_propose (run, action);
  json_builder_begin_object (b);
  add_string (b, "shown", "A card on the screen with the message and a Send button.");
  add_string (b, "to", named);
  add_string (b, "status", "Not sent yet: it is sent only when the user taps Send.");
  return finish (b);
}

/* ---------------- Wi-Fi and Bluetooth ---------------- */

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"

static void
add_wifi (JsonBuilder *b, ToolRun *run)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) props = dbus_properties (G_BUS_TYPE_SYSTEM, NM, NM_PATH, NM, run, &error);
  g_autoptr (GVariant) active = NULL;
  gboolean on = FALSE;
  const char *network = NULL;
  g_autoptr (GVariant) keep = NULL;

  json_builder_set_member_name (b, "wifi");
  json_builder_begin_object (b);
  if (!props)
    {
      add_string (b, "error", "NetworkManager does not answer");
      json_builder_end_object (b);
      return;
    }
  g_variant_lookup (props, "WirelessEnabled", "b", &on);
  add_bool (b, "on", on);
  active = g_variant_lookup_value (props, "ActiveConnections", G_VARIANT_TYPE ("ao"));
  for (gsize i = 0; active && i < g_variant_n_children (active) && !network; i++)
    {
      const char *path;
      g_autoptr (GVariant) ac = NULL;
      const char *type = NULL;
      guint32 state = 0;

      g_variant_get_child (active, i, "&o", &path);
      ac = dbus_properties (G_BUS_TYPE_SYSTEM, NM, path, NM ".Connection.Active", run, NULL);
      if (!ac)
        continue;
      g_variant_lookup (ac, "Type", "&s", &type);
      g_variant_lookup (ac, "State", "u", &state);
      /* activated (2) Wi-Fi: its connection's name, which is the network's */
      if (!g_strcmp0 (type, "802-11-wireless") && state == 2)
        {
          g_variant_lookup (ac, "Id", "&s", &network);
          keep = g_variant_ref (ac);
        }
    }
  add_bool (b, "connected", network != NULL);
  if (network)
    add_string (b, "network", network);
  json_builder_end_object (b);
}

#define BLUEZ "org.bluez"

/* BlueZ's objects (GetManagedObjects), or NULL when it is not running */
static GVariant *
bluez_objects (ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SYSTEM, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager",
                                          "GetManagedObjects", NULL, "(a{oa{sa{sv}}})", run, error);
  return reply ? g_variant_get_child_value (reply, 0) : NULL;
}

static void
add_bluetooth (JsonBuilder *b, ToolRun *run)
{
  g_autoptr (GVariant) objects = bluez_objects (run, NULL);
  GVariantIter iter;
  const char *path;
  GVariant *interfaces;
  gboolean adapter = FALSE, on = FALSE;

  json_builder_set_member_name (b, "bluetooth");
  json_builder_begin_object (b);
  if (objects)
    {
      g_variant_iter_init (&iter, objects);
      while (g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
        {
          g_autoptr (GVariant) a = g_variant_lookup_value (interfaces, "org.bluez.Adapter1", NULL);
          if (a && !adapter)
            {
              adapter = TRUE;
              g_variant_lookup (a, "Powered", "b", &on);
            }
          g_variant_unref (interfaces);
        }
    }
  if (!adapter)
    {
      add_bool (b, "available", FALSE);
      json_builder_end_object (b);
      return;
    }
  add_bool (b, "on", on);
  json_builder_set_member_name (b, "connected_devices");
  json_builder_begin_array (b);
  g_variant_iter_init (&iter, objects);
  while (on && g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
    {
      g_autoptr (GVariant) d = g_variant_lookup_value (interfaces, "org.bluez.Device1", NULL);
      gboolean connected = FALSE;
      const char *alias = NULL;

      if (d && g_variant_lookup (d, "Connected", "b", &connected) && connected)
        {
          if (!g_variant_lookup (d, "Alias", "&s", &alias))
            g_variant_lookup (d, "Name", "&s", &alias);
          json_builder_add_string_value (b, alias ? alias : "a device");
        }
      g_variant_unref (interfaces);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
}

static JsonNode *
get_wifi_bluetooth (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  add_wifi (b, run);
  add_bluetooth (b, run);
  return finish (b);
}

/* the path of BlueZ's first adapter in `objects`, or NULL */
static const char *
bluez_adapter (GVariant *objects)
{
  GVariantIter iter;
  const char *path, *adapter = NULL;
  GVariant *interfaces;

  if (!objects)
    return NULL;
  g_variant_iter_init (&iter, objects);
  while (!adapter && g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
    {
      g_autoptr (GVariant) a = g_variant_lookup_value (interfaces, "org.bluez.Adapter1", NULL);
      if (a)
        adapter = path;
      g_variant_unref (interfaces);
    }
  return adapter;
}

/* whether Wi-Fi ("wifi") or Bluetooth is on now; FALSE with the error when that cannot be told */
static gboolean
radio_on (const char *radio, ToolRun *run, gboolean *on, GError **error)
{
  *on = FALSE;
  if (!g_ascii_strcasecmp (radio, "wifi"))
    {
      g_autoptr (GVariant) props = dbus_properties (G_BUS_TYPE_SYSTEM, NM, NM_PATH, NM, run, error);
      return props && g_variant_lookup (props, "WirelessEnabled", "b", on);
    }
  {
    g_autoptr (GVariant) objects = bluez_objects (run, error);
    GVariantIter iter;
    const char *path;
    GVariant *interfaces;
    gboolean found = FALSE;

    if (!objects)
      return FALSE;
    g_variant_iter_init (&iter, objects);
    while (!found && g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
      {
        g_autoptr (GVariant) a = g_variant_lookup_value (interfaces, "org.bluez.Adapter1", NULL);
        if (a)
          found = g_variant_lookup (a, "Powered", "b", on);
        g_variant_unref (interfaces);
      }
    if (!found)
      tool_error (error, "This phone has no Bluetooth that can be turned on.");
    return found;
  }
}

/* Wi-Fi ("wifi") or Bluetooth turned on or off, as Settings' switches do; the model's error otherwise */
static gboolean
set_radio (const char *radio, gboolean on, ToolRun *run, GError **error)
{
  GError *failure = NULL;

  if (!g_ascii_strcasecmp (radio, "wifi"))
    {
      if (dbus_set (G_BUS_TYPE_SYSTEM, NM, NM_PATH, NM, "WirelessEnabled", g_variant_new_boolean (on), run,
                    &failure))
        return TRUE;
      tool_error (error, "Wi-Fi could not be turned %s: %s", on ? "on" : "off", plain (failure));
      g_error_free (failure);
      return FALSE;
    }
  {
    g_autoptr (GVariant) objects = bluez_objects (run, NULL);
    const char *adapter = bluez_adapter (objects);

    if (!adapter)
      {
        tool_error (error, "This phone has no Bluetooth that can be turned on.");
        return FALSE;
      }
    if (dbus_set (G_BUS_TYPE_SYSTEM, BLUEZ, adapter, "org.bluez.Adapter1", "Powered", g_variant_new_boolean (on),
                  run, &failure))
      return TRUE;
    if (strstr (failure->message, "rfkill"))
      tool_error (error, "Bluetooth is blocked: airplane mode is on.");
    else
      tool_error (error, "Bluetooth could not be turned %s: %s", on ? "on" : "off", plain (failure));
    g_error_free (failure);
    return FALSE;
  }
}

/* what undoing a change needs, for the action log: {"KEY": value, "about": "…"} */
static JsonNode *
undo_int (const char *key, gint64 value, const char *about)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  add_int (b, key, value);
  add_string (b, "about", about);
  return finish (b);
}

static JsonNode *
set_wifi_bluetooth (JsonObject *args, ToolRun *run, GError **error)
{
  const char *radio = tool_arg_string (args, "radio");
  JsonNode *on_node = json_object_get_member (args, "on");
  gboolean on = tool_arg_bool (args, "on", FALSE), was = FALSE, known;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  const char *name;

  if (!radio || (g_ascii_strcasecmp (radio, "wifi") && g_ascii_strcasecmp (radio, "bluetooth")))
    {
      tool_error (error, "radio must be \"wifi\" or \"bluetooth\".");
      return NULL;
    }
  if (!on_node)
    {
      tool_error (error, "Whether to turn it on or off is missing.");
      return NULL;
    }
  name = g_ascii_strcasecmp (radio, "wifi") ? "bluetooth" : "wifi";
  /* as it was, for undo */
  known = radio_on (name, run, &was, NULL);
  if (!set_radio (name, on, run, error))
    return NULL;
  json_builder_begin_object (b);
  add_string (b, name, on ? "on" : "off");
  if (known && was == on)
    {
      add_string (b, "note", on ? "It was already on." : "It was already off.");
      tool_run_set_unchanged (run);
    }
  else
    {
      g_autoptr (JsonBuilder) u = json_builder_new ();
      g_autofree char *about = g_strdup_printf ("%s turned %s", strcmp (name, "wifi") ? "Bluetooth" : "Wi-Fi",
                                                on ? "on" : "off");

      json_builder_begin_object (u);
      add_string (u, "radio", name);
      add_bool (u, "on", !on);
      add_string (u, "about", about);
      tool_run_set_undo (run, finish (u));
      add_bool (b, "can_undo", TRUE);
    }
  return finish (b);
}

/* ---------------- alarms and timers (the Clock's service) ---------------- */

#define CLOCK "vibeos.Clock.Alarms"

static const char *const DAY_NAMES[] = { "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
                                         "sunday" };

static char *
days_text (guint32 days)
{
  GString *s;

  if (days == 0)
    return g_strdup ("once");
  if (days == 0x7f)
    return g_strdup ("every day");
  if (days == 0x1f)
    return g_strdup ("on weekdays");
  if (days == 0x60)
    return g_strdup ("at weekends");
  s = g_string_new ("on ");
  for (guint i = 0, n = 0; i < 7; i++)
    if (days & (1u << i))
      g_string_append_printf (s, "%s%c%s", n++ ? ", " : "", g_ascii_toupper (DAY_NAMES[i][0]), DAY_NAMES[i] + 1);
  return g_string_free (s, FALSE);
}

/* when an alarm at hour:minute on `days` next rings: "in 7 h 12 min", for the model to say "tomorrow at 6" */
static char *
rings_in (int hour, int minute, guint32 days)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  for (int d = 0; d <= 7; d++)
    {
      g_autoptr (GDateTime) day = g_date_time_add_days (now, d);
      g_autoptr (GDateTime) at = g_date_time_new_local (g_date_time_get_year (day), g_date_time_get_month (day),
                                                        g_date_time_get_day_of_month (day), hour, minute, 0);
      int dow = g_date_time_get_day_of_week (at) - 1;

      if (g_date_time_compare (at, now) <= 0 || (days && !(days & (1u << dow))))
        continue;
      return tool_when (g_date_time_to_unix (at) * 1000, TRUE);
    }
  return NULL;
}

static JsonNode *
get_alarms (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) alarms = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms",
                                           "GetAlarms", NULL, "(aa{sv})", run, error);
  g_autoptr (GVariant) timers = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GVariant) list = NULL;
  guint n = 0;

  if (!alarms)
    return NULL;
  timers = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "GetTimers", NULL,
                      "(aa{sv})", run, NULL);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "alarms");
  json_builder_begin_array (b);
  list = g_variant_get_child_value (alarms, 0);
  for (gsize i = 0; i < g_variant_n_children (list) && n < ROWS_MAX; i++, n++)
    {
      g_autoptr (GVariant) a = g_variant_get_child_value (list, i);
      gint32 hour = 0, minute = 0;
      guint32 days = 0;
      gboolean enabled = FALSE;
      const char *label = NULL;
      g_autofree char *time = NULL, *repeats = NULL, *next = NULL;

      g_variant_lookup (a, "hour", "i", &hour);
      g_variant_lookup (a, "minute", "i", &minute);
      g_variant_lookup (a, "days", "u", &days);
      g_variant_lookup (a, "enabled", "b", &enabled);
      g_variant_lookup (a, "label", "&s", &label);
      json_builder_begin_object (b);
      time = g_strdup_printf ("%02d:%02d", hour, minute);
      add_string (b, "time", time);
      repeats = days_text (days);
      add_string (b, "repeats", repeats);
      add_bool (b, "on", enabled);
      if (label && *label)
        add_string (b, "label", label);
      if (enabled && (next = rings_in (hour, minute, days)))
        add_string (b, "next", next);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  if (timers)
    {
      g_autoptr (GVariant) tl = g_variant_get_child_value (timers, 0);

      json_builder_set_member_name (b, "running_timers");
      json_builder_begin_array (b);
      for (gsize i = 0; i < g_variant_n_children (tl); i++)
        {
          g_autoptr (GVariant) t = g_variant_get_child_value (tl, i);
          guint32 state = 0;
          gint64 remaining = 0, length = 0, last_start = 0;
          const char *label = NULL;
          g_autofree char *left = NULL;

          g_variant_lookup (t, "state", "u", &state);
          g_variant_lookup (t, "remaining", "x", &remaining);
          g_variant_lookup (t, "length", "x", &length);
          g_variant_lookup (t, "last-start", "x", &last_start);
          g_variant_lookup (t, "label", "&s", &label);
          /* running (1) or paused (2) */
          if (state != 1 && state != 2)
            continue;
          json_builder_begin_object (b);
          /* a running timer's remaining is as of its last start, on the Clock's elapsed clock (CLOCK_BOOTTIME, ms) */
          if (state == 1 && last_start > 0)
            {
              struct timespec now;
              clock_gettime (CLOCK_BOOTTIME, &now);
              remaining -= (gint64) now.tv_sec * 1000 + now.tv_nsec / 1000000 - last_start;
            }
          left = duration_text (MAX (remaining, 0) / 1000);
          add_string (b, "left", left);
          add_bool (b, "paused", state == 2);
          if (label && *label)
            add_string (b, "label", label);
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
    }
  return finish (b);
}

/* "06:30", "6:30", "6", "6:30 pm", "18.30" */
static gboolean
parse_time (const char *s, int *hour, int *minute)
{
  char *end;
  gint64 h = g_ascii_strtoll (s, &end, 10), m = 0;

  if (end == s)
    return FALSE;
  if (*end == ':' || *end == '.' || *end == 'h')
    {
      const char *p = end + 1;
      if (g_ascii_isdigit (*p))
        m = g_ascii_strtoll (p, &end, 10);
    }
  while (*end == ' ')
    end++;
  if (!g_ascii_strncasecmp (end, "pm", 2) || !g_ascii_strncasecmp (end, "p.m", 3))
    h = h % 12 + 12;
  else if (!g_ascii_strncasecmp (end, "am", 2) || !g_ascii_strncasecmp (end, "a.m", 3))
    h = h % 12;
  if (h < 0 || h > 23 || m < 0 || m > 59)
    return FALSE;
  *hour = h, *minute = m;
  return TRUE;
}

static JsonNode *
set_alarm (JsonObject *args, ToolRun *run, GError **error)
{
  const char *time = tool_arg_string (args, "time"), *label = tool_arg_string (args, "label");
  JsonNode *days_node = json_object_get_member (args, "days");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GVariant) reply = NULL;
  g_autofree char *at = NULL, *repeats = NULL, *next = NULL;
  GVariantBuilder alarm;
  guint32 days = 0;
  int hour, minute;

  if (!time || !parse_time (time, &hour, &minute))
    {
      tool_error (error, "The time is missing or not HH:MM: ask the user when.");
      return NULL;
    }
  if (days_node && JSON_NODE_HOLDS_ARRAY (days_node))
    {
      JsonArray *list = json_node_get_array (days_node);
      for (guint i = 0; i < json_array_get_length (list); i++)
        {
          const char *d = json_array_get_string_element (list, i);
          for (guint j = 0; d && j < 7; j++)
            if (!g_ascii_strncasecmp (d, DAY_NAMES[j], 3))
              days |= 1u << j;
          if (d && !g_ascii_strcasecmp (d, "weekdays"))
            days |= 0x1f;
          if (d && (!g_ascii_strcasecmp (d, "every day") || !g_ascii_strcasecmp (d, "daily")))
            days |= 0x7f;
        }
    }
  g_variant_builder_init (&alarm, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&alarm, "{sv}", "hour", g_variant_new_int32 (hour));
  g_variant_builder_add (&alarm, "{sv}", "minute", g_variant_new_int32 (minute));
  g_variant_builder_add (&alarm, "{sv}", "days", g_variant_new_uint32 (days));
  g_variant_builder_add (&alarm, "{sv}", "enabled", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&alarm, "{sv}", "label", g_variant_new_string (label ? label : ""));
  reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms", "AddAlarm",
                     g_variant_new ("(a{sv})", &alarm), "(u)", run, error);
  if (!reply)
    return NULL;
  json_builder_begin_object (b);
  at = g_strdup_printf ("%02d:%02d", hour, minute);
  add_string (b, "alarm_set", at);
  repeats = days_text (days);
  add_string (b, "repeats", repeats);
  if ((next = rings_in (hour, minute, days)))
    add_string (b, "rings", next);
  {
    guint32 id;
    g_autofree char *about = g_strdup_printf ("the alarm at %s", at);

    g_variant_get (reply, "(u)", &id);
    tool_run_set_undo (run, undo_int ("delete_alarm", id, about));
    add_bool (b, "can_undo", TRUE);
  }
  return finish (b);
}

static JsonNode *
set_timer (JsonObject *args, ToolRun *run, GError **error)
{
  double minutes = tool_arg_double (args, "minutes", 0);
  gint64 seconds = tool_arg_int (args, "seconds", 0) + (gint64) (minutes * 60 + 0.5);
  const char *label = tool_arg_string (args, "label");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GVariant) added = NULL, started = NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) end = NULL;
  g_autofree char *length = NULL, *ends = NULL;
  guint32 id;

  if (seconds <= 0 || seconds > 24 * 3600)
    {
      tool_error (error, "The length is missing: ask the user how long.");
      return NULL;
    }
  added = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "AddTimer",
                     g_variant_new ("(xsb)", seconds * 1000, label ? label : "", TRUE), "(u)", run, error);
  if (!added)
    return NULL;
  g_variant_get (added, "(u)", &id);
  started = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "Start",
                       g_variant_new ("(u)", id), NULL, run, error);
  if (!started)
    return NULL;
  json_builder_begin_object (b);
  length = duration_text (seconds);
  add_string (b, "timer_started", length);
  end = g_date_time_add_seconds (now, seconds);
  ends = g_date_time_format (end, "%H:%M");
  add_string (b, "ends_at", ends);
  {
    g_autofree char *about = g_strdup_printf ("the %s timer", length);
    tool_run_set_undo (run, undo_int ("delete_timer", id, about));
    add_bool (b, "can_undo", TRUE);
  }
  return finish (b);
}

/* ---------------- the calendar ---------------- */

typedef struct {
  gint64 start, end;
  char *summary, *location;
  gboolean all_day;
} Event;

static void
event_free (Event *e)
{
  g_free (e->summary);
  g_free (e->location);
  g_free (e);
}

static gint
by_start (gconstpointer a, gconstpointer b)
{
  const Event *x = *(Event **) a, *y = *(Event **) b;
  return x->start < y->start ? -1 : x->start > y->start;
}

typedef struct {
  GPtrArray *events;
  ICalTimezone *zone;
  gint64 from;                  /* the asked range's start */
} Collect;

/* an all-day event's day as the phone's local midnight: the time zone EDS was given (or none) does not move it */
static gint64
local_midnight (ICalTime *t)
{
  g_autoptr (GDateTime) day = g_date_time_new_local (i_cal_time_get_year (t), i_cal_time_get_month (t),
                                                    i_cal_time_get_day (t), 0, 0, 0);

  return day ? g_date_time_to_unix (day) : 0;
}

static gboolean
collect (ICalComponent *comp, ICalTime *start, ICalTime *end, gpointer data, GCancellable *c, GError **error)
{
  Collect *col = data;
  Event *e;
  const char *summary = i_cal_component_get_summary (comp), *location = i_cal_component_get_location (comp);
  gboolean all_day = i_cal_time_is_date (start);
  gint64 to = all_day ? local_midnight (end) : i_cal_time_as_timet_with_zone (end, i_cal_time_get_timezone (end));

  /* the day before's all-day event ends at the asked day's midnight: EDS gives it, it is not on that day */
  if (to <= col->from)
    return TRUE;
  e = g_new0 (Event, 1);
  e->all_day = all_day;
  e->start = all_day ? local_midnight (start) : i_cal_time_as_timet_with_zone (start, i_cal_time_get_timezone (start));
  e->end = to;
  e->summary = g_strdup (summary && *summary ? summary : "(no title)");
  e->location = location && *location ? g_strdup (location) : NULL;
  g_ptr_array_add (col->events, e);
  return col->events->len < 200;
}

static JsonNode *
get_events (JsonObject *args, ToolRun *run, GError **error)
{
  const char *date = tool_arg_string (args, "date");
  gint64 days = CLAMP (tool_arg_int (args, "days", 1), 1, 14);
  g_autoptr (ESourceRegistry) r = registry (run, error);
  g_autoptr (GDateTime) from = NULL;
  g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) event_free);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GTimeZone) local = g_time_zone_new_local ();
  const char *zone_id = g_time_zone_get_identifier (local);
  Collect col = { events, NULL, 0 };
  GList *sources;
  int y, m, d;

  if (!r)
    return NULL;
  if (date && sscanf (date, "%d-%d-%d", &y, &m, &d) == 3)
    from = g_date_time_new_local (y, m, d, 0, 0, 0);
  if (!from)
    {
      g_autoptr (GDateTime) now = g_date_time_new_now_local ();
      from = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                    g_date_time_get_day_of_month (now), 0, 0, 0);
    }
  col.zone = zone_id && *zone_id != '/' && *zone_id != ':' ? i_cal_timezone_get_builtin_timezone (zone_id) : NULL;
  sources = e_source_registry_list_enabled (r, E_SOURCE_EXTENSION_CALENDAR);
  for (GList *l = sources; l; l = l->next)
    {
      g_autoptr (GError) failure = NULL;
      g_autoptr (EClient) client = e_cal_client_connect_sync (l->data, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, EDS_NO_WAIT,
                                                              tool_run_cancellable (run), &failure);
      gint64 start = g_date_time_to_unix (from);

      col.from = start;
      if (!client)
        {
          g_debug ("tools: a calendar: %s", failure->message);
          continue;
        }
      if (col.zone)
        e_cal_client_set_default_timezone (E_CAL_CLIENT (client), col.zone);
      e_cal_client_generate_instances_sync (E_CAL_CLIENT (client), start, start + days * 86400, NULL, collect, &col);
    }
  g_list_free_full (sources, g_object_unref);
  g_ptr_array_sort (events, by_start);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "events");
  json_builder_begin_array (b);
  for (guint i = 0; i < events->len && i < ROWS_MAX; i++)
    {
      Event *e = events->pdata[i];
      g_autofree char *when = tool_when (e->start * 1000, !e->all_day);

      json_builder_begin_object (b);
      add_string (b, "title", e->summary);
      add_string (b, "when", when);
      if (e->all_day)
        add_bool (b, "all_day", TRUE);
      else
        {
          g_autoptr (GDateTime) end = g_date_time_new_from_unix_local (e->end);
          g_autofree char *until = g_date_time_format (end, "%H:%M");
          add_string (b, "until", until);
        }
      if (e->location)
        add_string (b, "where", e->location);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  if (!events->len)
    add_string (b, "note", "Nothing in the calendar then.");
  return finish (b);
}

/* ---------------- apps ---------------- */

static JsonNode *
get_running_apps (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SESSION, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell",
                                          "RunningApps", NULL, "(a(ss))", run, NULL);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GVariant) list = NULL;

  if (!reply)
    {
      tool_error (error, "The phone's shell cannot tell which apps are open.");
      return NULL;
    }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "open_apps");
  json_builder_begin_array (b);
  list = g_variant_get_child_value (reply, 0);
  for (gsize i = 0; i < g_variant_n_children (list); i++)
    {
      const char *id, *name;
      g_variant_get_child (list, i, "(&s&s)", &id, &name);
      json_builder_add_string_value (b, name && *name ? name : id);
    }
  json_builder_end_array (b);
  if (!g_variant_n_children (list))
    add_string (b, "note", "No apps are open.");
  return finish (b);
}

/* the apps the launcher shows */
static GPtrArray *
shown_apps (void)
{
  GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);
  GList *all = g_app_info_get_all ();

  for (GList *l = all; l; l = l->next)
    {
      GAppInfo *info = l->data;
      const char *id = g_app_info_get_id (info);

      if (!id || !g_app_info_should_show (info) || g_str_has_prefix (id, "vibeos.Launcher")
          || (G_IS_DESKTOP_APP_INFO (info) && g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (info), "Terminal")))
        continue;
      g_ptr_array_add (apps, g_object_ref (info));
    }
  g_list_free_full (all, g_object_unref);
  return apps;
}

static char *
fold (const char *s)
{
  g_autofree char *down = g_utf8_casefold (s, -1);
  return g_utf8_normalize (down, -1, G_NORMALIZE_ALL);
}

/* the app called `name`: its name exactly, then its name or generic name or keywords containing it */
static GAppInfo *
find_app (GPtrArray *apps, const char *name)
{
  g_autofree char *want = fold (name);
  GAppInfo *contains = NULL;

  for (guint i = 0; i < apps->len; i++)
    {
      GAppInfo *info = apps->pdata[i];
      g_autofree char *display = fold (g_app_info_get_display_name (info));
      if (!strcmp (display, want))
        return info;
      if (!contains && strstr (display, want))
        contains = info;
    }
  if (contains)
    return contains;
  for (guint i = 0; i < apps->len; i++)
    {
      GAppInfo *info = apps->pdata[i];
      const char *generic = G_IS_DESKTOP_APP_INFO (info)
                            ? g_desktop_app_info_get_generic_name (G_DESKTOP_APP_INFO (info)) : NULL;
      const char *const *keywords = G_IS_DESKTOP_APP_INFO (info)
                                    ? g_desktop_app_info_get_keywords (G_DESKTOP_APP_INFO (info)) : NULL;
      g_autofree char *g = generic ? fold (generic) : NULL;

      if (g && strstr (g, want))
        return info;
      for (guint k = 0; keywords && keywords[k]; k++)
        {
          g_autofree char *kw = fold (keywords[k]);
          if (!strcmp (kw, want))
            return info;
        }
    }
  return NULL;
}

static gboolean
launch (const char *id, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = NULL;

  /* the shell starts it, as the launcher does (its splash, the focus) */
  reply = dbus_call (G_BUS_TYPE_SESSION, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell", "Launch",
                     g_variant_new ("(s(dddd)s)", id, 0.0, 0.0, 0.0, 0.0, ""), NULL, run, NULL);
  if (reply)
    return TRUE;
  {
    g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new (id);
    return info && g_app_info_launch (G_APP_INFO (info), NULL, NULL, error);
  }
}

static JsonNode *
open_app (JsonObject *args, ToolRun *run, GError **error)
{
  const char *name = tool_arg_string (args, "name");
  g_autoptr (GPtrArray) apps = shown_apps ();
  g_autoptr (JsonBuilder) b = json_builder_new ();
  GAppInfo *app;

  if (!name)
    {
      tool_error (error, "Which app is missing: ask the user.");
      return NULL;
    }
  app = find_app (apps, name);
  if (!app)
    {
      tool_error (error, "There is no app called \"%s\" on the phone.", name);
      return NULL;
    }
  if (tool_run_dry (run))
    g_message ("tools: would open an app (dry run)");
  else if (!launch (g_app_info_get_id (app), run, error))
    return NULL;
  json_builder_begin_object (b);
  add_string (b, "opened", g_app_info_get_display_name (app));
  return finish (b);
}

/* ---------------- navigation (Navi) ---------------- */

/* the first place Navi's geocoder (our own Photon, as Navi's search) finds for `query`: its name and position */
static gboolean
geocode (const char *query, ToolRun *run, char **name, double *lat, double *lon, GError **error)
{
  const char *server = g_getenv ("NAVI_SERVER");
  g_autofree char *escaped = g_uri_escape_string (query, NULL, FALSE);
  g_autofree char *url = g_strdup_printf ("%s/photon/api?q=%s&limit=1&lang=en",
                                          server && *server ? server : "https://navi.vibeos.top", escaped);
  g_autoptr (SoupSession) session = soup_session_new_with_options ("timeout", 8, "user-agent", "vibeos-agent", NULL);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root, *feature, *props;
  JsonArray *features, *coords;
  GString *label;
  const char *keys[] = { "name", "street", "housenumber", "city", "country" };

  body = soup_session_send_and_read (session, msg, tool_run_cancellable (run), error);
  if (!body || soup_message_get_status (msg) != SOUP_STATUS_OK
      || !json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL))
    {
      g_clear_error (error);
      tool_error (error, "Places cannot be looked up right now.");
      return FALSE;
    }
  root = json_node_get_object (json_parser_get_root (parser));
  features = root && json_object_has_member (root, "features") ? json_object_get_array_member (root, "features")
                                                               : NULL;
  if (!features || json_array_get_length (features) == 0)
    {
      tool_error (error, "No place called \"%s\" was found. Ask the user for an address.", query);
      return FALSE;
    }
  feature = json_array_get_object_element (features, 0);
  coords = json_object_get_array_member (json_object_get_object_member (feature, "geometry"), "coordinates");
  props = json_object_get_object_member (feature, "properties");
  *lon = json_array_get_double_element (coords, 0);
  *lat = json_array_get_double_element (coords, 1);
  label = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      const char *v = json_object_has_member (props, keys[i]) ? json_object_get_string_member (props, keys[i]) : NULL;
      if (!v || !*v)
        continue;
      if (!strcmp (keys[i], "housenumber"))
        g_string_append_printf (label, " %s", v);
      else
        g_string_append_printf (label, "%s%s", label->len ? ", " : "", v);
    }
  *name = g_string_free (label, FALSE);
  return TRUE;
}

static JsonNode *
navigate_to (JsonObject *args, ToolRun *run, GError **error)
{
  const char *place = tool_arg_string (args, "place");
  g_autoptr (GDesktopAppInfo) navi = g_desktop_app_info_new ("vibeos.Navi.desktop");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *name = NULL;
  double lat, lon;

  if (!place)
    {
      tool_error (error, "Where to is missing: ask the user.");
      return NULL;
    }
  if (!navi && !tool_run_dry (run))
    {
      tool_error (error, "Navi, the maps app, is not installed on this phone.");
      return NULL;
    }
  tool_run_add_sent (run, "the place's name, to Navi's place search on VibeOS's server");
  if (!geocode (place, run, &name, &lat, &lon, error))
    return NULL;
  if (tool_run_dry (run))
    g_message ("tools: would open Navi with routes (dry run)");
  else
    {
      /* Navi's own command line: it shows the routes there, and the user picks one */
      char to[128];
      g_autofree char *dest = NULL;
      const char *exec = g_app_info_get_executable (G_APP_INFO (navi));
      const char *argv[] = { exec, "--to", NULL, NULL };

      g_snprintf (to, sizeof to, "%.6f,%.6f", lat, lon);
      dest = g_strdup_printf ("%s,%s", to, name);
      for (char *p = dest; *p; p++)
        if (*p == '\n')
          *p = ' ';
      argv[2] = dest;
      if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error))
        return NULL;
    }
  json_builder_begin_object (b);
  add_string (b, "opened", "Navi, with the routes there; the user picks one to start");
  add_string (b, "destination", name);
  return finish (b);
}

/* ---------------- removing an alarm or a timer, and putting it back ---------------- */

/* the Clock's alarms (GetAlarms' array), or NULL with the error */
static GVariant *
clock_alarms (ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms",
                                          "GetAlarms", NULL, "(aa{sv})", run, error);
  return reply ? g_variant_get_child_value (reply, 0) : NULL;
}

/* "06:45 on weekdays (Work)", for the model to ask which */
static char *
alarm_text (GVariant *a)
{
  gint32 hour = 0, minute = 0;
  guint32 days = 0;
  gboolean enabled = TRUE;
  const char *label = NULL;
  g_autofree char *repeats = NULL;

  g_variant_lookup (a, "hour", "i", &hour);
  g_variant_lookup (a, "minute", "i", &minute);
  g_variant_lookup (a, "days", "u", &days);
  g_variant_lookup (a, "enabled", "b", &enabled);
  g_variant_lookup (a, "label", "&s", &label);
  repeats = days_text (days);
  return g_strdup_printf ("%02d:%02d %s%s%s%s%s", hour, minute, repeats, label && *label ? " (" : "",
                          label && *label ? label : "", label && *label ? ")" : "", enabled ? "" : ", off");
}

static JsonNode *
cancel_alarm (JsonObject *args, ToolRun *run, GError **error)
{
  const char *time = tool_arg_string (args, "time"), *label = tool_arg_string (args, "label");
  g_autoptr (GVariant) list = NULL;
  g_autoptr (GPtrArray) exact = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_autoptr (GPtrArray) twelve = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonBuilder) u = json_builder_new ();
  g_autoptr (GVariant) deleted = NULL;
  g_autofree char *at = NULL, *repeats = NULL, *about = NULL;
  GPtrArray *matching;
  GVariant *a;
  gint32 hour = 0, minute = 0;
  guint32 id = 0, days = 0;
  gboolean enabled = TRUE, vibrate = TRUE, has_vibrate;
  const char *alarm_label = NULL, *ringtone = NULL;
  int want_hour = -1, want_minute = -1;

  if (time && !parse_time (time, &want_hour, &want_minute))
    {
      tool_error (error, "The time is not HH:MM: ask the user which alarm.");
      return NULL;
    }
  if (!(list = clock_alarms (run, error)))
    return NULL;
  for (gsize i = 0; i < g_variant_n_children (list); i++)
    {
      g_autoptr (GVariant) one = g_variant_get_child_value (list, i);
      const char *l = NULL;

      g_variant_lookup (one, "hour", "i", &hour);
      g_variant_lookup (one, "minute", "i", &minute);
      g_variant_lookup (one, "label", "&s", &l);
      if (label && !(l && strcasestr (l, label)))
        continue;
      if (!time || (hour == want_hour && minute == want_minute))
        g_ptr_array_add (exact, g_variant_ref (one));
      else if (hour % 12 == want_hour % 12 && minute == want_minute)
        /* "half past six" given as 06:30 for an alarm at 18:30 */
        g_ptr_array_add (twelve, g_variant_ref (one));
    }
  matching = exact->len ? exact : twelve;
  if (matching->len != 1)
    {
      GString *all = g_string_new (NULL);
      GPtrArray *named = matching->len ? matching : NULL;
      gsize n = named ? named->len : g_variant_n_children (list);

      for (gsize i = 0; i < n && i < ROWS_MAX; i++)
        {
          g_autoptr (GVariant) one = named ? g_variant_ref (named->pdata[i]) : g_variant_get_child_value (list, i);
          g_autofree char *text = alarm_text (one);
          g_string_append_printf (all, "%s%s", i ? "; " : "", text);
        }
      if (g_variant_n_children (list) == 0)
        tool_error (error, "There are no alarms.");
      else if (matching->len == 0)
        tool_error (error, "No alarm matches that. The alarms are: %s. Ask the user which one.", all->str);
      else
        tool_error (error, "Several alarms match: %s. Ask the user which one.", all->str);
      g_string_free (all, TRUE);
      return NULL;
    }
  a = matching->pdata[0];
  g_variant_lookup (a, "id", "u", &id);
  g_variant_lookup (a, "hour", "i", &hour);
  g_variant_lookup (a, "minute", "i", &minute);
  g_variant_lookup (a, "days", "u", &days);
  g_variant_lookup (a, "enabled", "b", &enabled);
  g_variant_lookup (a, "label", "&s", &alarm_label);
  g_variant_lookup (a, "ringtone", "&s", &ringtone);
  has_vibrate = g_variant_lookup (a, "vibrate", "b", &vibrate);
  deleted = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms", "DeleteAlarm",
                       g_variant_new ("(u)", id), NULL, run, error);
  if (!deleted)
    return NULL;
  at = g_strdup_printf ("%02d:%02d", hour, minute);
  about = g_strdup_printf ("the alarm at %s", at);
  /* what putting it back needs: AddAlarm's keys */
  json_builder_begin_object (u);
  json_builder_set_member_name (u, "add_alarm");
  json_builder_begin_object (u);
  add_int (u, "hour", hour);
  add_int (u, "minute", minute);
  add_int (u, "days", days);
  add_bool (u, "enabled", enabled);
  add_string (u, "label", alarm_label ? alarm_label : "");
  if (has_vibrate)
    add_bool (u, "vibrate", vibrate);
  if (ringtone)
    add_string (u, "ringtone", ringtone);
  json_builder_end_object (u);
  add_string (u, "about", about);
  tool_run_set_undo (run, finish (u));
  json_builder_begin_object (b);
  add_string (b, "alarm_removed", at);
  repeats = days_text (days);
  add_string (b, "repeats", repeats);
  if (alarm_label && *alarm_label)
    add_string (b, "label", alarm_label);
  add_bool (b, "can_undo", TRUE);
  return finish (b);
}

/* a timer's time left now (ms): its remaining as of its last start for a running one (timer-service.c) */
static gint64
timer_left (GVariant *t)
{
  guint32 state = 0;
  gint64 remaining = 0, last_start = 0;

  g_variant_lookup (t, "state", "u", &state);
  g_variant_lookup (t, "remaining", "x", &remaining);
  g_variant_lookup (t, "last-start", "x", &last_start);
  if (state == 1 && last_start > 0)
    {
      struct timespec now;
      clock_gettime (CLOCK_BOOTTIME, &now);
      remaining -= (gint64) now.tv_sec * 1000 + now.tv_nsec / 1000000 - last_start;
    }
  return MAX (remaining, 0);
}

static JsonNode *
cancel_timer (JsonObject *args, ToolRun *run, GError **error)
{
  const char *label = tool_arg_string (args, "label");
  g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers",
                                          "GetTimers", NULL, "(aa{sv})", run, error);
  g_autoptr (GVariant) list = NULL, deleted = NULL;
  g_autoptr (GPtrArray) matching = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonBuilder) u = json_builder_new ();
  g_autofree char *left = NULL, *about = NULL;
  const char *timer_label = NULL;
  guint32 id = 0, state = 0;
  gint64 ms;
  GVariant *t;

  if (!reply)
    return NULL;
  list = g_variant_get_child_value (reply, 0);
  for (gsize i = 0; i < g_variant_n_children (list); i++)
    {
      g_autoptr (GVariant) one = g_variant_get_child_value (list, i);
      const char *l = NULL;

      g_variant_lookup (one, "state", "u", &state);
      g_variant_lookup (one, "label", "&s", &l);
      /* running (1) or paused (2) */
      if ((state == 1 || state == 2) && (!label || (l && strcasestr (l, label))))
        g_ptr_array_add (matching, g_variant_ref (one));
    }
  if (matching->len != 1)
    {
      GString *all = g_string_new (NULL);

      for (guint i = 0; i < matching->len && i < ROWS_MAX; i++)
        {
          const char *l = NULL;
          g_autofree char *d = duration_text (timer_left (matching->pdata[i]) / 1000);
          g_variant_lookup (matching->pdata[i], "label", "&s", &l);
          g_string_append_printf (all, "%s%s left%s%s%s", i ? "; " : "", d, l && *l ? " (" : "", l && *l ? l : "",
                                  l && *l ? ")" : "");
        }
      if (!matching->len)
        tool_error (error, label ? "No timer called \"%s\" is running." : "No timer is running.", label);
      else
        tool_error (error, "Several timers are running: %s. Ask the user which one.", all->str);
      g_string_free (all, TRUE);
      return NULL;
    }
  t = matching->pdata[0];
  g_variant_lookup (t, "id", "u", &id);
  g_variant_lookup (t, "state", "u", &state);
  g_variant_lookup (t, "label", "&s", &timer_label);
  ms = timer_left (t);
  deleted = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "Delete",
                       g_variant_new ("(u)", id), NULL, run, error);
  if (!deleted)
    return NULL;
  left = duration_text ((ms + 999) / 1000);
  about = g_strdup_printf ("the timer with %s left", left);
  json_builder_begin_object (u);
  json_builder_set_member_name (u, "add_timer");
  json_builder_begin_object (u);
  add_int (u, "ms", ms);
  add_string (u, "label", timer_label ? timer_label : "");
  add_bool (u, "running", state == 1);
  json_builder_end_object (u);
  add_string (u, "about", about);
  tool_run_set_undo (run, finish (u));
  json_builder_begin_object (b);
  add_string (b, "timer_stopped", left);
  add_string (b, "time_it_had_left", left);
  if (timer_label && *timer_label)
    add_string (b, "label", timer_label);
  add_bool (b, "can_undo", TRUE);
  return finish (b);
}

/* ---------------- the action log, and undo (dictate-actions.h) ---------------- */

static JsonNode *
get_action_log (JsonObject *args, ToolRun *run, GError **error)
{
  gint64 count = CLAMP (tool_arg_int (args, "count", 5), 1, ROWS_MAX);
  g_autoptr (GPtrArray) entries = dictate_actions_recent (200);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  guint n = 0, questions = 0;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "actions");
  json_builder_begin_array (b);
  for (guint i = 0; i < entries->len && n < count; i++)
    {
      JsonObject *e = entries->pdata[i];
      const char *tool = json_object_get_string_member_with_default (e, "tool", "");
      const DictateTool *t = dictate_tools_find (tool);
      JsonObject *undo = json_object_has_member (e, "undo") ? json_object_get_object_member (e, "undo") : NULL;
      JsonArray *sent = json_object_has_member (e, "sent") ? json_object_get_array_member (e, "sent") : NULL;
      g_autofree char *when = tool_when (json_object_get_int_member_with_default (e, "t", 0) * 1000, TRUE);
      g_autofree char *what = NULL;
      gint64 id = json_object_get_int_member_with_default (e, "id", 0);
      guint others = 0;

      if (!strcmp (tool, "question"))
        {
          questions++;
          continue;
        }
      if (undo && json_object_has_member (undo, "about"))
        what = g_strdup_printf ("%s (%s)", t ? t->did : tool, json_object_get_string_member (undo, "about"));
      else
        what = g_strdup (t ? t->did : tool);
      json_builder_begin_object (b);
      add_string (b, "what", what);
      add_string (b, "tool", tool);
      add_string (b, "when", when);
      add_int (b, "tier", json_object_get_int_member_with_default (e, "tier", 0));
      add_string (b, "outcome", json_object_get_string_member_with_default (e, "outcome", ""));
      for (guint j = 0; sent && j < json_array_get_length (sent); j++)
        if (g_strcmp0 (json_array_get_string_element (sent, j), DICTATE_SENT_SERVER))
          {
            if (!others++)
              {
                json_builder_set_member_name (b, "also_sent_to");
                json_builder_begin_array (b);
              }
            json_builder_add_string_value (b, json_array_get_string_element (sent, j));
          }
      if (others)
        json_builder_end_array (b);
      if (dictate_actions_undone (entries, id))
        add_bool (b, "undone", TRUE);
      json_builder_end_object (b);
      n++;
    }
  json_builder_end_array (b);
  if (questions)
    add_int (b, "questions_answered_meanwhile", questions);
  add_string (b, "where_things_went",
              "Every question the user asks Agent, and what each tool finds, goes to VibeOS's own model server, "
              "which writes the answers; it goes nowhere else unless also_sent_to says so. What was done (an alarm, "
              "a setting) was done on the phone itself.");
  if (!n)
    add_string (b, "note", "Agent has not done anything yet.");
  return finish (b);
}

/* call_control's actions, for undoing one (the call going on, further down) */
static gboolean call_do (const char *action, ToolRun *run, gboolean *changed, char **about, GError **error);

static JsonNode *
undo_last_action (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GPtrArray) entries = dictate_actions_recent (100);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC, id = 0;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *done = NULL;
  JsonObject *target = NULL, *undo;
  const DictateTool *tool;
  const char *about;

  for (guint i = 0; i < entries->len && !target; i++)
    {
      JsonObject *e = entries->pdata[i];
      const char *name = json_object_get_string_member_with_default (e, "tool", "");
      const char *outcome = json_object_get_string_member_with_default (e, "outcome", "");

      if (now - json_object_get_int_member_with_default (e, "t", 0) > 24 * 3600)
        break;
      /* reads, undoing itself, and what did nothing (failed, cancelled, a card still waiting, a switch already as
       * asked) are passed over */
      if (json_object_get_int_member_with_default (e, "tier", 0) < DICTATE_TIER_UNDOABLE
          || !strcmp (name, "undo_last_action") || g_str_has_prefix (outcome, "failed")
          || strstr (outcome, "cancelled") || strstr (outcome, "waiting") || strstr (outcome, "nothing changed"))
        continue;
      if (dictate_actions_undone (entries, json_object_get_int_member_with_default (e, "id", 0)))
        continue;
      target = e;
    }
  if (!target)
    {
      tool_error (error, "Agent has done nothing in the last day that could be undone.");
      return NULL;
    }
  tool = dictate_tools_find (json_object_get_string_member (target, "tool"));
  undo = json_object_has_member (target, "undo") ? json_object_get_object_member (target, "undo") : NULL;
  if (!undo)
    {
      if (json_object_get_int_member_with_default (target, "tier", 0) >= DICTATE_TIER_CONFIRMED)
        tool_error (error, "The last thing done was this: %s, done when the user tapped its card. Agent cannot "
                    "undo that.", tool ? tool->did : "an action");
      else
        tool_error (error, "The last thing done was this: %s. That cannot be undone from here%s.",
                    tool ? tool->did : "an action",
                    tool && !strcmp (tool->name, "open_app") ? "; the app can be closed in the recent apps" : "");
      return NULL;
    }
  about = json_object_get_string_member_with_default (undo, "about", "it");
  id = json_object_get_int_member_with_default (target, "id", 0);
  if (json_object_has_member (undo, "radio"))
    {
      const char *radio = json_object_get_string_member (undo, "radio");
      gboolean on = json_object_get_boolean_member_with_default (undo, "on", TRUE);

      if (!tool_run_area_on (run, "access-settings"))
        {
          tool_error (error, "No access: the user has turned off the phone's settings in Agent's settings.");
          return NULL;
        }
      if (!set_radio (radio, on, run, error))
        return NULL;
      done = g_strdup_printf ("%s is %s again", strcmp (radio, "wifi") ? "Bluetooth" : "Wi-Fi", on ? "on" : "off");
    }
  else if (json_object_has_member (undo, "call_action"))
    {
      g_autofree char *again = NULL;
      gboolean changed;

      if (!tool_run_area_on (run, "access-calls"))
        {
          tool_error (error, "No access: the user has turned off calls in Agent's settings.");
          return NULL;
        }
      if (!call_do (json_object_get_string_member (undo, "call_action"), run, &changed, &again, error))
        return NULL;
      done = g_strdup_printf ("undone: %s", about);
    }
  else
    {
      g_autoptr (GVariant) reply = NULL;

      if (!tool_run_area_on (run, "access-alarms"))
        {
          tool_error (error, "No access: the user has turned off alarms and timers in Agent's settings.");
          return NULL;
        }
      if (json_object_has_member (undo, "delete_alarm"))
        {
          reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms", "DeleteAlarm",
                             g_variant_new ("(u)", (guint32) json_object_get_int_member (undo, "delete_alarm")),
                             NULL, run, error);
          done = g_strdup_printf ("%s is removed", about);
        }
      else if (json_object_has_member (undo, "delete_timer"))
        {
          reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "Delete",
                             g_variant_new ("(u)", (guint32) json_object_get_int_member (undo, "delete_timer")),
                             NULL, run, error);
          done = g_strdup_printf ("%s is stopped and removed", about);
        }
      else if (json_object_has_member (undo, "add_alarm"))
        {
          JsonObject *a = json_object_get_object_member (undo, "add_alarm");
          GVariantBuilder alarm;

          g_variant_builder_init (&alarm, G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add (&alarm, "{sv}", "hour",
                                 g_variant_new_int32 (json_object_get_int_member_with_default (a, "hour", 0)));
          g_variant_builder_add (&alarm, "{sv}", "minute",
                                 g_variant_new_int32 (json_object_get_int_member_with_default (a, "minute", 0)));
          g_variant_builder_add (&alarm, "{sv}", "days",
                                 g_variant_new_uint32 (json_object_get_int_member_with_default (a, "days", 0)));
          g_variant_builder_add (&alarm, "{sv}", "enabled",
                                 g_variant_new_boolean (json_object_get_boolean_member_with_default (a, "enabled",
                                                                                                     TRUE)));
          g_variant_builder_add (&alarm, "{sv}", "label",
                                 g_variant_new_string (json_object_get_string_member_with_default (a, "label", "")));
          if (json_object_has_member (a, "vibrate"))
            g_variant_builder_add (&alarm, "{sv}", "vibrate",
                                   g_variant_new_boolean (json_object_get_boolean_member (a, "vibrate")));
          if (json_object_has_member (a, "ringtone"))
            g_variant_builder_add (&alarm, "{sv}", "ringtone",
                                   g_variant_new_string (json_object_get_string_member (a, "ringtone")));
          reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Alarms", "vibeos.Clock.Alarms", "AddAlarm",
                             g_variant_new ("(a{sv})", &alarm), "(u)", run, error);
          done = g_strdup_printf ("%s is back", about);
        }
      else if (json_object_has_member (undo, "add_timer"))
        {
          JsonObject *t = json_object_get_object_member (undo, "add_timer");
          gint64 ms = json_object_get_int_member_with_default (t, "ms", 0);
          guint32 timer;

          reply = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "AddTimer",
                             g_variant_new ("(xsb)", MAX (ms, 1000),
                                            json_object_get_string_member_with_default (t, "label", ""), TRUE),
                             "(u)", run, error);
          if (reply && json_object_get_boolean_member_with_default (t, "running", TRUE))
            {
              g_autoptr (GVariant) started = NULL;
              g_variant_get (reply, "(u)", &timer);
              started = dbus_call (G_BUS_TYPE_SESSION, CLOCK, "/vibeos/Clock/Timers", "vibeos.Clock.Timers", "Start",
                                   g_variant_new ("(u)", timer), NULL, run, error);
              if (!started)
                g_clear_pointer (&reply, g_variant_unref);
            }
          done = g_strdup_printf ("%s runs again", about);
        }
      else
        {
          tool_error (error, "That cannot be undone.");
          return NULL;
        }
      if (!reply)
        return NULL;
    }
  tool_run_set_undid (run, id);
  json_builder_begin_object (b);
  add_string (b, "undone", tool ? tool->did : "the last action");
  add_string (b, "now", done);
  return finish (b);
}

/* ---------------- the mobile network (ModemManager, NetworkManager) ---------------- */

#define MM "org.freedesktop.ModemManager1"

static const char *
generation (guint32 tech)
{
  /* MMModemAccessTechnology: 5G NR, LTE, the UMTS family, the GSM family */
  return tech & (1u << 15) ? "5G" : tech & (1u << 14) ? "4G" : tech & 0x3e0 ? "3G" : tech & 0x1e ? "2G" : NULL;
}

static JsonNode *
get_mobile_network (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SYSTEM, MM, "/org/freedesktop/ModemManager1",
                                          "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                                          "(a{oa{sa{sv}}})", run, NULL);
  g_autoptr (GVariant) objects = reply ? g_variant_get_child_value (reply, 0) : NULL;
  g_autoptr (GVariant) modem = NULL, gpp = NULL, nm = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  GVariantIter iter;
  const char *path, *sim = NULL, *op = NULL;
  GVariant *interfaces;
  gint32 state = 0;
  guint32 quality = 0, tech = 0, registration = 0;
  gboolean recent = FALSE, wwan = FALSE;

  /* only these: nothing that names the phone or the SIM (the IMEI, the IMSI, the number) is read */
  if (objects)
    {
      g_variant_iter_init (&iter, objects);
      while (!modem && g_variant_iter_next (&iter, "{&o@a{sa{sv}}}", &path, &interfaces))
        {
          modem = g_variant_lookup_value (interfaces, MM ".Modem", G_VARIANT_TYPE_VARDICT);
          if (modem)
            gpp = g_variant_lookup_value (interfaces, MM ".Modem.Modem3gpp", G_VARIANT_TYPE_VARDICT);
          g_variant_unref (interfaces);
        }
    }
  json_builder_begin_object (b);
  if (!modem)
    {
      add_bool (b, "modem", FALSE);
      add_string (b, "note", reply ? "This device has no mobile network: it has no modem."
                                   : "The mobile network cannot be read: ModemManager does not answer.");
      return finish (b);
    }
  g_variant_lookup (modem, "Sim", "&o", &sim);
  g_variant_lookup (modem, "State", "i", &state);
  g_variant_lookup (modem, "SignalQuality", "(ub)", &quality, &recent);
  g_variant_lookup (modem, "AccessTechnologies", "u", &tech);
  if (gpp)
    {
      g_variant_lookup (gpp, "OperatorName", "&s", &op);
      g_variant_lookup (gpp, "RegistrationState", "u", &registration);
    }
  add_bool (b, "sim", sim && g_strcmp0 (sim, "/"));
  /* MMModemState: -1 failed, 2 locked (the SIM's PIN), 3 disabled, 6 enabled, 7 searching, 8 registered,
   * 10 connecting, 11 connected */
  add_string (b, "state", state < 0 ? "failed" : state == 2 ? "the SIM is locked (its PIN is needed)"
                          : state < 6 ? "off" : state == 6 ? "on, but on no network"
                          : state == 7 ? "searching for a network" : "on a network");
  if (op && *op && state >= 8)
    add_string (b, "operator", op);
  if (state >= 8)
    {
      /* bars as Android draws them: four, a quarter of the quality each */
      add_int (b, "signal_bars", quality >= 75 ? 4 : quality >= 50 ? 3 : quality >= 25 ? 2 : quality > 0 ? 1 : 0);
      add_int (b, "signal_bars_of", 4);
      add_int (b, "signal_percent", quality);
      if (generation (tech))
        add_string (b, "network_type", generation (tech));
      /* MMModem3gppRegistrationState: roaming, roaming SMS only, roaming CSFB not preferred */
      add_bool (b, "roaming", registration == 5 || registration == 7 || registration == 10);
    }
  nm = dbus_properties (G_BUS_TYPE_SYSTEM, NM, NM_PATH, NM, run, NULL);
  if (nm && g_variant_lookup (nm, "WwanEnabled", "b", &wwan))
    add_bool (b, "mobile_data_on", wwan);
  add_bool (b, "data_connected", state == 11);
  return finish (b);
}

/* ---------------- the weather (Open-Meteo, as the Weather app) ---------------- */

/* a GET's JSON, or NULL with the model's error `failed` */
static JsonNode *
http_json (const char *url, ToolRun *run, const char *failed, GError **error)
{
  g_autoptr (SoupSession) session = soup_session_new_with_options ("timeout", 8, "user-agent",
                                                                   "vibeos-agent/1.0 (VibeOS)", NULL);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();

  body = msg ? soup_session_send_and_read (session, msg, tool_run_cancellable (run), NULL) : NULL;
  if (!body || soup_message_get_status (msg) != SOUP_STATUS_OK
      || !json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL)
      || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      tool_error (error, "%s", failed);
      return NULL;
    }
  return json_node_copy (json_parser_get_root (parser));
}

/* WMO's weather codes, as Open-Meteo gives them */
static const char *
sky (gint64 code)
{
  switch (code)
    {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: case 81: return "rain showers";
    case 82: return "heavy rain showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorms";
    case 96: case 99: return "thunderstorms with hail";
    default: return "unknown";
    }
}

static double
number_at (JsonObject *o, const char *array, guint i, double fallback)
{
  JsonArray *a = o && json_object_has_member (o, array) ? json_object_get_array_member (o, array) : NULL;
  JsonNode *n = a && i < json_array_get_length (a) ? json_array_get_element (a, i) : NULL;

  return n && JSON_NODE_HOLDS_VALUE (n) ? json_node_get_double (n) : fallback;
}

/* the place the Weather app shows (and its home screen widget): ~/.cache/vibeos-launcher/weather.json, else the city
 * chosen in it */
static gboolean
weather_place (double *lat, double *lon, char **name)
{
  g_autofree char *path = g_build_filename (g_get_user_cache_dir (), "vibeos-launcher", "weather.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (json_parser_load_from_file (parser, path, NULL) && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      JsonObject *o = json_node_get_object (json_parser_get_root (parser));
      *lat = json_object_get_double_member_with_default (o, "lat", 0);
      *lon = json_object_get_double_member_with_default (o, "lon", 0);
      *name = g_strdup (json_object_get_string_member_with_default (o, "place", ""));
      if (*lat || *lon)
        return TRUE;
      g_clear_pointer (name, g_free);
    }
  {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "vibeos.Weather", TRUE)
                                                : NULL;
    g_autoptr (GSettings) settings = schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
    g_autofree char *city = NULL;

    if (!settings)
      return FALSE;
    g_settings_get (settings, "city", "(sdd)", &city, lat, lon);
    if (!*city)
      return FALSE;
    *name = g_steal_pointer (&city);
    return TRUE;
  }
}

/* the Weather app's last forecast, if it is for here and less than half an hour old */
static JsonNode *
weather_cached (double lat, double lon)
{
  g_autofree char *path = g_build_filename (g_get_user_cache_dir (), "vibeos-launcher", "forecast.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  GStatBuf st;
  JsonObject *o;

  if (g_stat (path, &st) != 0 || time (NULL) - st.st_mtime > 30 * 60
      || !json_parser_load_from_file (parser, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return NULL;
  o = json_node_get_object (json_parser_get_root (parser));
  if (fabs (json_object_get_double_member_with_default (o, "latitude", 999) - lat) > 0.2
      || fabs (json_object_get_double_member_with_default (o, "longitude", 999) - lon) > 0.2)
    return NULL;
  return json_node_copy (json_parser_get_root (parser));
}

static JsonNode *
get_weather (JsonObject *args, ToolRun *run, GError **error)
{
  const char *place = tool_arg_string (args, "place");
  gint64 days = CLAMP (tool_arg_int (args, "days", 2), 1, 7);
  const char *server = g_getenv ("DICTATE_WEATHER_SERVER");
  g_autoptr (JsonNode) forecast = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *name = NULL, *today = g_date_time_format (now, "%Y-%m-%d");
  JsonObject *f, *current, *daily;
  JsonArray *dates;
  double lat, lon;
  guint n = 0;

  if (place)
    {
      /* a town named: found by Open-Meteo's place search, as Weather's Set City finds it */
      g_autofree char *escaped = g_uri_escape_string (place, NULL, FALSE);
      g_autofree char *url = g_strdup_printf ("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1"
                                              "&language=en&format=json", escaped);
      g_autoptr (JsonNode) found = http_json (url, run, "Places cannot be looked up right now.", error);
      JsonObject *first;
      JsonArray *results;

      if (!found)
        return NULL;
      tool_run_add_sent (run, "the place's name, to Open-Meteo's place search");
      results = json_object_has_member (json_node_get_object (found), "results")
                ? json_object_get_array_member (json_node_get_object (found), "results") : NULL;
      if (!results || !json_array_get_length (results))
        {
          tool_error (error, "No place called \"%s\" was found.", place);
          return NULL;
        }
      first = json_array_get_object_element (results, 0);
      lat = json_object_get_double_member_with_default (first, "latitude", 0);
      lon = json_object_get_double_member_with_default (first, "longitude", 0);
      name = g_strdup_printf ("%s, %s", json_object_get_string_member_with_default (first, "name", place),
                              json_object_get_string_member_with_default (first, "country", ""));
    }
  else
    {
      if (!weather_place (&lat, &lon, &name))
        {
          tool_error (error, "Where the phone is is not known yet: the user can open the Weather app once, or name a "
                      "town.");
          return NULL;
        }
      forecast = weather_cached (lat, lon);
    }
  if (!forecast)
    {
      /* the town, not the street: the position to a tenth of a degree, about 10 km */
      char la[G_ASCII_DTOSTR_BUF_SIZE], lo[G_ASCII_DTOSTR_BUF_SIZE];
      g_autofree char *url = NULL;

      g_ascii_formatd (la, sizeof la, "%.1f", lat);
      g_ascii_formatd (lo, sizeof lo, "%.1f", lon);
      url = g_strdup_printf ("%s/v1/forecast?latitude=%s&longitude=%s&timezone=auto&forecast_days=%d"
                             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
                             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max",
                             server && *server ? server : "https://api.open-meteo.com", la, lo, (int) days);
      if (!(forecast = http_json (url, run, "The weather service does not answer right now.", error)))
        return NULL;
      tool_run_add_sent (run, "the town's rough position (to about 10 km), to Open-Meteo, the weather service");
    }
  f = json_node_get_object (forecast);
  current = json_object_has_member (f, "current") ? json_object_get_object_member (f, "current") : NULL;
  daily = json_object_has_member (f, "daily") ? json_object_get_object_member (f, "daily") : NULL;
  dates = daily && json_object_has_member (daily, "time") ? json_object_get_array_member (daily, "time") : NULL;
  json_builder_begin_object (b);
  if (name && *name)
    add_string (b, "place", name);
  if (current)
    {
      /* what the forecast has of these: the Weather app asks for more, an older one for fewer */
      static const struct { const char *from, *to; } NOW[] = {
        { "temperature_2m", "temperature_c" }, { "apparent_temperature", "feels_like_c" },
        { "wind_speed_10m", "wind_kmh" },
      };

      json_builder_set_member_name (b, "now");
      json_builder_begin_object (b);
      for (guint i = 0; i < G_N_ELEMENTS (NOW); i++)
        if (json_object_has_member (current, NOW[i].from))
          add_int (b, NOW[i].to, (gint64) floor (json_object_get_double_member (current, NOW[i].from) + 0.5));
      if (json_object_has_member (current, "weather_code"))
        add_string (b, "sky", sky (json_object_get_int_member (current, "weather_code")));
      json_builder_end_object (b);
    }
  json_builder_set_member_name (b, "days");
  json_builder_begin_array (b);
  for (guint i = 0; dates && i < json_array_get_length (dates) && n < days; i++)
    {
      const char *date = json_array_get_string_element (dates, i);
      int y, m, d;
      g_autoptr (GDateTime) noon = NULL;
      g_autofree char *day = NULL;

      /* the Weather app's forecast starts with yesterday */
      if (!date || strcmp (date, today) < 0 || sscanf (date, "%d-%d-%d", &y, &m, &d) != 3)
        continue;
      noon = g_date_time_new_local (y, m, d, 12, 0, 0);
      day = tool_when (g_date_time_to_unix (noon) * 1000, FALSE);
      json_builder_begin_object (b);
      add_string (b, "day", day);
      add_string (b, "sky", sky ((gint64) number_at (daily, "weather_code", i, -1)));
      add_int (b, "high_c", (gint64) floor (number_at (daily, "temperature_2m_max", i, 0) + 0.5));
      add_int (b, "low_c", (gint64) floor (number_at (daily, "temperature_2m_min", i, 0) + 0.5));
      add_int (b, "rain_chance_percent", (gint64) number_at (daily, "precipitation_probability_max", i, 0));
      json_builder_end_object (b);
      n++;
    }
  json_builder_end_array (b);
  return finish (b);
}

/* ---------------- what the user missed ---------------- */

typedef struct {
  char *who;
  guint n;
  gint64 last;                  /* ms */
  char *text;                   /* the newest one's, when asked for */
} Missed;

static void
missed_free (Missed *m)
{
  g_free (m->who);
  g_free (m->text);
  g_free (m);
}

/* the entry for `who` in `list`, new if there is none */
static Missed *
missed_for (GPtrArray *list, const char *who)
{
  Missed *m;

  for (guint i = 0; i < list->len; i++)
    if (!g_strcmp0 (((Missed *) list->pdata[i])->who, who))
      return list->pdata[i];
  m = g_new0 (Missed, 1);
  m->who = g_strdup (who);
  g_ptr_array_add (list, m);
  return m;
}

static void
add_missed (JsonBuilder *b, const char *name, const char *who_key, const char *count_key, GPtrArray *list)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_array (b);
  for (guint i = 0; i < list->len && i < ROWS_MAX; i++)
    {
      Missed *m = list->pdata[i];
      g_autofree char *when = tool_when (m->last, TRUE);

      json_builder_begin_object (b);
      add_string (b, who_key, m->who);
      add_int (b, count_key, m->n);
      add_string (b, "latest", when);
      if (m->text)
        add_string (b, "text", m->text);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
}

/* who a number is: a contact's name, or what the store has */
static char *
caller (GPtrArray *all, const char *number, int presentation)
{
  Person *p = number && *number ? person_for (all, number, NULL) : NULL;

  if (p)
    return g_strdup (p->name);
  if (number && *number && presentation == 1)
    return g_strdup (number);
  return g_strdup ("a hidden number");
}

static JsonNode *
what_did_i_miss (JsonObject *args, ToolRun *run, GError **error)
{
  gboolean with_text = tool_arg_bool (args, "include_text", FALSE) && !tool_run_private (run);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC, since = dictate_actions_missed_since ();
  g_autoptr (GPtrArray) all = people_with_numbers (run);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *from = NULL;
  gboolean first = since <= 0 || now - since > 7 * 24 * 3600;

  /* the first time, or after a week: the last day */
  if (first)
    since = now - 24 * 3600;
  json_builder_begin_object (b);
  if (!with_text && tool_arg_bool (args, "include_text", FALSE))
    add_string (b, "private_mode", "What they say is not given: private mode is on and the answer plays out loud on "
                                   "the speaker. Say that the user can hear it with headphones on.");
  from = tool_when (since * 1000, TRUE);
  add_string (b, "since", from);
  if (first)
    add_string (b, "since_note", "Agent was not asked this before (or not for a week): the last 24 hours.");

  /* missed calls since then */
  if (!tool_run_area_on (run, "access-calls"))
    add_string (b, "missed_calls", "no access: calls are turned off in Agent's settings");
  else
    {
      g_autoptr (GPtrArray) calls = g_ptr_array_new_with_free_func ((GDestroyNotify) missed_free);
      sqlite3 *db = open_store ("vibeos-dialer", "calls.db", NULL);
      sqlite3_stmt *rows = NULL;
      guint total = 0;

      if (db && sqlite3_prepare_v2 (db, "SELECT number, date, presentation FROM calls WHERE type = 3 AND date >= ?"
                                        " ORDER BY date DESC LIMIT 100", -1, &rows, NULL) == SQLITE_OK)
        {
          sqlite3_bind_int64 (rows, 1, since * 1000);
          while (sqlite3_step (rows) == SQLITE_ROW)
            {
              g_autofree char *who = caller (all, (const char *) sqlite3_column_text (rows, 0),
                                             sqlite3_column_int (rows, 2));
              Missed *m = missed_for (calls, who);
              if (!m->n++)
                m->last = sqlite3_column_int64 (rows, 1);
              total++;
            }
        }
      sqlite3_finalize (rows);
      sqlite3_close (db);
      add_int (b, "missed_call_count", total);
      add_missed (b, "missed_calls", "from", "calls", calls);
    }

  /* unread messages, whenever they came */
  if (!tool_run_area_on (run, "access-messages"))
    add_string (b, "unread_messages", "no access: messages are turned off in Agent's settings");
  else
    {
      g_autoptr (GPtrArray) messages = g_ptr_array_new_with_free_func ((GDestroyNotify) missed_free);
      sqlite3 *db = open_store ("vibeos-messaging", "bugle_db", NULL);
      sqlite3_stmt *rows = NULL;
      guint total = 0;

      if (db && sqlite3_prepare_v2 (db,
            "SELECT c.participant_normalized_destination, c.name, m.received_timestamp,"
            " (SELECT group_concat(p.text, '') FROM parts p WHERE p.message_id = m._id)"
            " FROM messages m JOIN conversations c ON c._id = m.conversation_id"
            " WHERE m.read = 0 AND m.message_status >= 100 AND c.archive_status = 0"
            " ORDER BY m.received_timestamp DESC LIMIT 200", -1, &rows, NULL) == SQLITE_OK)
        while (sqlite3_step (rows) == SQLITE_ROW)
          {
            Conversation c = { 0, (char *) sqlite3_column_text (rows, 0), (char *) sqlite3_column_text (rows, 1) };
            g_autofree char *owned = NULL;
            Missed *m = missed_for (messages, conversation_who (&c, all, &owned));

            if (!m->n++)
              {
                m->last = sqlite3_column_int64 (rows, 2);
                if (with_text && sqlite3_column_text (rows, 3))
                  m->text = g_strdup ((const char *) sqlite3_column_text (rows, 3));
              }
            total++;
          }
      sqlite3_finalize (rows);
      sqlite3_close (db);
      add_int (b, "unread_message_count", total);
      add_missed (b, "unread_messages", "from", "messages", messages);
    }

  /* the notifications still in the shade since then, from the other apps (the calls' and the messages' are above) */
  if (!tool_run_area_on (run, "access-apps"))
    add_string (b, "notifications", "no access: apps are turned off in Agent's settings");
  else
    {
      g_autoptr (GVariant) reply = dbus_call (G_BUS_TYPE_SESSION, "vibeos.Shell", "/vibeos/Shell", "vibeos.Shell",
                                              "Notifications", NULL, "(a(ssssx))", run, NULL);
      if (!reply)
        add_string (b, "notifications", "this phone's shell cannot tell its notifications yet");
      else
        {
          g_autoptr (GVariant) list = g_variant_get_child_value (reply, 0);
          g_autoptr (GPtrArray) apps = g_ptr_array_new_with_free_func ((GDestroyNotify) missed_free);
          guint total = 0;

          for (gsize i = 0; i < g_variant_n_children (list); i++)
            {
              const char *id, *app, *title, *body;
              gint64 when;
              Missed *m;

              g_variant_get_child (list, i, "(&s&s&s&sx)", &id, &app, &title, &body, &when);
              if (when < since || g_str_has_prefix (id, "vibeos.Messaging") || g_str_has_prefix (id, "vibeos.Dialer")
                  || g_str_has_prefix (id, "vibeos.Agent"))
                continue;
              m = missed_for (apps, *app ? app : id);
              if (!m->n++)
                {
                  m->last = when * 1000;
                  if (with_text)
                    m->text = g_strdup_printf ("%s%s%s", title, *title && *body ? ": " : "", body);
                }
              total++;
            }
          add_int (b, "notification_count", total);
          add_missed (b, "notifications", "app", "notifications", apps);
        }
    }
  /* the next "what did I miss?" starts from here; a test of the private phone starts from scratch each time */
  dictate_actions_set_missed_since (now);
  return finish (b);
}

/* ---------------- a new contact, a new event (cards) ---------------- */

/* a store's EDS client: the default address book or calendar, connected */
static EClient *
default_store (gboolean calendar, GError **error)
{
  g_autoptr (ESourceRegistry) r = e_source_registry_new_sync (NULL, error);
  g_autoptr (ESource) source = NULL;

  if (!r)
    return NULL;
  source = calendar ? e_source_registry_ref_default_calendar (r) : e_source_registry_ref_default_address_book (r);
  if (!source)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("There is nowhere to keep it"));
      return NULL;
    }
  return calendar ? e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, EDS_NO_WAIT, NULL, error)
                  : e_book_client_connect_sync (source, EDS_NO_WAIT, NULL, error);
}

/* the card's Add: the contact, as a vCard in `text` */
static gboolean
add_contact (const char *unused, const char *text, GError **error)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (EContact) contact = e_contact_new_from_vcard (text);
  g_autoptr (EClient) client = NULL;
  gboolean ok;

  g_main_context_push_thread_default (context);
  client = default_store (FALSE, error);
  ok = client && e_book_client_add_contact_sync (E_BOOK_CLIENT (client), contact, E_BOOK_OPERATION_FLAG_NONE, NULL,
                                                 NULL, error);
  g_main_context_pop_thread_default (context);
  return ok;
}

static JsonNode *
propose_contact (JsonObject *args, ToolRun *run, GError **error)
{
  const char *name = tool_arg_string (args, "name"), *number = tool_arg_string (args, "number");
  const char *type = tool_arg_string (args, "number_type"), *email = tool_arg_string (args, "email");
  g_autoptr (EContact) contact = NULL;
  g_autoptr (DictateAction) action = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) same = NULL;
  g_autofree char *vcard = NULL, *about = NULL;

  if (!name)
    {
      tool_error (error, "The name is missing: ask the user.");
      return NULL;
    }
  if (!number && !email)
    {
      tool_error (error, "A number or an email address is missing: ask the user.");
      return NULL;
    }
  if (number && !looks_like_number (number))
    {
      tool_error (error, "\"%s\" is not a phone number: ask the user for it.", number);
      return NULL;
    }
  if (!type || (strcmp (type, "home") && strcmp (type, "work")))
    type = "mobile";
  contact = e_contact_new ();
  e_contact_set (contact, E_CONTACT_FULL_NAME, name);
  {
    EContactName *parts = e_contact_name_from_string (name);
    e_contact_set (contact, E_CONTACT_NAME, parts);
    e_contact_name_free (parts);
  }
  g_ptr_array_add (lines, g_strdup (name));
  if (number)
    {
      EVCardAttribute *attr = e_vcard_attribute_new (NULL, EVC_TEL);
      e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_TYPE),
                                              !strcmp (type, "home") ? "HOME" : !strcmp (type, "work") ? "WORK"
                                                                                                     : "CELL");
      e_vcard_append_attribute_with_value (E_VCARD (contact), attr, number);
      g_ptr_array_add (lines, g_strdup_printf ("%s · %s", _(type), number));
    }
  if (email)
    {
      e_vcard_append_attribute_with_value (E_VCARD (contact), e_vcard_attribute_new (NULL, EVC_EMAIL), email);
      g_ptr_array_add (lines, g_strdup (email));
    }
  g_ptr_array_add (lines, NULL);
  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  about = g_strdup_printf ("the new contact %s", name);
  action = dictate_action_new ("add a contact", _("Add a contact"), (const char *const *) lines->pdata, _("Add"),
                               _("Added"), about, add_contact, NULL, vcard);
  dictate_action_set_log (action, "propose_contact", NULL);
  tool_run_propose (run, action);
  json_builder_begin_object (b);
  add_string (b, "shown", "A card on the screen with the new contact and an Add button.");
  add_string (b, "status", "Not added yet: it is added only when the user taps Add.");
  same = tool_run_area_on (run, "access-contacts") ? people_named (name, run, NULL) : NULL;
  if (same && same->len)
    add_string (b, "note", "There is already a contact with that name.");
  return finish (b);
}

/* the card's Add: the event, as iCalendar in `text` */
static gboolean
add_event (const char *unused, const char *text, GError **error)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  ICalComponent *comp = i_cal_component_new_from_string (text);
  g_autoptr (EClient) client = NULL;
  gboolean ok;

  if (!comp)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The event could not be made"));
      return FALSE;
    }
  g_main_context_push_thread_default (context);
  client = default_store (TRUE, error);
  ok = client && e_cal_client_create_object_sync (E_CAL_CLIENT (client), comp, E_CAL_OPERATION_FLAG_NONE, NULL, NULL,
                                                  error);
  g_main_context_pop_thread_default (context);
  g_object_unref (comp);
  return ok;
}

static JsonNode *
propose_event (JsonObject *args, ToolRun *run, GError **error)
{
  const char *title = tool_arg_string (args, "title"), *date = tool_arg_string (args, "date");
  const char *time = tool_arg_string (args, "time"), *where = tool_arg_string (args, "where");
  gint64 minutes = CLAMP (tool_arg_int (args, "minutes", 60), 5, 24 * 60);
  gint64 reminder = tool_arg_int (args, "reminder_minutes", -1);
  g_autoptr (GDateTime) start = NULL, end = NULL;
  g_autoptr (DictateAction) action = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *uid = NULL, *ical = NULL, *about = NULL, *when = NULL;
  ICalComponent *comp;
  ICalTime *t0, *t1;
  int y, m, d, hour = 0, minute = 0;

  if (!title)
    {
      tool_error (error, "What it is called is missing: ask the user.");
      return NULL;
    }
  if (!date || sscanf (date, "%d-%d-%d", &y, &m, &d) != 3 || !(start = g_date_time_new_local (y, m, d, 0, 0, 0)))
    {
      tool_error (error, "The day is missing or not YYYY-MM-DD: ask the user which day.");
      return NULL;
    }
  if (time && !parse_time (time, &hour, &minute))
    {
      tool_error (error, "The time is not HH:MM: ask the user when.");
      return NULL;
    }
  comp = i_cal_component_new_vevent ();
  uid = e_util_generate_uid ();
  i_cal_component_set_uid (comp, uid);
  i_cal_component_set_summary (comp, title);
  if (where)
    i_cal_component_set_location (comp, where);
  if (time)
    {
      g_autoptr (GDateTime) at = g_date_time_new_local (y, m, d, hour, minute, 0);
      g_autofree char *from = g_date_time_format (at, "%A %e %B, %H:%M");

      g_clear_pointer (&start, g_date_time_unref);
      start = g_steal_pointer (&at);
      end = g_date_time_add_minutes (start, minutes);
      t0 = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (start), FALSE, i_cal_timezone_get_utc_timezone ());
      t1 = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (end), FALSE, i_cal_timezone_get_utc_timezone ());
      {
        g_autofree char *until = g_date_time_format (end, "%H:%M");
        when = g_strdup_printf ("%s–%s", g_strstrip (from), until);
      }
    }
  else
    {
      t0 = i_cal_time_new_null_time ();
      i_cal_time_set_date (t0, y, m, d);
      i_cal_time_set_is_date (t0, TRUE);
      t1 = i_cal_time_clone (t0);
      i_cal_time_adjust (t1, 1, 0, 0, 0);
      when = g_date_time_format (start, "%A %e %B");
      g_strstrip (when);
    }
  i_cal_component_set_dtstart (comp, t0);
  i_cal_component_set_dtend (comp, t1);
  g_object_unref (t0);
  g_object_unref (t1);
  if (reminder >= 0)
    {
      /* a notification at the time, or so many minutes before it */
      ICalComponent *alarm = i_cal_component_new_valarm ();
      ICalTrigger *trigger = i_cal_trigger_new_from_string ("-PT0M");
      ICalDuration *before = i_cal_duration_new_from_int (-(int) MIN (reminder, 7 * 24 * 60) * 60);

      i_cal_trigger_set_duration (trigger, before);
      ICalProperty *action_p = i_cal_property_new_action (I_CAL_ACTION_DISPLAY);

      i_cal_component_take_property (alarm, action_p);
      i_cal_component_take_property (alarm, i_cal_property_new_trigger (trigger));
      i_cal_component_take_property (alarm, i_cal_property_new_description (title));
      i_cal_component_take_component (comp, alarm);
      g_object_unref (trigger);
      g_object_unref (before);
    }
  ical = i_cal_component_as_ical_string (comp);
  g_object_unref (comp);
  g_ptr_array_add (lines, g_strdup (title));
  g_ptr_array_add (lines, g_strdup (when));
  if (where)
    g_ptr_array_add (lines, g_strdup (where));
  if (reminder == 0)
    g_ptr_array_add (lines, g_strdup (_("A reminder at the time")));
  else if (reminder > 0)
    g_ptr_array_add (lines, g_strdup_printf (_("A reminder %d minutes before"), (int) reminder));
  g_ptr_array_add (lines, NULL);
  about = g_strdup_printf ("the calendar event \"%s\"", title);
  action = dictate_action_new ("add a calendar event", _("Add to the calendar"), (const char *const *) lines->pdata,
                               _("Add"), _("Added"), about, add_event, NULL, ical);
  dictate_action_set_log (action, "propose_event", NULL);
  tool_run_propose (run, action);
  json_builder_begin_object (b);
  add_string (b, "shown", "A card on the screen with the event and an Add button.");
  add_string (b, "when", when);
  add_string (b, "status", "Not added yet: it is added only when the user taps Add.");
  return finish (b);
}

/* ---------------- the call going on (the Phone's call service, dictate-calls.h) ---------------- */

static GVariant *
dialer_call (const char *method, GVariant *parameters, ToolRun *run, GError **error)
{
  g_autoptr (GDBusConnection) bus = dictate_calls_bus (tool_run_cancellable (run), error);

  if (!bus)
    {
      if (parameters)
        g_variant_unref (g_variant_ref_sink (parameters));
      return NULL;
    }
  return g_dbus_connection_call_sync (bus, DICTATE_CALLS_NAME, DICTATE_CALLS_PATH, DICTATE_CALLS_INTERFACE, method,
                                      parameters, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, DBUS_TIMEOUT_MS,
                                      tool_run_cancellable (run), error);
}

/* the service's properties: the calls not over (DictateCall), and the microphone and the speaker */
static GPtrArray *
dialer_calls (ToolRun *run, gboolean *muted, gboolean *speaker, gboolean *can_hold, GError **error)
{
  g_autoptr (GDBusConnection) bus = dictate_calls_bus (tool_run_cancellable (run), error);
  g_autoptr (GVariant) reply = NULL, props = NULL, calls = NULL;
  g_autofree const char **features = NULL;

  if (!bus)
    return NULL;
  reply = g_dbus_connection_call_sync (bus, DICTATE_CALLS_NAME, DICTATE_CALLS_PATH, "org.freedesktop.DBus.Properties",
                                       "GetAll", g_variant_new ("(s)", DICTATE_CALLS_INTERFACE),
                                       G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, DBUS_TIMEOUT_MS,
                                       tool_run_cancellable (run), error);
  if (!reply)
    {
      g_clear_error (error);
      tool_error (error, "The Phone's call service is not running, so there is no call.");
      return NULL;
    }
  props = g_variant_get_child_value (reply, 0);
  calls = g_variant_lookup_value (props, "Calls", G_VARIANT_TYPE ("aa{sv}"));
  if (muted && !g_variant_lookup (props, "Muted", "b", muted))
    *muted = FALSE;
  if (speaker && !g_variant_lookup (props, "Speaker", "b", speaker))
    *speaker = FALSE;
  if (can_hold)
    {
      *can_hold = FALSE;
      if (g_variant_lookup (props, "Features", "^a&s", &features))
        *can_hold = g_strv_contains (features, "hold");
    }
  return dictate_calls_parse (calls);
}

/* who a call is with, for the model: a contact's name, never the number (`known`: in the contacts) */
static char *
call_with (DictateCall *call, GPtrArray *all, gboolean *known)
{
  Person *p = call->number && *call->number ? person_for (all, call->number, NULL) : NULL;

  *known = p != NULL;
  if (p)
    return g_strdup (p->name);
  if (call->presentation == 2)
    return g_strdup ("a private number");
  if (!call->number || !*call->number)
    return g_strdup ("an unknown caller");
  return g_strdup ("an unknown number (not in the contacts)");
}

static const char *
call_state (DictateCall *call)
{
  switch (call->state)
    {
    case DICTATE_CALL_CONNECTING:
    case DICTATE_CALL_DIALING:
      return "calling, not answered yet";
    case DICTATE_CALL_INCOMING:
      return "ringing";
    case DICTATE_CALL_WAITING:
      return "ringing while another call goes on";
    case DICTATE_CALL_ACTIVE:
      return "going on";
    case DICTATE_CALL_ONHOLD:
      return "on hold";
    default:
      return "ending";
    }
}

/* the call `pick` means: the one ringing, or the one going on (else the one on hold), or any */
typedef enum { PICK_RINGING, PICK_TALKING } Pick;

static DictateCall *
pick_call (GPtrArray *calls, Pick pick)
{
  DictateCall *held = NULL, *dialing = NULL;

  for (guint i = 0; calls && i < calls->len; i++)
    {
      DictateCall *c = calls->pdata[i];
      if (pick == PICK_RINGING && dictate_call_is_ringing (c))
        return c;
      if (pick == PICK_TALKING && c->state == DICTATE_CALL_ACTIVE)
        return c;
      if (c->state == DICTATE_CALL_ONHOLD && !held)
        held = c;
      if ((c->state == DICTATE_CALL_DIALING || c->state == DICTATE_CALL_CONNECTING) && !dialing)
        dialing = c;
    }
  if (pick == PICK_TALKING)
    return dialing ? dialing : held;
  return NULL;
}

static JsonNode *
get_current_call (JsonObject *args, ToolRun *run, GError **error)
{
  gboolean muted = FALSE, speaker = FALSE, can_hold = FALSE;
  g_autoptr (GPtrArray) calls = dialer_calls (run, &muted, &speaker, &can_hold, error);
  g_autoptr (GPtrArray) all = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  gint64 now = g_get_real_time () / 1000;

  if (!calls)
    return NULL;
  json_builder_begin_object (b);
  if (!calls->len)
    {
      add_string (b, "note", "No call is ringing or going on.");
      return finish (b);
    }
  all = people_with_numbers (run);
  json_builder_set_member_name (b, "calls");
  json_builder_begin_array (b);
  for (guint i = 0; i < calls->len; i++)
    {
      DictateCall *c = calls->pdata[i];
      gboolean known;
      g_autofree char *with = call_with (c, all, &known);

      json_builder_begin_object (b);
      add_string (b, "with", with);
      add_bool (b, "in_contacts", known);
      if (c->presentation == 2)
        add_bool (b, "private_number", TRUE);
      add_string (b, "state", call_state (c));
      add_string (b, "direction", c->direction == 1 ? "incoming" : "outgoing");
      if (c->connected > 0 && c->state != DICTATE_CALL_INCOMING)
        {
          g_autofree char *length = duration_text ((now - c->connected) / 1000);
          add_string (b, "for", length);
        }
      /* the digits only when the user asked for the number */
      if (tool_arg_bool (args, "include_number", FALSE) && c->presentation == 1 && c->number && *c->number)
        add_string (b, "number", c->number);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  add_bool (b, "microphone_muted", muted);
  add_bool (b, "speaker_on", speaker);
  add_bool (b, "can_hold", can_hold);
  return finish (b);
}

/* what call_control's undo does: the opposite, while the call is still there */
static const char *
call_opposite (const char *action)
{
  static const char *const pairs[][2] = { { "speaker_on", "speaker_off" }, { "mute", "unmute" },
                                          { "hold", "resume" } };

  for (guint i = 0; i < G_N_ELEMENTS (pairs); i++)
    {
      if (!strcmp (action, pairs[i][0]))
        return pairs[i][1];
      if (!strcmp (action, pairs[i][1]))
        return pairs[i][0];
    }
  return NULL;
}

/* one of call_control's actions on the call going on; `about` what was done, for the model */
static gboolean
call_do (const char *action, ToolRun *run, gboolean *changed, char **about, GError **error)
{
  gboolean muted = FALSE, speaker = FALSE, can_hold = FALSE;
  g_autoptr (GPtrArray) calls = dialer_calls (run, &muted, &speaker, &can_hold, error);
  g_autoptr (GVariant) reply = NULL;
  DictateCall *call;

  if (!calls)
    return FALSE;
  *changed = TRUE;
  call = pick_call (calls, PICK_TALKING);
  if (!call && !strcmp (action, "hang_up"))
    call = pick_call (calls, PICK_RINGING);
  if (!call)
    {
      tool_error (error, "No call is going on.");
      return FALSE;
    }
  if (!strcmp (action, "speaker_on") || !strcmp (action, "speaker_off"))
    {
      gboolean on = !strcmp (action, "speaker_on");
      *changed = on != speaker;
      reply = dialer_call ("SetSpeaker", g_variant_new ("(b)", on), run, error);
      *about = g_strdup (on ? "the speaker turned on" : "the speaker turned off");
    }
  else if (!strcmp (action, "mute") || !strcmp (action, "unmute"))
    {
      gboolean on = !strcmp (action, "mute");
      *changed = on != muted;
      reply = dialer_call ("SetMuted", g_variant_new ("(b)", on), run, error);
      *about = g_strdup (on ? "the microphone muted" : "the microphone on again");
    }
  else if (!strcmp (action, "hold") || !strcmp (action, "resume"))
    {
      gboolean on = !strcmp (action, "hold");
      if (!can_hold)
        {
          tool_error (error, "This phone cannot hold calls.");
          return FALSE;
        }
      *changed = on != (call->state == DICTATE_CALL_ONHOLD);
      reply = dialer_call ("SetHeld", g_variant_new ("(ub)", call->id, on), run, error);
      *about = g_strdup (on ? "the call put on hold" : "the call taken off hold");
    }
  else if (!strcmp (action, "hang_up"))
    {
      reply = dialer_call ("Hangup", g_variant_new ("(u)", call->id), run, error);
      *about = g_strdup ("the call hung up");
    }
  else
    {
      tool_error (error, "action must be one of speaker_on, speaker_off, mute, unmute, hold, resume, hang_up.");
      return FALSE;
    }
  if (!reply && error && *error)
    g_dbus_error_strip_remote_error (*error);
  return reply != NULL;
}

static JsonNode *
call_control (JsonObject *args, ToolRun *run, GError **error)
{
  const char *action = tool_arg_string (args, "action");
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *about = NULL;
  gboolean changed;

  if (!action)
    {
      tool_error (error, "Which action is missing.");
      return NULL;
    }
  if (!call_do (action, run, &changed, &about, error))
    return NULL;
  json_builder_begin_object (b);
  add_string (b, "done", about);
  if (!strcmp (action, "hang_up"))
    add_string (b, "note", "Hung up at once; that cannot be undone. Say that it is done.");
  else if (!changed)
    {
      add_string (b, "note", "It was like that already.");
      tool_run_set_unchanged (run);
    }
  else
    {
      g_autoptr (JsonBuilder) u = json_builder_new ();

      json_builder_begin_object (u);
      add_string (u, "call_action", call_opposite (action));
      add_string (u, "about", about);
      tool_run_set_undo (run, finish (u));
      add_bool (b, "can_undo", TRUE);
    }
  return finish (b);
}

static JsonNode *
answer_call (JsonObject *args, ToolRun *run, GError **error)
{
  g_autoptr (GPtrArray) calls = dialer_calls (run, NULL, NULL, NULL, error);
  g_autoptr (GPtrArray) all = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *with = NULL;
  DictateCall *call;
  gboolean known;

  if (!calls)
    return NULL;
  if (!(call = pick_call (calls, PICK_RINGING)))
    {
      tool_error (error, "No call is ringing.");
      return NULL;
    }
  if (!(reply = dialer_call ("Answer", g_variant_new ("(u)", call->id), run, error)))
    {
      g_dbus_error_strip_remote_error (*error);
      return NULL;
    }
  all = people_with_numbers (run);
  with = call_with (call, all, &known);
  json_builder_begin_object (b);
  add_string (b, "answered", with);
  add_string (b, "note", "Answered at once; the call is going on now.");
  return finish (b);
}

static JsonNode *
decline_call (JsonObject *args, ToolRun *run, GError **error)
{
  const char *text = tool_arg_string (args, "text");
  g_autoptr (GPtrArray) calls = dialer_calls (run, NULL, NULL, NULL, error);
  g_autoptr (GPtrArray) all = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autofree char *with = NULL, *number = NULL;
  DictateCall *call;
  gboolean known;

  if (!calls)
    return NULL;
  if (!(call = pick_call (calls, PICK_RINGING)))
    {
      tool_error (error, "No call is ringing.");
      return NULL;
    }
  number = g_strdup (call->number);
  if (!(reply = dialer_call ("Decline", g_variant_new ("(u)", call->id), run, error)))
    {
      g_dbus_error_strip_remote_error (*error);
      return NULL;
    }
  all = people_with_numbers (run);
  with = call_with (call, all, &known);
  json_builder_begin_object (b);
  add_string (b, "declined", with);
  if (text && *text)
    {
      if (!number || !*number || call->presentation != 1)
        add_string (b, "message", "Not prepared: the caller's number did not come with the call.");
      else if (!tool_run_area_on (run, "access-messages"))
        add_string (b, "message", "Not prepared: the user has turned off messages in Agent's settings.");
      else
        {
          Person *p = person_for (all, number, NULL);
          g_autofree char *line = p ? g_strdup_printf ("%s · %s", p->name, number) : g_strdup (number);
          g_autofree char *quoted = g_strdup_printf ("“%s”", text);
          g_autofree char *about = g_strdup_printf ("the message to %s", known ? with : "the caller");
          const char *lines[] = { line, quoted, NULL };
          g_autoptr (DictateAction) action = dictate_action_new ("send a message", _("Send a message"), lines,
                                                                 _("Send"), _("Sent"), about, send_message, number,
                                                                 text);

          dictate_action_set_log (action, "propose_message", "the message, by SMS to its recipient");
          tool_run_propose (run, action);
          add_string (b, "message_to", known ? with : "the caller");
          add_string (b, "message", "A card on the screen with the message and a Send button. Not sent yet: it is "
                                    "sent only when the user taps Send.");
        }
    }
  return finish (b);
}

/* ---------------- the table ---------------- */

static const DictateTool TOOLS[] = {
  { "get_time", NULL, NULL,
    "The current time, date, weekday and time zone on the phone.",
    "{\"type\":\"object\",\"properties\":{}}", get_time,
    DICTATE_TIER_READ, "told the time", FALSE },
  { "get_battery", NULL, N_("Checking the battery…"),
    "The battery's charge in percent, whether it is charging, and how long it lasts.",
    "{\"type\":\"object\",\"properties\":{}}", get_battery,
    DICTATE_TIER_READ, "read the battery", FALSE },
  { "get_storage", NULL, N_("Checking the storage…"),
    "How much storage space is free and used on the phone, in GB.",
    "{\"type\":\"object\",\"properties\":{}}", get_storage,
    DICTATE_TIER_READ, "read the storage", FALSE },
  { "get_recent_calls", "access-calls", N_("Checking your calls…"),
    "The phone's call history, newest first: who called or was called, whether it was missed, when, and how long.",
    "{\"type\":\"object\",\"properties\":{"
    "\"count\":{\"type\":\"integer\",\"description\":\"How many calls, 1 to 10. Default 5.\"},"
    "\"missed_only\":{\"type\":\"boolean\",\"description\":\"Only missed calls.\"},"
    "\"contact\":{\"type\":\"string\",\"description\":\"Only calls with this person: a contact's name or a number.\"}"
    "}}", get_recent_calls,
    DICTATE_TIER_READ, "read the call history", FALSE },
  { "propose_call", "access-calls", N_("Getting the call ready…"),
    "Prepares a phone call: shows the user a card with whom it will call. The call starts only when the user taps "
    "Call on the card; this tool never calls by itself.",
    "{\"type\":\"object\",\"properties\":{"
    "\"to\":{\"type\":\"string\",\"description\":\"A contact's name, or a phone number.\"},"
    "\"number_type\":{\"type\":\"string\",\"enum\":[\"mobile\",\"home\",\"work\"],"
    "\"description\":\"Which of the contact's numbers, only if the user said.\"}"
    "},\"required\":[\"to\"]}", propose_call,
    DICTATE_TIER_CONFIRMED, "a phone call, on a card", FALSE },
  { "find_contacts", "access-contacts", N_("Looking in your contacts…"),
    "Looks people up in the phone's contacts by name. Gives their names and the kinds of numbers they have "
    "(mobile, home, work); the numbers, emails and notes themselves only when asked for with include_numbers, "
    "include_emails or include_note.",
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"A name or part of one.\"},"
    "\"include_numbers\":{\"type\":\"boolean\",\"description\":\"Only when the user wants to know a number.\"},"
    "\"include_emails\":{\"type\":\"boolean\",\"description\":\"Only when the user wants to know an email address.\"},"
    "\"include_note\":{\"type\":\"boolean\",\"description\":\"Only when the user asks what the contact's note says.\"}"
    "},\"required\":[\"name\"]}", find_contacts,
    DICTATE_TIER_READ, "looked in the contacts", TRUE },
  { "get_messages", "access-messages", N_("Checking your messages…"),
    "Text messages (SMS), the user's and the notices companies send by text: a parcel's delivery, a code, an "
    "appointment. Without contact: the latest conversations, with whom, when, and how many are unread. With "
    "contact: the latest messages with that person or sender. What a message says only with include_text, when the "
    "user wants to know what was written.",
    "{\"type\":\"object\",\"properties\":{"
    "\"contact\":{\"type\":\"string\",\"description\":\"A contact's name, a number, or a sender's name.\"},"
    "\"count\":{\"type\":\"integer\",\"description\":\"How many, 1 to 10.\"},"
    "\"unread_only\":{\"type\":\"boolean\",\"description\":\"Only unread messages.\"},"
    "\"include_text\":{\"type\":\"boolean\",\"description\":\"Include what the messages say.\"}"
    "}}", get_messages,
    DICTATE_TIER_READ, "read the messages", TRUE },
  { "propose_message", "access-messages", N_("Writing the message…"),
    "Prepares a text message (SMS): shows the user a card with the recipient and the text. It is sent only when the "
    "user taps Send on the card; this tool never sends by itself.",
    "{\"type\":\"object\",\"properties\":{"
    "\"to\":{\"type\":\"string\",\"description\":\"A contact's name, or a phone number.\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"The message, as the user wants it written.\"}"
    "},\"required\":[\"to\",\"text\"]}", propose_message,
    DICTATE_TIER_CONFIRMED, "a text message, on a card", FALSE },
  { "get_events", "access-calendar", N_("Checking your calendar…"),
    "The events in the phone's calendar on a day, or on several days from it.",
    "{\"type\":\"object\",\"properties\":{"
    "\"date\":{\"type\":\"string\",\"description\":\"The first day, YYYY-MM-DD. Default today.\"},"
    "\"days\":{\"type\":\"integer\",\"description\":\"How many days, 1 to 14. Default 1.\"}"
    "}}", get_events,
    DICTATE_TIER_READ, "read the calendar", TRUE },
  { "get_alarms", "access-alarms", N_("Checking your alarms…"),
    "The alarms set in the Clock app, and the timers running.",
    "{\"type\":\"object\",\"properties\":{}}", get_alarms,
    DICTATE_TIER_READ, "read the alarms", FALSE },
  { "set_alarm", "access-alarms", N_("Setting the alarm…"),
    "Sets an alarm in the Clock app.",
    "{\"type\":\"object\",\"properties\":{"
    "\"time\":{\"type\":\"string\",\"description\":\"HH:MM, 24-hour.\"},"
    "\"days\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"monday\",\"tuesday\",\"wednesday\","
    "\"thursday\",\"friday\",\"saturday\",\"sunday\"]},"
    "\"description\":\"The days it repeats on; leave it out for an alarm that rings once.\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"A name for it, only if the user gave one.\"}"
    "},\"required\":[\"time\"]}", set_alarm,
    DICTATE_TIER_UNDOABLE, "set an alarm", FALSE },
  { "set_timer", "access-alarms", N_("Starting the timer…"),
    "Starts a countdown timer in the Clock app.",
    "{\"type\":\"object\",\"properties\":{"
    "\"minutes\":{\"type\":\"number\",\"description\":\"How long, in minutes.\"},"
    "\"seconds\":{\"type\":\"integer\",\"description\":\"Seconds, added to the minutes.\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"A name for it, only if the user gave one.\"}"
    "}}", set_timer,
    DICTATE_TIER_UNDOABLE, "started a timer", FALSE },
  { "get_wifi_bluetooth", "access-settings", N_("Checking Wi-Fi and Bluetooth…"),
    "Whether Wi-Fi and Bluetooth are on, the Wi-Fi network the phone is connected to, and the Bluetooth devices "
    "connected.",
    "{\"type\":\"object\",\"properties\":{}}", get_wifi_bluetooth,
    DICTATE_TIER_READ, "read Wi-Fi and Bluetooth", TRUE },
  { "set_wifi_bluetooth", "access-settings", N_("Changing the setting…"),
    "Turns Wi-Fi or Bluetooth on or off.",
    "{\"type\":\"object\",\"properties\":{"
    "\"radio\":{\"type\":\"string\",\"enum\":[\"wifi\",\"bluetooth\"]},"
    "\"on\":{\"type\":\"boolean\",\"description\":\"true to turn it on, false to turn it off.\"}"
    "},\"required\":[\"radio\",\"on\"]}", set_wifi_bluetooth,
    DICTATE_TIER_UNDOABLE, "changed a setting", FALSE },
  { "get_running_apps", "access-apps", N_("Checking your apps…"),
    "The apps open on the phone right now, as in the recent apps.",
    "{\"type\":\"object\",\"properties\":{}}", get_running_apps,
    DICTATE_TIER_READ, "read the open apps", FALSE },
  { "open_app", "access-apps", N_("Opening the app…"),
    "Opens an app on the phone.",
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"The app's name, such as Camera, Clock or Settings.\"}"
    "},\"required\":[\"name\"]}", open_app,
    DICTATE_TIER_UNDOABLE, "opened an app", FALSE },
  { "navigate_to", "access-location", N_("Finding the way…"),
    "Opens Navi, the maps app, with routes to a place: an address or the name of a place.",
    "{\"type\":\"object\",\"properties\":{"
    "\"place\":{\"type\":\"string\",\"description\":\"The address or the place's name, with the town if known.\"}"
    "},\"required\":[\"place\"]}", navigate_to,
    DICTATE_TIER_UNDOABLE, "opened Navi with routes", FALSE },
  { "cancel_alarm", "access-alarms", N_("Removing the alarm…"),
    "Removes an alarm from the Clock app. It can be undone. Without time and label when the user means their only "
    "alarm.",
    "{\"type\":\"object\",\"properties\":{"
    "\"time\":{\"type\":\"string\",\"description\":\"The alarm's time, HH:MM, 24-hour.\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"Its name, only if the user said it.\"}"
    "}}", cancel_alarm,
    DICTATE_TIER_UNDOABLE, "removed an alarm", FALSE },
  { "cancel_timer", "access-alarms", N_("Stopping the timer…"),
    "Stops and removes a running timer in the Clock app. It can be undone.",
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Its name, only if the user said it.\"}"
    "}}", cancel_timer,
    DICTATE_TIER_UNDOABLE, "stopped a timer", FALSE },
  { "undo_last_action", NULL, N_("Undoing it…"),
    "Undoes the last thing Agent did that can be undone, within a day: removes the alarm or timer it set, puts back "
    "an alarm or timer it removed, turns Wi-Fi or Bluetooth back as they were, or puts a call's speaker, microphone or "
    "hold back. For \"undo\", \"undo that\", \"take that back\", \"put it back\", \"oops\": call it rather than "
    "doing the thing again yourself, it puts back exactly what was there.",
    "{\"type\":\"object\",\"properties\":{}}", undo_last_action,
    DICTATE_TIER_UNDOABLE, "undid the last action", FALSE },
  { "get_action_log", NULL, N_("Checking what I did…"),
    "What Agent itself did lately, newest first: the actions it took or offered on a card (an alarm, a setting, a "
    "message), when, how each went, whether it was undone, and where anything went off the phone. Always call it "
    "for \"what did you just do?\", \"what have you done?\" and \"did that go to the server?\", even when you "
    "think you remember: the conversation does not hold what was done before it, or on a card.",
    "{\"type\":\"object\",\"properties\":{"
    "\"count\":{\"type\":\"integer\",\"description\":\"How many actions, 1 to 10. Default 5.\"}"
    "}}", get_action_log,
    DICTATE_TIER_READ, "read Agent's log", FALSE },
  { "get_mobile_network", "access-settings", N_("Checking the mobile network…"),
    "The mobile network: whether there is a SIM, the operator, the signal in bars (of 4), the network type (4G, "
    "5G), whether the phone is roaming, and whether mobile data is on.",
    "{\"type\":\"object\",\"properties\":{}}", get_mobile_network,
    DICTATE_TIER_READ, "read the mobile network", FALSE },
  { "get_weather", "access-location", N_("Checking the weather…"),
    "The weather now and the forecast for the next days: the temperature, the sky, the wind, the chance of rain. "
    "Where the phone is (the town the Weather app shows), or in a town the user names.",
    "{\"type\":\"object\",\"properties\":{"
    "\"place\":{\"type\":\"string\",\"description\":\"A town, only if the user named one.\"},"
    "\"days\":{\"type\":\"integer\",\"description\":\"How many days from today, 1 to 7. Default 2.\"}"
    "}}", get_weather,
    DICTATE_TIER_READ, "looked up the weather", FALSE },
  { "what_did_i_miss", NULL, N_("Checking what you missed…"),
    "What the user missed since they last asked this: missed calls, unread text messages, and the other apps' "
    "notifications, as counts and from whom or which app. Tell the user that much in two sentences at most, the "
    "names with the counts (\"A missed call from Kim and two messages from Lee. Two notifications, from Mail and "
    "News.\"). What the messages and notifications say only with include_text.",
    "{\"type\":\"object\",\"properties\":{"
    "\"include_text\":{\"type\":\"boolean\",\"description\":\"Only when the user's own words ask to have them "
    "read out (\\\"read them to me\\\"); never for \\\"what did I miss?\\\" alone.\"}"
    "}}", what_did_i_miss,
    DICTATE_TIER_READ, "read what was missed", TRUE },
  { "propose_contact", "access-contacts", N_("Getting the contact ready…"),
    "Prepares a new contact: shows the user a card with the name and the number or email. It is added only when the "
    "user taps Add on the card; this tool never adds by itself.",
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"The person's name.\"},"
    "\"number\":{\"type\":\"string\",\"description\":\"Their phone number, as the user said it.\"},"
    "\"number_type\":{\"type\":\"string\",\"enum\":[\"mobile\",\"home\",\"work\"]},"
    "\"email\":{\"type\":\"string\"}"
    "},\"required\":[\"name\"]}", propose_contact,
    DICTATE_TIER_CONFIRMED, "a new contact, on a card", FALSE },
  { "propose_event", "access-calendar", N_("Getting the event ready…"),
    "Prepares a calendar event or a reminder: shows the user a card with what, when and where. It is added only "
    "when the user taps Add on the card; this tool never adds by itself. A reminder is an event with "
    "reminder_minutes.",
    "{\"type\":\"object\",\"properties\":{"
    "\"title\":{\"type\":\"string\",\"description\":\"What it is.\"},"
    "\"date\":{\"type\":\"string\",\"description\":\"The day, YYYY-MM-DD.\"},"
    "\"time\":{\"type\":\"string\",\"description\":\"When it starts, HH:MM, 24-hour; leave it out for the whole day.\"},"
    "\"minutes\":{\"type\":\"integer\",\"description\":\"How long, in minutes. Default 60.\"},"
    "\"where\":{\"type\":\"string\",\"description\":\"Where, only if the user said.\"},"
    "\"reminder_minutes\":{\"type\":\"integer\",\"description\":\"A notification this many minutes before it (0: at "
    "the time); leave it out for none.\"}"
    "},\"required\":[\"title\",\"date\"]}", propose_event,
    DICTATE_TIER_CONFIRMED, "a calendar event, on a card", FALSE },
  { "get_current_call", "access-calls", N_("Checking the call…"),
    "The phone call ringing or going on now: who it is with (a contact's name, or that the number is not in the "
    "contacts or is private), whether it rings, goes on or is on hold, for how long, and whether the microphone is "
    "muted and the speaker on. For \"who is this?\" and \"who is calling?\". Never say a number's digits unless "
    "the user asked for the number; then call it with include_number.",
    "{\"type\":\"object\",\"properties\":{"
    "\"include_number\":{\"type\":\"boolean\",\"description\":\"Only when the user asks for the number.\"}"
    "}}", get_current_call,
    DICTATE_TIER_READ, "checked the call", FALSE },
  { "call_control", "access-calls", N_("Changing the call…"),
    "Changes the call going on, at once: the speaker on or off, the microphone muted or on again, the call put on "
    "hold or taken off it, or the call hung up. Hanging up cannot be undone; say that it is done. The others can be "
    "undone.",
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"speaker_on\",\"speaker_off\",\"mute\",\"unmute\","
    "\"hold\",\"resume\",\"hang_up\"]}"
    "},\"required\":[\"action\"]}", call_control,
    DICTATE_TIER_UNDOABLE, "changed the call", FALSE },
  { "answer_call", "access-calls", N_("Answering…"),
    "Answers the call ringing now, at once. Only when the user says to answer it.",
    "{\"type\":\"object\",\"properties\":{}}", answer_call,
    DICTATE_TIER_UNDOABLE, "answered a call", FALSE },
  { "decline_call", "access-calls", N_("Declining…"),
    "Declines the call ringing now, at once. With text, a text message to the caller is then prepared on a card "
    "(\"decline and text I'll call back\"); it is sent only when the user taps Send.",
    "{\"type\":\"object\",\"properties\":{"
    "\"text\":{\"type\":\"string\",\"description\":\"A message to send the caller, only if the user said one.\"}"
    "}}", decline_call,
    DICTATE_TIER_UNDOABLE, "declined a call", FALSE },
};

const DictateTool *
dictate_tools_table (guint *n)
{
  *n = G_N_ELEMENTS (TOOLS);
  return TOOLS;
}
