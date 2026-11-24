/* agent-eval-mock: a private copy of a phone for agent-eval.c, made from tests/agent-eval/fixtures.json, so that
 * Agent's tools read made-up data and never the phone's own.
 *
 * Run it in a private environment (run.sh eval does: a dbus-run-session of its own, HOME and the XDG directories
 * in a new directory, the system bus pointed at that same private bus):
 *  - the stores the tools read, written where the apps keep them: the Dialer's calls.db and Messaging's bugle_db in
 *    XDG_DATA_HOME, the contacts and the events in EDS's built-in address book and calendar (EDS started on the
 *    private bus by D-Bus activation, its files in the private XDG directories);
 *  - the services the tools ask, stood in for on the private bus: NetworkManager, BlueZ, UPower and ModemManager (as
 *    on the system bus), the Clock's alarms and timers and the shell's open apps and notifications (on the session
 *    bus). Turning Wi-Fi or Bluetooth off, setting or removing an alarm or a timer change only these;
 *  - the Weather app's last place and forecast, in the private cache directory, so that the weather is read from
 *    there and never fetched;
 *  - the Phone's call service on a mock modem, on buses of its own, for the in-call and incoming-call cases. */
#include "agent-eval-mock.h"

#include <glib/gstdio.h>
#include <libebook/libebook.h>
#include <time.h>
#include <libecal/libecal.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/prctl.h>

static JsonObject *fixture;     /* the fixtures' "phone" */

/* ---------------- the services ---------------- */

static gboolean wifi_on, bt_on;
static GPtrArray *alarms;       /* GVariant a{sv} */
static GPtrArray *timers;       /* GVariant a{sv} */
static guint32 next_id;         /* the next alarm's or timer's */
static guint launched;

static void write_weather (JsonObject *weather);

static const char NM_XML[] =
  "<node>"
  " <interface name='org.freedesktop.NetworkManager'>"
  "  <property name='WirelessEnabled' type='b' access='readwrite'/>"
  "  <property name='WirelessHardwareEnabled' type='b' access='read'/>"
  "  <property name='NetworkingEnabled' type='b' access='read'/>"
  "  <property name='ActiveConnections' type='ao' access='read'/>"
  "  <property name='Connectivity' type='u' access='read'/>"
  "  <property name='State' type='u' access='read'/>"
  "  <property name='WwanEnabled' type='b' access='read'/>"
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

static const char MM_XML[] =
  "<node>"
  " <interface name='org.freedesktop.DBus.ObjectManager'>"
  "  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
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
  "  <method name='DeleteAlarm'><arg type='u' direction='in'/></method>"
  " </interface>"
  " <interface name='vibeos.Clock.Timers'>"
  "  <method name='GetTimers'><arg type='aa{sv}' direction='out'/></method>"
  "  <method name='AddTimer'><arg type='x' direction='in'/><arg type='s' direction='in'/>"
  "   <arg type='b' direction='in'/><arg type='u' direction='out'/></method>"
  "  <method name='Start'><arg type='u' direction='in'/></method>"
  "  <method name='Delete'><arg type='u' direction='in'/></method>"
  " </interface>"
  "</node>";

static const char SHELL_XML[] =
  "<node>"
  " <interface name='vibeos.Shell'>"
  "  <method name='RunningApps'><arg type='a(ss)' direction='out'/></method>"
  "  <method name='Notifications'><arg type='a(ssssx)' direction='out'/></method>"
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
  if (!strcmp (property, "WwanEnabled"))
    return g_variant_new_boolean (member_bool (member_object (fixture, "mobile"), "data_on", TRUE));
  return g_variant_new_uint32 (70);
}

/* the modem, as ModemManager has it: with what names the phone and its SIM (the number, the IMEI) among its
 * properties, as a real modem's, so that the test set sees that the tool never reads them */
