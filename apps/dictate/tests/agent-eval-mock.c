/* agent-eval-mock: a private copy of a phone for agent-eval.c, made from tests/agent-eval/fixtures.json, so that
 * Agent's tools read made-up data and never the phone's own.
 *
 * Run it in a private environment (run.sh eval does: a dbus-run-session of its own, HOME and the XDG directories
 * in a new directory, the system bus pointed at that same private bus):
 *  - the stores the tools read, written where the apps keep them: the Dialer's calls.db and Messaging's bugle_db in
 *    XDG_DATA_HOME, the contacts and the events in EDS's built-in address book and calendar (EDS started on the
 *    private bus by D-Bus activation, its files in the private XDG directories);
 *  - the services the tools ask, stood in for on the private bus: NetworkManager, BlueZ and UPower (as on the
 *    system bus), the Clock's alarms and timers and the shell's open apps (on the session bus). Turning Wi-Fi or
 *    Bluetooth off, setting an alarm, starting a timer change only these. */
#include "agent-eval-mock.h"

#include <glib/gstdio.h>
#include <libebook/libebook.h>
#include <libecal/libecal.h>
#include <sqlite3.h>
#include <string.h>

static JsonObject *fixture;     /* the fixtures' "phone" */

/* ---------------- the services ---------------- */

static gboolean wifi_on, bt_on;
static GVariant *alarms;        /* aa{sv} */
static GPtrArray *timers;       /* GVariant a{sv} */
static guint launched;

static const char NM_XML[] =
  "<node>"
  " <interface name='org.freedesktop.NetworkManager'>"
  "  <property name='WirelessEnabled' type='b' access='readwrite'/>"
  "  <property name='WirelessHardwareEnabled' type='b' access='read'/>"
  "  <property name='NetworkingEnabled' type='b' access='read'/>"
  "  <property name='ActiveConnections' type='ao' access='read'/>"
  "  <property name='Connectivity' type='u' access='read'/>"
  "  <property name='State' type='u' access='read'/>"
  " </interface>"
  " <interface name='org.freedesktop.NetworkManager.Connection.Active'>"
  "  <property name='Type' type='s' access='read'/>"
  "  <property name='Id' type='s' access='read'/>"
  "  <property name='State' type='u' access='read'/>"
  " </interface>"
  "</node>";

static const char BLUEZ_XML[] =
  "<node>"
  " <interface name='org.freedesktop.DBus.ObjectManager'>"
  "  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
  " </interface>"
  " <interface name='org.bluez.Adapter1'>"
  "  <property name='Powered' type='b' access='readwrite'/>"
  " </interface>"
  "</node>";

static const char UPOWER_XML[] =
  "<node>"
  " <interface name='org.freedesktop.UPower.Device'>"
  "  <property name='Percentage' type='d' access='read'/>"
  "  <property name='State' type='u' access='read'/>"
  "  <property name='TimeToEmpty' type='x' access='read'/>"
  "  <property name='TimeToFull' type='x' access='read'/>"
  "  <property name='IsPresent' type='b' access='read'/>"
  " </interface>"
  "</node>";

static const char CLOCK_XML[] =
  "<node>"
  " <interface name='vibeos.Clock.Alarms'>"
  "  <method name='GetAlarms'><arg type='aa{sv}' direction='out'/></method>"
  "  <method name='AddAlarm'><arg type='a{sv}' direction='in'/><arg type='u' direction='out'/></method>"
  " </interface>"
  " <interface name='vibeos.Clock.Timers'>"
  "  <method name='GetTimers'><arg type='aa{sv}' direction='out'/></method>"
  "  <method name='AddTimer'><arg type='x' direction='in'/><arg type='s' direction='in'/>"
  "   <arg type='b' direction='in'/><arg type='u' direction='out'/></method>"
  "  <method name='Start'><arg type='u' direction='in'/></method>"
  " </interface>"
  "</node>";

static const char SHELL_XML[] =
  "<node>"
  " <interface name='vibeos.Shell'>"
  "  <method name='RunningApps'><arg type='a(ss)' direction='out'/></method>"
  "  <method name='Launch'><arg type='s' direction='in'/><arg type='(dddd)' direction='in'/>"
  "   <arg type='s' direction='in'/></method>"
  " </interface>"
  "</node>";