static void
mm_call (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *method,
         GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  JsonObject *m = member_object (fixture, "mobile");
  GVariantBuilder objects, modem, gpp, interfaces;
  const char *numbers[] = { member_string (m, "own_number", ""), NULL };

  g_variant_builder_init (&objects, G_VARIANT_TYPE ("a{oa{sa{sv}}}"));
  if (m)
    {
      g_variant_builder_init (&modem, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&modem, "{sv}", "State", g_variant_new_int32 (member_int (m, "state", 8)));
      g_variant_builder_add (&modem, "{sv}", "SignalQuality",
                             g_variant_new ("(ub)", (guint32) member_int (m, "signal", 60), TRUE));
      g_variant_builder_add (&modem, "{sv}", "AccessTechnologies",
                             g_variant_new_uint32 (member_int (m, "technology", 1 << 14)));
      g_variant_builder_add (&modem, "{sv}", "Sim",
                             g_variant_new_object_path (member_bool (m, "sim", TRUE)
                                                        ? "/org/freedesktop/ModemManager1/SIM/0" : "/"));
      g_variant_builder_add (&modem, "{sv}", "OwnNumbers", g_variant_new_strv (numbers, -1));
      g_variant_builder_add (&modem, "{sv}", "EquipmentIdentifier",
                             g_variant_new_string (member_string (m, "imei", "")));
      g_variant_builder_init (&gpp, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&gpp, "{sv}", "OperatorName", g_variant_new_string (member_string (m, "operator", "")));
      g_variant_builder_add (&gpp, "{sv}", "RegistrationState",
                             g_variant_new_uint32 (member_bool (m, "roaming", FALSE) ? 5 : 1));
      g_variant_builder_add (&gpp, "{sv}", "Imei", g_variant_new_string (member_string (m, "imei", "")));
      g_variant_builder_init (&interfaces, G_VARIANT_TYPE ("a{sa{sv}}"));
      g_variant_builder_add (&interfaces, "{s@a{sv}}", "org.freedesktop.ModemManager1.Modem",
                             g_variant_builder_end (&modem));
      g_variant_builder_add (&interfaces, "{s@a{sv}}", "org.freedesktop.ModemManager1.Modem.Modem3gpp",
                             g_variant_builder_end (&gpp));
      g_variant_builder_add (&objects, "{o@a{sa{sv}}}", "/org/freedesktop/ModemManager1/Modem/0",
                             g_variant_builder_end (&interfaces));
    }
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{oa{sa{sv}}})", &objects));
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

/* a list's entries, as an aa{sv} */
static GVariant *
listed (GPtrArray *list)
{
  GVariantBuilder all;

  g_variant_builder_init (&all, G_VARIANT_TYPE ("aa{sv}"));
  for (guint i = 0; i < list->len; i++)
    g_variant_builder_add_value (&all, list->pdata[i]);
  return g_variant_builder_end (&all);
}

/* the entry with that id gone from a list; FALSE when there is none */
static gboolean
remove_id (GPtrArray *list, guint32 id)
{
  for (guint i = 0; i < list->len; i++)
    {
      guint32 has = 0;
      g_variant_lookup (list->pdata[i], "id", "u", &has);
      if (has == id)
        {
          g_ptr_array_remove_index (list, i);
          return TRUE;
        }
    }
  return FALSE;
}

/* now on CLOCK_BOOTTIME, in ms: the Clock's timers count on it */
static gint64
boottime_ms (void)
{
  struct timespec now;

  clock_gettime (CLOCK_BOOTTIME, &now);
  return (gint64) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static GVariant *
timer_new (guint32 id, guint32 state, gint64 length, gint64 remaining, gint64 started, const char *label)
{
  return g_variant_ref_sink (g_variant_new_parsed (
    "{'id': <%u>, 'state': <%u>, 'length': <%x>, 'total-length': <%x>, 'remaining': <%x>, 'last-start': <%x>,"
    " 'label': <%s>}", id, state, length, length, remaining, started, label));
}

static void
clock_call (GDBusConnection *c, const char *sender, const char *path, const char *interface, const char *method,
            GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  if (!strcmp (method, "GetAlarms"))
    g_dbus_method_invocation_return_value (invocation, g_variant_new_tuple ((GVariant *[]) { listed (alarms) }, 1));
  else if (!strcmp (method, "AddAlarm"))
    {
      g_autoptr (GVariant) added = g_variant_get_child_value (parameters, 0);
      GVariantDict alarm;
      guint32 id = next_id++;

      g_variant_dict_init (&alarm, added);
      g_variant_dict_insert (&alarm, "id", "u", id);
      g_ptr_array_add (alarms, g_variant_ref_sink (g_variant_dict_end (&alarm)));
      g_message ("mock: an alarm added");
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", id));
    }
  else if (!strcmp (method, "DeleteAlarm") || !strcmp (method, "Delete"))
    {
      guint32 id;

      g_variant_get (parameters, "(u)", &id);
      if (!remove_id (strcmp (method, "Delete") ? alarms : timers, id))
        {
          g_dbus_method_invocation_return_dbus_error (invocation, "vibeos.Clock.Error.NotFound", "No such id");
          return;
        }
      g_message ("mock: %s removed", strcmp (method, "Delete") ? "an alarm" : "a timer");
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (!strcmp (method, "GetTimers"))
    g_dbus_method_invocation_return_value (invocation, g_variant_new_tuple ((GVariant *[]) { listed (timers) }, 1));
  else if (!strcmp (method, "AddTimer"))
    {
      gint64 length;
      const char *label;
      gboolean once;
      guint32 id = next_id++;

      g_variant_get (parameters, "(x&sb)", &length, &label, &once);
      /* 4: reset, until it is started */
      g_ptr_array_add (timers, timer_new (id, 4, length, length, 0, label));
      g_message ("mock: a timer added");
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", id));
    }
  else if (!strcmp (method, "Start"))
    {
      guint32 id;

      g_variant_get (parameters, "(u)", &id);
      for (guint i = 0; i < timers->len; i++)
        {
          guint32 has = 0;
          gint64 length = 0, remaining = 0;
          const char *label = "";

          g_variant_lookup (timers->pdata[i], "id", "u", &has);
          if (has != id)
            continue;
          g_variant_lookup (timers->pdata[i], "length", "x", &length);
          g_variant_lookup (timers->pdata[i], "remaining", "x", &remaining);
          g_variant_lookup (timers->pdata[i], "label", "&s", &label);
          {
            GVariant *started = timer_new (id, 1, length, remaining, boottime_ms (), label);
            g_variant_unref (timers->pdata[i]);
            timers->pdata[i] = started;
          }
        }
      g_dbus_method_invocation_return_value (invocation, NULL);
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
  if (!strcmp (method, "Notifications"))
    {
      GVariantBuilder all;
      JsonArray *list = member_array (fixture, "notifications");
      gint64 now = g_get_real_time () / G_USEC_PER_SEC;

      g_variant_builder_init (&all, G_VARIANT_TYPE ("a(ssssx)"));
      for (guint i = 0; list && i < json_array_get_length (list); i++)
        {
          JsonObject *n = json_array_get_object_element (list, i);
          g_variant_builder_add (&all, "(ssssx)", member_string (n, "app_id", ""), member_string (n, "app", ""),
                                 member_string (n, "title", ""), member_string (n, "body", ""),
                                 now - member_int (n, "minutes_ago", 0) * 60);
        }
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(ssssx))", &all));
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

/* VibeOS's web search (POST {"query", "language", "count"}), stood in for: the fixtures' "web" maps search words to
 * results ({"Pergamon museum": [{"title", "snippet", "site", "url"}]}); a query holding all of an entry's words gets
 * its results, any other nothing */
static void
on_search (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  SoupMessageBody *request = soup_server_message_get_request_body (msg);
  g_autoptr (GBytes) bytes = soup_message_body_flatten (request);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *said = NULL, *body = NULL;
  JsonObject *web = member_object (fixture, "web");
  JsonArray *found = NULL;

  if (strcmp (soup_server_message_get_method (msg), "POST")
      || !json_parser_load_from_data (parser, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), NULL)
      || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      soup_server_message_set_status (msg, 422, NULL);
      return;
    }
  said = g_utf8_strdown (member_string (json_node_get_object (json_parser_get_root (parser)), "query", ""), -1);
  if (web)
    {
      g_autoptr (GList) keys = json_object_get_members (web);

      for (GList *l = keys; l && !found; l = l->next)
        {
          g_autofree char *low = g_utf8_strdown (l->data, -1);
          g_auto (GStrv) words = g_strsplit (low, " ", -1);
          gboolean all = TRUE;

          for (guint i = 0; words[i] && all; i++)
            all = !*words[i] || strstr (said, words[i]) != NULL;
          if (all)
            found = member_array (web, l->data);
        }
    }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "data");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "results");
  if (found)
    json_builder_add_value (b, json_node_init_array (json_node_alloc (), found));
  else
    {
      json_builder_begin_array (b);
      json_builder_end_array (b);
    }
  json_builder_set_member_name (b, "answer");
  json_builder_add_null_value (b);
  json_builder_end_object (b);
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  body = json_to_string (root, FALSE);
  soup_server_message_set_status (msg, 200, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, body, strlen (body));
}

/* the search on the main loop, for the run: its address is the tools' (DICTATE_SEARCH_URL) */
static void
search_start (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  g_autoptr (GError) error = NULL;
  GSList *uris;
  g_autofree char *url = NULL;

  soup_server_add_handler (server, "/v1/search", on_search, NULL, NULL);
  if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
    g_error ("mock: no web search: %s", error->message);
  uris = soup_server_get_uris (server);
  url = g_strdup_printf ("http://127.0.0.1:%d/v1/search", g_uri_get_port (uris->data));
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_setenv ("DICTATE_SEARCH_URL", url, TRUE);
  /* it serves for the rest of the run */
}

void
mock_services_start (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_autoptr (GDBusConnection) system = session ? g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error) : NULL;
  static const GDBusInterfaceVTable nm = { NULL, nm_get, nm_set }, bluez_om = { bluez_call },
                                    bluez_adapter = { NULL, bluez_get, bluez_set }, upower = { NULL, upower_get },
                                    clock = { clock_call }, shell = { shell_call }, mm = { mm_call };

  if (!system)
    g_error ("mock: no bus: %s", error->message);
  alarms = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  timers = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  mock_services_reset ();

  serve (system, NM_XML, "/org/freedesktop/NetworkManager",
         (const char *const[]) { "org.freedesktop.NetworkManager", NULL }, &nm);
  serve (system, NM_XML, "/org/freedesktop/NetworkManager/ActiveConnection/1",
         (const char *const[]) { "org.freedesktop.NetworkManager.Connection.Active", NULL }, &nm);
  serve (system, BLUEZ_XML, "/", (const char *const[]) { "org.freedesktop.DBus.ObjectManager", NULL }, &bluez_om);
  serve (system, BLUEZ_XML, "/org/bluez/hci0", (const char *const[]) { "org.bluez.Adapter1", NULL }, &bluez_adapter);
  serve (system, UPOWER_XML, "/org/freedesktop/UPower/devices/DisplayDevice",
         (const char *const[]) { "org.freedesktop.UPower.Device", NULL }, &upower);
  serve (system, MM_XML, "/org/freedesktop/ModemManager1",
         (const char *const[]) { "org.freedesktop.DBus.ObjectManager", NULL }, &mm);
  serve (session, CLOCK_XML, "/vibeos/Clock/Alarms", (const char *const[]) { "vibeos.Clock.Alarms", NULL }, &clock);
  serve (session, CLOCK_XML, "/vibeos/Clock/Timers", (const char *const[]) { "vibeos.Clock.Timers", NULL }, &clock);
  serve (session, SHELL_XML, "/vibeos/Shell", (const char *const[]) { "vibeos.Shell", NULL }, &shell);
  /* the system bus's names are taken on a connection of their own: on a private bus that is the session's */
  own (system, "org.freedesktop.NetworkManager");
  own (system, "org.bluez");
  own (system, "org.freedesktop.UPower");
  own (system, "org.freedesktop.ModemManager1");
  own (session, "vibeos.Clock.Alarms");
  own (session, "vibeos.Shell");
  search_start ();
  /* both connections stay open for the rest of the run */
  g_object_ref (session);
  g_object_ref (system);
}