static JsonObject *
member_object (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_object_member (o, name) : NULL;
}

static JsonArray *
member_array (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
}

static const char *
member_string (JsonObject *o, const char *name, const char *fallback)
{
  return o && json_object_has_member (o, name) ? json_object_get_string_member (o, name) : fallback;
}

static gint64
member_int (JsonObject *o, const char *name, gint64 fallback)
{
  return o && json_object_has_member (o, name) ? json_object_get_int_member (o, name) : fallback;
}

static gboolean
member_bool (JsonObject *o, const char *name, gboolean fallback)
{
  return o && json_object_has_member (o, name) ? json_object_get_boolean_member (o, name) : fallback;
}

static GVariant *
nm_get (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *property,
        GError **error, gpointer data)
{
  JsonObject *wifi = member_object (fixture, "wifi");
  const char *network = member_string (wifi, "network", NULL);

  if (!strcmp (interface, "org.freedesktop.NetworkManager.Connection.Active"))
    {
      if (!strcmp (property, "Type"))
        return g_variant_new_string ("802-11-wireless");
      if (!strcmp (property, "Id"))
        return g_variant_new_string (network ? network : "");
      return g_variant_new_uint32 (wifi_on && network ? 2 : 4);
    }
  if (!strcmp (property, "WirelessEnabled"))
    return g_variant_new_boolean (wifi_on);
  if (!strcmp (property, "WirelessHardwareEnabled") || !strcmp (property, "NetworkingEnabled"))
    return g_variant_new_boolean (TRUE);
  if (!strcmp (property, "ActiveConnections"))
    {
      const char *one[] = { "/org/freedesktop/NetworkManager/ActiveConnection/1", NULL };
      return g_variant_new_objv (wifi_on && network ? one : NULL, wifi_on && network ? 1 : 0);
    }
  if (!strcmp (property, "Connectivity"))
    return g_variant_new_uint32 (4);
  return g_variant_new_uint32 (70);
}

static gboolean
nm_set (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *property,
        GVariant *value, GError **error, gpointer data)
{
  wifi_on = g_variant_get_boolean (value);
  g_message ("mock: Wi-Fi %s", wifi_on ? "on" : "off");
  return TRUE;
}

static GVariant *
bluez_objects (void)
{
  GVariantBuilder objects, adapter;
  JsonArray *devices = member_array (member_object (fixture, "bluetooth"), "connected");

  g_variant_builder_init (&objects, G_VARIANT_TYPE ("a{oa{sa{sv}}}"));
  g_variant_builder_init (&adapter, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&adapter, "{sv}", "Powered", g_variant_new_boolean (bt_on));
  g_variant_builder_add (&objects, "{o@a{sa{sv}}}", "/org/bluez/hci0",
                         g_variant_new_parsed ("{'org.bluez.Adapter1': %@a{sv}}", g_variant_builder_end (&adapter)));
  for (guint i = 0; devices && i < json_array_get_length (devices); i++)
    {
      g_autofree char *path = g_strdup_printf ("/org/bluez/hci0/dev_%u", i);
      g_variant_builder_add (&objects, "{o@a{sa{sv}}}", path,
                             g_variant_new_parsed ("{'org.bluez.Device1': {'Alias': <%s>, 'Connected': <%b>}}",
                                                   json_array_get_string_element (devices, i), bt_on));
    }
  return g_variant_builder_end (&objects);
}

static void
bluez_call (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *method,
            GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  g_dbus_method_invocation_return_value (invocation, g_variant_new_tuple ((GVariant *[]) { bluez_objects () }, 1));
}

static GVariant *
bluez_get (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *property,
           GError **error, gpointer data)
{
  return g_variant_new_boolean (bt_on);
}

static gboolean
bluez_set (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *property,
           GVariant *value, GError **error, gpointer data)
{
  bt_on = g_variant_get_boolean (value);
  g_message ("mock: Bluetooth %s", bt_on ? "on" : "off");
  return TRUE;
}