void
mock_services_reset (void)
{
  JsonArray *list = member_array (fixture, "alarms");

  /* each case starts from the fixtures' state: Wi-Fi, Bluetooth, the alarms and the timers, and a forecast the
   * Weather app has just fetched (a long run outlasts the half hour Agent takes one to be new) */
  write_weather (member_object (fixture, "weather"));
  wifi_on = member_bool (member_object (fixture, "wifi"), "on", TRUE);
  bt_on = member_bool (member_object (fixture, "bluetooth"), "on", TRUE);
  g_ptr_array_set_size (alarms, 0);
  g_ptr_array_set_size (timers, 0);
  next_id = 1;
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *a = json_array_get_object_element (list, i);
      int hour = 0, minute = 0;

      sscanf (member_string (a, "time", "7:00"), "%d:%d", &hour, &minute);
      g_ptr_array_add (alarms, g_variant_ref_sink (g_variant_new_parsed (
        "{'id': <%u>, 'hour': <%i>, 'minute': <%i>, 'days': <%u>, 'enabled': <%b>, 'label': <%s>}", next_id++, hour,
        minute, days_of (member_array (a, "days")), member_bool (a, "on", TRUE), member_string (a, "label", ""))));
    }
  list = member_array (fixture, "timers");
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *t = json_array_get_object_element (list, i);
      gint64 left = member_int (t, "minutes_left", 5) * 60000;
      gboolean running = member_bool (t, "running", TRUE);

      g_ptr_array_add (timers, timer_new (next_id++, running ? 1 : 2, member_int (t, "minutes", 10) * 60000, left,
                                          running ? boottime_ms () : 0, member_string (t, "label", "")));
    }
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
      if (json_object_has_member (c, "note"))
        e_contact_set (contact, E_CONTACT_NOTE, member_string (c, "note", ""));
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

/* the Weather app's cache (apps/weather): the place it shows, and its forecast, fetched "just now", from yesterday on
 * as Open-Meteo gives it with past_days=1 */
static void
write_weather (JsonObject *weather)
{
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "vibeos-launcher", NULL);
  g_autofree char *place = NULL, *forecast = NULL, *at = NULL;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  JsonObject *current = member_object (weather, "now");
  JsonArray *days = member_array (weather, "days");
  const char *daily[] = { "weather_code", "temperature_2m_max", "temperature_2m_min",
                          "precipitation_probability_max" };
  const char *keys[] = { "code", "high", "low", "rain" };

  if (!weather)
    return;
  g_mkdir_with_parents (dir, 0700);
  at = g_build_filename (dir, "weather.json", NULL);
  place = g_strdup_printf ("{\"lat\": %g, \"lon\": %g, \"place\": \"%s\"}",
                           json_object_get_double_member (weather, "lat"), json_object_get_double_member (weather, "lon"),
                           member_string (weather, "place", ""));
  g_file_set_contents (at, place, -1, NULL);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "latitude");
  json_builder_add_double_value (b, json_object_get_double_member (weather, "lat"));
  json_builder_set_member_name (b, "longitude");
  json_builder_add_double_value (b, json_object_get_double_member (weather, "lon"));
  json_builder_set_member_name (b, "current");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "temperature_2m");
  json_builder_add_double_value (b, member_int (current, "temperature", 10));
  json_builder_set_member_name (b, "apparent_temperature");
  json_builder_add_double_value (b, member_int (current, "feels_like", 8));
  json_builder_set_member_name (b, "weather_code");
  json_builder_add_int_value (b, member_int (current, "code", 3));
  json_builder_set_member_name (b, "wind_speed_10m");
  json_builder_add_double_value (b, member_int (current, "wind", 12));
  json_builder_end_object (b);
  json_builder_set_member_name (b, "daily");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "time");
  json_builder_begin_array (b);
  for (guint i = 0; days && i <= json_array_get_length (days); i++)
    {
      g_autoptr (GDateTime) day = g_date_time_add_days (now, (int) i - 1);
      g_autofree char *date = g_date_time_format (day, "%Y-%m-%d");
      json_builder_add_string_value (b, date);
    }
  json_builder_end_array (b);
  for (guint k = 0; k < G_N_ELEMENTS (daily); k++)
    {
      json_builder_set_member_name (b, daily[k]);
      json_builder_begin_array (b);
      /* yesterday: as today */
      for (guint i = 0; days && i <= json_array_get_length (days); i++)
        json_builder_add_int_value (b, member_int (json_array_get_object_element (days, i ? i - 1 : 0), keys[k], 0));
      json_builder_end_array (b);
    }
  json_builder_end_object (b);
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  forecast = json_to_string (root, FALSE);
  g_free (at), at = g_build_filename (dir, "forecast.json", NULL);
  g_file_set_contents (at, forecast, -1, NULL);
}