static GVariant *
upower_get (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *property,
            GError **error, gpointer data)
{
  JsonObject *battery = member_object (fixture, "battery");

  if (!strcmp (property, "Percentage"))
    return g_variant_new_double (member_int (battery, "percent", 50));
  if (!strcmp (property, "State"))
    return g_variant_new_uint32 (member_bool (battery, "charging", FALSE) ? 1 : 2);
  if (!strcmp (property, "TimeToEmpty"))
    return g_variant_new_int64 (member_int (battery, "minutes_left", 0) * 60);
  if (!strcmp (property, "TimeToFull"))
    return g_variant_new_int64 (member_int (battery, "minutes_to_full", 0) * 60);
  return g_variant_new_boolean (TRUE);
}

static void
clock_call (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *method,
            GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  if (!strcmp (method, "GetAlarms"))
    g_dbus_method_invocation_return_value (invocation, g_variant_new_tuple (&alarms, 1));
  else if (!strcmp (method, "AddAlarm"))
    {
      GVariantBuilder all;
      GVariantIter iter;
      GVariant *a;
      g_autoptr (GVariant) added = g_variant_get_child_value (parameters, 0);

      g_variant_builder_init (&all, G_VARIANT_TYPE ("aa{sv}"));
      g_variant_iter_init (&iter, alarms);
      while ((a = g_variant_iter_next_value (&iter)))
        {
          g_variant_builder_add_value (&all, a);
          g_variant_unref (a);
        }
      g_variant_builder_add_value (&all, added);
      g_variant_unref (alarms);
      alarms = g_variant_ref_sink (g_variant_builder_end (&all));
      g_message ("mock: an alarm added");
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", 100 + g_variant_n_children (alarms)));
    }
  else if (!strcmp (method, "GetTimers"))
    {
      GVariantBuilder all;
      g_variant_builder_init (&all, G_VARIANT_TYPE ("aa{sv}"));
      for (guint i = 0; i < timers->len; i++)
        g_variant_builder_add_value (&all, timers->pdata[i]);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(aa{sv})", &all));
    }
  else if (!strcmp (method, "AddTimer"))
    {
      gint64 length;
      const char *label;
      gboolean once;

      g_variant_get (parameters, "(x&sb)", &length, &label, &once);
      g_ptr_array_add (timers, g_variant_ref_sink (g_variant_new_parsed (
        "{'id': <%u>, 'state': <uint32 4>, 'length': <%x>, 'remaining': <%x>, 'label': <%s>}", timers->len + 1, length,
        length, label)));
      g_message ("mock: a timer added");
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", timers->len));
    }
  else
    g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
shell_call (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *method,
            GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  if (!strcmp (method, "RunningApps"))
    {
      GVariantBuilder apps;
      JsonArray *list = member_array (fixture, "open_apps");

      g_variant_builder_init (&apps, G_VARIANT_TYPE ("a(ss)"));
      for (guint i = 0; list && i < json_array_get_length (list); i++)
        {
          JsonObject *app = json_array_get_object_element (list, i);
          g_variant_builder_add (&apps, "(ss)", member_string (app, "id", ""), member_string (app, "name", ""));
        }
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(ss))", &apps));
      return;
    }
  launched++;
  g_message ("mock: an app launched");
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
serve (GDBusConnection *bus, const char *xml, const char *path, const char *const *interfaces,
       const GDBusInterfaceVTable *vtable)
{
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (xml, NULL);

  g_assert_nonnull (node);
  for (guint i = 0; interfaces[i]; i++)
    {
      GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface (node, interfaces[i]);
      g_autoptr (GError) error = NULL;

      g_assert_nonnull (info);
      if (!g_dbus_connection_register_object (bus, path, info, vtable, NULL, NULL, &error))
        g_error ("mock: %s at %s: %s", interfaces[i], path, error->message);
    }
}

static void
own (GDBusConnection *bus, const char *name)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                            "org.freedesktop.DBus", "RequestName",
                                                            g_variant_new ("(su)", name, 4 /* DO_NOT_QUEUE */),
                                                            G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                            NULL);
  guint32 result = 0;

  if (reply)
    g_variant_get (reply, "(u)", &result);
  /* 1: the primary owner. Anything else means this is not a private bus: stop before touching the real ones */
  if (result != 1)
    g_error ("mock: %s is someone else's on this bus: not a private bus", name);
}