/* the places saved in Navi ("navi": {"home": {"name", "lat", "lng"}, "work": …}), in Navi's settings file */
static void
write_navi (JsonObject *navi)
{
  g_autofree char *dir = g_build_filename (g_get_user_config_dir (), "vibeos-navi", NULL);
  g_autofree char *path = g_build_filename (dir, "navi.ini", NULL);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  const char *groups[] = { "home", "work" };

  if (!navi)
    return;
  for (guint i = 0; i < G_N_ELEMENTS (groups); i++)
    {
      JsonObject *place = json_object_has_member (navi, groups[i]) ? json_object_get_object_member (navi, groups[i])
                                                                   : NULL;
      if (!place)
        continue;
      g_key_file_set_integer (kf, groups[i], "kind", 0);
      g_key_file_set_string (kf, groups[i], "name", json_object_get_string_member (place, "name"));
      g_key_file_set_double (kf, groups[i], "lat", json_object_get_double_member (place, "lat"));
      g_key_file_set_double (kf, groups[i], "lng", json_object_get_double_member (place, "lng"));
    }
  g_mkdir_with_parents (dir, 0700);
  if (!g_key_file_save_to_file (kf, path, NULL))
    g_error ("fixtures: Navi's settings could not be written");
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
  write_navi (json_object_has_member (phone, "navi") ? json_object_get_object_member (phone, "navi") : NULL);
}

guint
mock_launched (void)
{
  return launched;
}

/* ---------------- the calls (the Phone's call service on a mock modem) ---------------- */

/* The call service (vibeos-dialer-calls) and the Dialer's mock phone (apps/dialer/tests/mock-phone.py: a
 * ModemManager with a voice modem, callaudiod, a notification server) on a pair of buses of their own, as the
 * Dialer's tests run them: never the phone's modem, so never a real call, and no ringtone (no sound server there).
 * Agent's tools reach the service through DICTATE_DIALER_BUS. Everything here ends with agent-eval. */

static GDBusConnection *rig;    /* the rig's "session" bus: the service and the mock's controls */

/* a child that ends when agent-eval does, however it ends */
static void
die_with_parent (gpointer data)
{
  prctl (PR_SET_PDEATHSIG, SIGTERM);
}

/* `env`: names and values in turn, a NULL value to unset; what it writes is read (`piped`) or dropped */
static GSubprocess *
spawn (const char *const *argv, const char *const *env, gboolean piped, GError **error)
{
  g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (piped ? G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                                              : G_SUBPROCESS_FLAGS_STDOUT_SILENCE);

  g_subprocess_launcher_set_child_setup (launcher, die_with_parent, NULL, NULL);
  for (guint i = 0; env && env[i]; i += 2)
    {
      if (env[i + 1])
        g_subprocess_launcher_setenv (launcher, env[i], env[i + 1], TRUE);
      else
        g_subprocess_launcher_unsetenv (launcher, env[i]);
    }
  return g_subprocess_launcher_spawnv (launcher, argv, error);
}

/* the first line a child writes */
static char *
first_line (GSubprocess *child, GError **error)
{
  g_autoptr (GDataInputStream) in = g_data_input_stream_new (g_subprocess_get_stdout_pipe (child));

  return g_data_input_stream_read_line_utf8 (in, NULL, NULL, error);
}

static char *
bus_daemon (const char *config, GError **error)
{
  const char *argv[] = { "dbus-daemon", "--nofork", "--config-file", config, "--print-address=1", NULL };
  g_autoptr (GSubprocess) child = spawn (argv, NULL, TRUE, error);

  return child ? first_line (child, error) : NULL;
}

static GVariant *
rig_call (const char *name, const char *path, const char *interface, const char *method, GVariant *args,
          const char *reply, GError **error)
{
  return g_dbus_connection_call_sync (rig, name, path, interface, method, args, reply ? G_VARIANT_TYPE (reply) : NULL,
                                      G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, error);
}

static GVariant *
phone_control (const char *method, GVariant *args, const char *reply)
{
  g_autoptr (GError) error = NULL;
  GVariant *v = rig_call ("vibeos.Test.Phone", "/vibeos/Test/Phone", "vibeos.Test.Phone", method, args, reply,
                          &error);

  if (!v)
    g_warning ("calls: mock phone %s: %s", method, error->message);
  return v;
}

gboolean
mock_calls_start (const char *mock_phone, const char *service)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *dir = g_build_filename (g_get_home_dir (), "call-rig", NULL);
  g_autofree char *config = g_build_filename (dir, "bus.conf", NULL);
  g_autofree char *conf = g_strdup_printf ("<busconfig><type>session</type><listen>unix:dir=%s</listen>"
                                           "<auth>EXTERNAL</auth><policy context=\"default\"><allow "
                                           "send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/>"
                                           "<allow own=\"*\"/></policy></busconfig>", dir);
  g_autofree char *session = NULL, *system = NULL, *ready = NULL, *no_sound = NULL;
  g_autoptr (GSubprocess) mock = NULL, calls = NULL;
  g_autoptr (GVariant) owner = NULL;
  gint64 deadline;

  if (!g_file_test (mock_phone, G_FILE_TEST_IS_REGULAR) || !g_file_test (service, G_FILE_TEST_IS_EXECUTABLE))
    {
      g_message ("calls: no rig (%s, %s): the call cases are skipped", mock_phone, service);
      return FALSE;
    }
  g_mkdir_with_parents (dir, 0700);
  no_sound = g_strdup_printf ("unix:%s/no-sound", dir);
  if (!g_file_set_contents (config, conf, -1, &error) || !(session = bus_daemon (config, &error))
      || !(system = bus_daemon (config, &error)))
    {
      g_warning ("calls: the rig's buses: %s", error ? error->message : "?");
      return FALSE;
    }
  {
    const char *argv[] = { "python3", mock_phone, system, NULL };
    const char *env[] = { "DBUS_SESSION_BUS_ADDRESS", session, NULL };

    mock = spawn (argv, env, TRUE, &error);
    if (!mock || !(ready = first_line (mock, &error)) || strcmp (ready, "ready"))
      {
        g_warning ("calls: the mock phone: %s", error ? error->message : ready ? ready : "?");
        return FALSE;
      }
  }
  rig = g_dbus_connection_new_for_address_sync (session, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                         | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                NULL, NULL, &error);
  if (!rig)
    {
      g_warning ("calls: the rig's bus: %s", error->message);
      return FALSE;
    }
  /* a modem that holds calls: the service finds out once, as it starts */
  g_clear_pointer (&owner, g_variant_unref);
  owner = phone_control ("SetHold", g_variant_new ("(b)", TRUE), NULL);
  g_clear_pointer (&owner, g_variant_unref);
  {
    const char *argv[] = { service, NULL };
    g_autofree char *data = g_build_filename (dir, "data", NULL), *state = g_build_filename (dir, "state", NULL);
    g_autofree char *conf_dir = g_build_filename (dir, "config", NULL);
    /* the Dialer's settings from its installed schema, in memory; its own call log, not the fixtures'; no sound
     * server: it rings into nothing */
    const char *env[] = { "DBUS_SESSION_BUS_ADDRESS", session, "VIBEOS_DIALER_SYSTEM_BUS", system,
                          "GSETTINGS_BACKEND", "memory", "GSETTINGS_SCHEMA_DIR", NULL, "PULSE_SERVER", no_sound,
                          "XDG_DATA_HOME", data, "XDG_STATE_HOME", state, "XDG_CONFIG_HOME", conf_dir, NULL };
    calls = spawn (argv, env, FALSE, &error);
    if (!calls)
      {
        g_warning ("calls: the call service: %s", error->message);
        return FALSE;
      }
  }
  for (deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC; g_get_monotonic_time () < deadline;)
    {
      gboolean up = FALSE;

      g_clear_pointer (&owner, g_variant_unref);
      owner = rig_call ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                        g_variant_new ("(s)", "vibeos.Dialer.Calls"), "(b)", NULL);
      if (owner)
        g_variant_get (owner, "(b)", &up);
      if (up)
        break;
      g_usleep (100 * 1000);
    }
  g_setenv ("DICTATE_DIALER_BUS", session, TRUE);
  g_message ("calls: the call service is up on its mock phone");
  /* the children live as long as agent-eval: their GSubprocess objects may go */
  return TRUE;
}

/* the service's calls now, and its microphone and speaker */
static GVariant *
service_properties (void)
{
  g_autoptr (GVariant) reply = rig_call ("vibeos.Dialer.Calls", "/vibeos/Dialer/Calls",
                                         "org.freedesktop.DBus.Properties", "GetAll",
                                         g_variant_new ("(s)", "vibeos.Dialer.Calls"), "(a{sv})", NULL);

  return reply ? g_variant_get_child_value (reply, 0) : NULL;
}

/* the first of the service's calls in `state`, or 0 */
static guint32
call_in (guint32 state)
{
  g_autoptr (GVariant) props = service_properties ();
  g_autoptr (GVariant) calls = props ? g_variant_lookup_value (props, "Calls", G_VARIANT_TYPE ("aa{sv}")) : NULL;
  GVariantIter iter;
  GVariant *call;

  if (!calls)
    return 0;
  g_variant_iter_init (&iter, calls);
  while ((call = g_variant_iter_next_value (&iter)))
    {
      guint32 id = 0, s = 99;
      g_variant_lookup (call, "id", "u", &id);
      g_variant_lookup (call, "state", "u", &s);
      g_variant_unref (call);
      if (s == state)
        return id;
    }
  return 0;
}