/* the days an alarm repeats on, from the fixtures' names */
static guint32
days_of (JsonArray *days)
{
  static const char *const names[] = { "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday" };
  guint32 bits = 0;

  for (guint i = 0; days && i < json_array_get_length (days); i++)
    for (guint j = 0; j < 7; j++)
      if (!g_ascii_strcasecmp (json_array_get_string_element (days, i), names[j]))
        bits |= 1u << j;
  return bits;
}

void
mock_services_start (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_autoptr (GDBusConnection) system = session ? g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error) : NULL;
  static const GDBusInterfaceVTable nm = { NULL, nm_get, nm_set }, bluez_om = { bluez_call },
                                    bluez_adapter = { NULL, bluez_get, bluez_set }, upower = { NULL, upower_get },
                                    clock = { clock_call }, shell = { shell_call };
  JsonArray *list = member_array (fixture, "alarms");
  GVariantBuilder all;

  if (!system)
    g_error ("mock: no bus: %s", error->message);
  wifi_on = member_bool (member_object (fixture, "wifi"), "on", TRUE);
  bt_on = member_bool (member_object (fixture, "bluetooth"), "on", TRUE);
  g_variant_builder_init (&all, G_VARIANT_TYPE ("aa{sv}"));
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *a = json_array_get_object_element (list, i);
      int hour = 0, minute = 0;

      sscanf (member_string (a, "time", "7:00"), "%d:%d", &hour, &minute);
      g_variant_builder_add_value (&all, g_variant_new_parsed (
        "{'id': <%u>, 'hour': <%i>, 'minute': <%i>, 'days': <%u>, 'enabled': <%b>, 'label': <%s>}", i + 1, hour,
        minute, days_of (member_array (a, "days")), member_bool (a, "on", TRUE), member_string (a, "label", "")));
    }
  alarms = g_variant_ref_sink (g_variant_builder_end (&all));
  timers = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  serve (system, NM_XML, "/org/freedesktop/NetworkManager",
         (const char *const[]) { "org.freedesktop.NetworkManager", NULL }, &nm);
  serve (system, NM_XML, "/org/freedesktop/NetworkManager/ActiveConnection/1",
         (const char *const[]) { "org.freedesktop.NetworkManager.Connection.Active", NULL }, &nm);
  serve (system, BLUEZ_XML, "/", (const char *const[]) { "org.freedesktop.DBus.ObjectManager", NULL }, &bluez_om);
  serve (system, BLUEZ_XML, "/org/bluez/hci0", (const char *const[]) { "org.bluez.Adapter1", NULL }, &bluez_adapter);
  serve (system, UPOWER_XML, "/org/freedesktop/UPower/devices/DisplayDevice",
         (const char *const[]) { "org.freedesktop.UPower.Device", NULL }, &upower);
  serve (session, CLOCK_XML, "/vibeos/Clock/Alarms", (const char *const[]) { "vibeos.Clock.Alarms", NULL }, &clock);
  serve (session, CLOCK_XML, "/vibeos/Clock/Timers", (const char *const[]) { "vibeos.Clock.Timers", NULL }, &clock);
  serve (session, SHELL_XML, "/vibeos/Shell", (const char *const[]) { "vibeos.Shell", NULL }, &shell);
  /* the system bus's names are taken on a connection of their own: on a private bus that is the session's */
  own (system, "org.freedesktop.NetworkManager");
  own (system, "org.bluez");
  own (system, "org.freedesktop.UPower");
  own (session, "vibeos.Clock.Alarms");
  own (session, "vibeos.Shell");
  /* both connections stay open for the rest of the run */
  g_object_ref (session);
  g_object_ref (system);
}

void
mock_services_reset (void)
{
  /* each case starts from the fixtures' state */
  wifi_on = member_bool (member_object (fixture, "wifi"), "on", TRUE);
  bt_on = member_bool (member_object (fixture, "bluetooth"), "on", TRUE);
}