static guint32
wait_call (guint32 state)
{
  guint32 id = 0;

  for (gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
       !(id = call_in (state)) && g_get_monotonic_time () < deadline;)
    g_usleep (100 * 1000);
  return id;
}

gboolean
mock_calls_set (JsonObject *call)
{
  g_autoptr (GVariant) report = NULL;
  g_autoptr (GVariant) done = NULL;
  g_autoptr (JsonNode) calls = NULL;
  const char *state = call ? member_string (call, "state", "ringing") : NULL;
  guint32 id;

  if (!rig)
    return FALSE;
  /* the last case's calls end, the microphone and the speaker as at the start */
  report = phone_control ("Report", NULL, "(s)");
  if (report)
    {
      const char *json;
      g_variant_get (report, "(&s)", &json);
      calls = json_from_string (json, NULL);
    }
  if (calls && JSON_NODE_HOLDS_OBJECT (calls)
      && json_object_has_member (json_node_get_object (calls), "calls"))
    {
      JsonObjectIter iter;
      const char *path;
      JsonNode *value;

      json_object_iter_init (&iter, json_object_get_object_member (json_node_get_object (calls), "calls"));
      while (json_object_iter_next (&iter, &path, &value))
        if (json_array_get_int_element (json_node_get_array (value), 0) != 7)
          {
            g_autoptr (GVariant) hung = phone_control ("RemoteHangup", g_variant_new ("(oi)", path, 4), NULL);
          }
    }
  for (gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC; g_get_monotonic_time () < deadline;)
    {
      if (!call_in (2) && !call_in (3) && !call_in (4) && !call_in (5))
        break;
      g_usleep (100 * 1000);
    }
  g_clear_pointer (&done, g_variant_unref);
  done = rig_call ("vibeos.Dialer.Calls", "/vibeos/Dialer/Calls", "vibeos.Dialer.Calls", "SetMuted",
                   g_variant_new ("(b)", FALSE), NULL, NULL);
  g_clear_pointer (&done, g_variant_unref);
  done = rig_call ("vibeos.Dialer.Calls", "/vibeos/Dialer/Calls", "vibeos.Dialer.Calls", "SetSpeaker",
                   g_variant_new ("(b)", FALSE), NULL, NULL);
  if (!call)
    return TRUE;
  /* the case's call: it rings, or it was answered (and maybe put on hold) */
  g_clear_pointer (&done, g_variant_unref);
  done = phone_control ("Incoming", g_variant_new ("(s)", member_string (call, "from", "")), "(o)");
  if (!done || !(id = wait_call (2)))
    return FALSE;
  if (!strcmp (state, "ringing"))
    return TRUE;
  g_clear_pointer (&done, g_variant_unref);
  done = rig_call ("vibeos.Dialer.Calls", "/vibeos/Dialer/Calls", "vibeos.Dialer.Calls", "Answer",
                   g_variant_new ("(u)", id), NULL, NULL);
  if (!done || !wait_call (4))
    return FALSE;
  if (!strcmp (state, "held"))
    {
      g_clear_pointer (&done, g_variant_unref);
      done = rig_call ("vibeos.Dialer.Calls", "/vibeos/Dialer/Calls", "vibeos.Dialer.Calls", "SetHeld",
                       g_variant_new ("(ub)", id, TRUE), NULL, NULL);
      return done && wait_call (5);
    }
  return TRUE;
}

char *
mock_calls_state (gboolean *muted, gboolean *speaker)
{
  g_autoptr (GVariant) props = service_properties ();
  static const char *const names[] = { "calling", "calling", "ringing", "ringing", "active", "held" };

  *muted = *speaker = FALSE;
  if (!props)
    return g_strdup ("no service");
  g_variant_lookup (props, "Muted", "b", muted);
  g_variant_lookup (props, "Speaker", "b", speaker);
  for (guint32 s = 0; s < G_N_ELEMENTS (names); s++)
    if (call_in (s))
      return g_strdup (names[s]);
  return g_strdup ("none");
}