/* ---------------- the stores ---------------- */

/* the Dialer's calls table (apps/dialer/src/call-log.c) */
static const char CALLS_SCHEMA[] =
  "CREATE TABLE IF NOT EXISTS calls (_id INTEGER PRIMARY KEY AUTOINCREMENT, number TEXT NOT NULL DEFAULT '',"
  " date INTEGER NOT NULL, duration INTEGER NOT NULL DEFAULT 0, type INTEGER NOT NULL,"
  " features INTEGER NOT NULL DEFAULT 0, country_iso TEXT, geocoded_location TEXT,"
  " presentation INTEGER NOT NULL DEFAULT 1, phone_account_id TEXT, is_read INTEGER NOT NULL DEFAULT 0,"
  " new INTEGER NOT NULL DEFAULT 1, post_dial_digits TEXT NOT NULL DEFAULT '', via_number TEXT NOT NULL DEFAULT '');"
  "CREATE INDEX IF NOT EXISTS calls_date ON calls (date DESC);";

/* Messaging's bugle_db, the tables and columns the tools read (apps/messaging/src/sms-store.c) */
static const char SMS_SCHEMA[] =
  "CREATE TABLE IF NOT EXISTS conversations (_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT,"
  " latest_message_id INT, snippet_text TEXT, draft_snippet_text TEXT, archive_status INT DEFAULT 0,"
  " sort_timestamp INT DEFAULT 0, last_read_timestamp INT DEFAULT 0, participant_normalized_destination TEXT,"
  " participant_count INT DEFAULT 0);"
  "CREATE TABLE IF NOT EXISTS participants (_id INTEGER PRIMARY KEY AUTOINCREMENT, sub_id INT DEFAULT -2,"
  " normalized_destination TEXT, send_destination TEXT, display_destination TEXT);"
  "CREATE TABLE IF NOT EXISTS messages (_id INTEGER PRIMARY KEY AUTOINCREMENT, conversation_id INT,"
  " sender_id INT, sent_timestamp INT DEFAULT 0, received_timestamp INT DEFAULT 0, message_protocol INT DEFAULT 0,"
  " message_status INT DEFAULT 0, seen INT DEFAULT 0, read INT DEFAULT 0, sms_message_uri TEXT,"
  " raw_status INT DEFAULT 0, retry_start_timestamp INT DEFAULT 0);"
  "CREATE TABLE IF NOT EXISTS parts (_id INTEGER PRIMARY KEY AUTOINCREMENT, message_id INT, text TEXT, uri TEXT,"
  " content_type TEXT, timestamp INT, conversation_id INT NOT NULL);";

static sqlite3 *
new_store (const char *app, const char *file, const char *schema)
{
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), app, NULL);
  g_autofree char *path = g_build_filename (dir, file, NULL);
  sqlite3 *db;

  g_mkdir_with_parents (dir, 0700);
  /* only ever in the private environment's data directory (checked by agent-eval before this) */
  g_unlink (path);
  if (sqlite3_open (path, &db) != SQLITE_OK || sqlite3_exec (db, schema, NULL, NULL, NULL) != SQLITE_OK)
    g_error ("fixtures: %s: %s", path, sqlite3_errmsg (db));
  return db;
}

static void
exec (sqlite3 *db, const char *sql)
{
  char *error = NULL;
  if (sqlite3_exec (db, sql, NULL, NULL, &error) != SQLITE_OK)
    g_error ("fixtures: %s: %s", sql, error);
}

static void
write_calls (JsonArray *calls, gint64 now_ms)
{
  sqlite3 *db = new_store ("vibeos-dialer", "calls.db", CALLS_SCHEMA);

  for (guint i = 0; calls && i < json_array_get_length (calls); i++)
    {
      JsonObject *c = json_array_get_object_element (calls, i);
      char *sql = sqlite3_mprintf ("INSERT INTO calls (number, date, duration, type, presentation) VALUES"
                                   " (%Q, %lld, %lld, %lld, %lld)", member_string (c, "number", ""),
                                   (long long) (now_ms - member_int (c, "minutes_ago", 0) * 60000),
                                   (long long) member_int (c, "seconds", 0), (long long) member_int (c, "type", 1),
                                   (long long) member_int (c, "presentation", 1));
      exec (db, sql);
      sqlite3_free (sql);
    }
  sqlite3_close (db);
}

static void
write_messages (JsonArray *conversations, gint64 now_ms)
{
  sqlite3 *db = new_store ("vibeos-messaging", "bugle_db", SMS_SCHEMA);

  for (guint i = 0; conversations && i < json_array_get_length (conversations); i++)
    {
      JsonObject *c = json_array_get_object_element (conversations, i);
      const char *with = member_string (c, "with", "");
      JsonArray *messages = member_array (c, "messages");
      gint64 conversation, participant, last = 0, last_time = 0;
      const char *last_text = "";
      char *sql;

      sql = sqlite3_mprintf ("INSERT INTO participants (normalized_destination, send_destination,"
                             " display_destination) VALUES (%Q, %Q, %Q)", with, with, with);
      exec (db, sql);
      sqlite3_free (sql);
      participant = sqlite3_last_insert_rowid (db);
      sql = sqlite3_mprintf ("INSERT INTO conversations (name, participant_normalized_destination,"
                             " participant_count) VALUES (%Q, %Q, 1)", with, with);
      exec (db, sql);
      sqlite3_free (sql);
      conversation = sqlite3_last_insert_rowid (db);
      for (guint j = 0; messages && j < json_array_get_length (messages); j++)
        {
          JsonObject *m = json_array_get_object_element (messages, j);
          gboolean incoming = member_bool (m, "from_them", TRUE);
          gint64 at = now_ms - member_int (m, "minutes_ago", 0) * 60000;
          gboolean read = !incoming || member_bool (m, "read", TRUE);

          {
            g_autofree char *sender = incoming ? g_strdup_printf ("%" G_GINT64_FORMAT, participant) : g_strdup ("NULL");
            sql = sqlite3_mprintf ("INSERT INTO messages (conversation_id, sender_id, sent_timestamp,"
                                   " received_timestamp, message_status, seen, read) VALUES"
                                   " (%lld, %s, %lld, %lld, %d, %d, %d)", (long long) conversation, sender,
                                   (long long) at, (long long) at, incoming ? 100 : 2, read, read);
          }
          exec (db, sql);
          sqlite3_free (sql);
          last = sqlite3_last_insert_rowid (db);
          sql = sqlite3_mprintf ("INSERT INTO parts (message_id, text, content_type, timestamp, conversation_id)"
                                 " VALUES (%lld, %Q, 'text/plain', %lld, %lld)", (long long) last,
                                 member_string (m, "text", ""), (long long) at, (long long) conversation);
          exec (db, sql);
          sqlite3_free (sql);
          if (at >= last_time)
            last_time = at, last_text = member_string (m, "text", "");
        }
      sql = sqlite3_mprintf ("UPDATE conversations SET latest_message_id = (SELECT _id FROM messages WHERE"
                             " conversation_id = %lld ORDER BY received_timestamp DESC LIMIT 1),"
                             " sort_timestamp = %lld, snippet_text = %Q WHERE _id = %lld", (long long) conversation,
                             (long long) last_time, last_text, (long long) conversation);
      exec (db, sql);
      sqlite3_free (sql);
    }
  sqlite3_close (db);
}

/* whether a process keeps its files in a private home: HOME, and XDG_DATA_HOME and XDG_CONFIG_HOME if it has them,
 * under /tmp */
static gboolean
private_process (guint32 pid)
{
  g_autofree char *path = g_strdup_printf ("/proc/%u/environ", pid);
  g_autofree char *environ = NULL;
  gsize length;
  gboolean home = FALSE;

  if (!g_file_get_contents (path, &environ, &length, NULL))
    return FALSE;
  for (const char *v = environ; v < environ + length; v += strlen (v) + 1)
    {
      if (g_str_has_prefix (v, "HOME="))
        home = g_str_has_prefix (v + 5, "/tmp/");
      else if ((g_str_has_prefix (v, "XDG_DATA_HOME=") || g_str_has_prefix (v, "XDG_CONFIG_HOME="))
               && !strstr (v, "=/tmp/"))
        return FALSE;
    }
  return home;
}

/* every part of EDS on the bus keeps its stores in a private home, or nothing is written: an EDS a bus started with
 * the user's own HOME (11-23: a test display's bus) has the phone's own address book and calendar, which the
 * fixtures would empty */
static void
eds_private (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_autoptr (GVariant) names = NULL;
  g_autoptr (GVariantIter) iter = NULL;
  const char *name;

  if (bus)
    names = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                         "ListNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                         &error);
  if (!names)
    g_error ("fixtures: the bus's names: %s", error->message);
  g_variant_get (names, "(as)", &iter);
  while (g_variant_iter_next (iter, "&s", &name))
    {
      g_autoptr (GVariant) pid = NULL;
      guint32 p;

      if (!g_str_has_prefix (name, "org.gnome.evolution.dataserver."))
        continue;
      pid = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                         "GetConnectionUnixProcessID", g_variant_new ("(s)", name),
                                         G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
      if (!pid)
        g_error ("fixtures: no process for %s", name);
      g_variant_get (pid, "(u)", &p);
      if (!private_process (p))
        g_error ("fixtures: %s (process %u) keeps its stores outside /tmp: nothing written", name, p);
    }
}

static EClient *
builtin (ESourceRegistry *registry, gboolean calendar)
{
  g_autoptr (ESource) source = calendar ? e_source_registry_ref_builtin_calendar (registry)
                                        : e_source_registry_ref_builtin_address_book (registry);
  g_autoptr (GError) error = NULL;
  /* (guint32) -1: no wait for "connected", which a local book never says (0 would wait for ever) */
  EClient *client = calendar ? e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, (guint32) -1, NULL,
                                                          &error)
                             : e_book_client_connect_sync (source, (guint32) -1, NULL, &error);

  if (!client)
    g_error ("fixtures: EDS: %s", error->message);
  /* the registry and this store's factory are up now: neither may be the phone's own */
  eds_private ();
  return client;
}

static void
write_contacts (ESourceRegistry *registry, JsonArray *contacts)
{
  g_autoptr (EClient) client = builtin (registry, FALSE);
  g_autoptr (GError) error = NULL;
  GSList *uids = NULL, *added = NULL;

  /* the private address book, emptied of the last run's */
  if (e_book_client_get_contacts_uids_sync (E_BOOK_CLIENT (client), "", &uids, NULL, NULL) && uids)
    e_book_client_remove_contacts_sync (E_BOOK_CLIENT (client), uids, E_BOOK_OPERATION_FLAG_NONE, NULL, NULL);
  g_slist_free_full (uids, g_free);
  for (guint i = 0; contacts && i < json_array_get_length (contacts); i++)
    {
      JsonObject *c = json_array_get_object_element (contacts, i);
      g_autoptr (EContact) contact = e_contact_new ();
      JsonArray *tels = member_array (c, "tels"), *emails = member_array (c, "emails");

      e_contact_set (contact, E_CONTACT_FULL_NAME, member_string (c, "name", ""));
      if (json_object_has_member (c, "nickname"))
        e_contact_set (contact, E_CONTACT_NICKNAME, member_string (c, "nickname", ""));
      for (guint j = 0; tels && j < json_array_get_length (tels); j++)
        {
          JsonObject *t = json_array_get_object_element (tels, j);
          EVCardAttribute *attr = e_vcard_attribute_new (NULL, EVC_TEL);
          const char *type = member_string (t, "type", "mobile");

          e_vcard_attribute_add_param_with_value (attr, e_vcard_attribute_param_new (EVC_TYPE),
                                                  !strcmp (type, "mobile") ? "CELL" : !strcmp (type, "home")
                                                  ? "HOME" : !strcmp (type, "work") ? "WORK" : "VOICE");
          e_vcard_append_attribute_with_value (E_VCARD (contact), attr, member_string (t, "number", ""));
        }
      for (guint j = 0; emails && j < json_array_get_length (emails); j++)
        {
          EVCardAttribute *attr = e_vcard_attribute_new (NULL, EVC_EMAIL);
          e_vcard_append_attribute_with_value (E_VCARD (contact), attr, json_array_get_string_element (emails, j));
        }
      added = g_slist_append (added, g_object_ref (contact));
    }
  if (added && !e_book_client_add_contacts_sync (E_BOOK_CLIENT (client), added, E_BOOK_OPERATION_FLAG_NONE, NULL,
                                                 NULL, &error))
    g_error ("fixtures: contacts: %s", error->message);
  g_slist_free_full (added, g_object_unref);
}

static void
write_events (ESourceRegistry *registry, JsonArray *events)
{
  g_autoptr (EClient) client = builtin (registry, TRUE);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  GSList *objects = NULL;

  if (e_cal_client_get_object_list_as_comps_sync (E_CAL_CLIENT (client), "#t", &objects, NULL, NULL))
    for (GSList *l = objects; l; l = l->next)
      e_cal_client_remove_object_sync (E_CAL_CLIENT (client), e_cal_component_get_uid (l->data), NULL,
                                       E_CAL_OBJ_MOD_ALL, E_CAL_OPERATION_FLAG_NONE, NULL, NULL);
  g_slist_free_full (objects, g_object_unref);
  for (guint i = 0; events && i < json_array_get_length (events); i++)
    {
      JsonObject *e = json_array_get_object_element (events, i);
      g_autoptr (GDateTime) day = g_date_time_add_days (now, member_int (e, "day", 0));
      ICalComponent *comp = i_cal_component_new_vevent ();
      g_autoptr (GError) error = NULL;
      gboolean all_day = member_bool (e, "all_day", FALSE);
      ICalTime *start, *end;

      i_cal_component_set_summary (comp, member_string (e, "title", ""));
      if (json_object_has_member (e, "where"))
        i_cal_component_set_location (comp, member_string (e, "where", ""));
      if (all_day)
        {
          start = i_cal_time_new_null_time ();
          i_cal_time_set_date (start, g_date_time_get_year (day), g_date_time_get_month (day),
                               g_date_time_get_day_of_month (day));
          i_cal_time_set_is_date (start, TRUE);
          end = i_cal_time_clone (start);
          i_cal_time_adjust (end, 1, 0, 0, 0);
        }
      else
        {
          int hour = 9, minute = 0;
          g_autoptr (GDateTime) at = NULL;

          sscanf (member_string (e, "start", "9:00"), "%d:%d", &hour, &minute);
          at = g_date_time_new_local (g_date_time_get_year (day), g_date_time_get_month (day),
                                      g_date_time_get_day_of_month (day), hour, minute, 0);
          start = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (at), FALSE,
                                                       i_cal_timezone_get_utc_timezone ());
          end = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (at) + member_int (e, "minutes", 60) * 60,
                                                     FALSE, i_cal_timezone_get_utc_timezone ());
        }
      i_cal_component_set_dtstart (comp, start);
      i_cal_component_set_dtend (comp, end);
      g_object_unref (start);
      g_object_unref (end);
      if (!e_cal_client_create_object_sync (E_CAL_CLIENT (client), comp, E_CAL_OPERATION_FLAG_NONE, NULL, NULL,
                                            &error))
        g_error ("fixtures: an event: %s", error->message);
      g_object_unref (comp);
    }
}

void
mock_fixtures_write (JsonObject *phone)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (ESourceRegistry) registry = NULL;
  gint64 now_ms = g_get_real_time () / 1000;

  fixture = json_object_ref (phone);
  write_calls (member_array (phone, "calls"), now_ms);
  write_messages (member_array (phone, "messages"), now_ms);
  registry = e_source_registry_new_sync (NULL, &error);
  if (!registry)
    g_error ("fixtures: EDS's registry: %s", error->message);
  write_contacts (registry, member_array (phone, "contacts"));
  write_events (registry, member_array (phone, "events"));
}

guint
mock_launched (void)
{
  return launched;
}
