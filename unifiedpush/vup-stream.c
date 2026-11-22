/* vibeos-unifiedpush: the one stream from the push server, for every registration's topic.
 *
 * ntfy's JSON stream: GET <server>/<topic>,<topic>,.../json?up=1&since=<id>. It answers with one JSON object per
 * line: "open" once it has subscribed us (from then on it accepts messages for those topics: ntfy refuses a message
 * for a UnifiedPush topic that has had no subscriber), then the messages it holds after <id> (it keeps them for 72
 * hours), then the new ones as they come, and a "keepalive" about every 45 s when there are none. "up=1" makes ntfy
 * charge what is sent to these topics to us, the subscriber, and not to whoever sends it.
 *
 * When the stream ends or fails it is opened again after a pause that doubles each time (1 s up to 5 minutes; a
 * stream that stayed up a minute starts again at 1 s), from the last message seen, so nothing is lost and nothing
 * comes twice. It is opened again at once when the network comes back or changes (GNetworkMonitor), when logind
 * says the system has resumed (PrepareForSleep false), and when the watchdog finds that the monotonic clock fell
 * behind the boot clock (a suspend that logind did not run: a phone's own sleep daemon). A stream that has sent
 * nothing, not even a keepalive, for twice the keepalive interval and 15 s is taken for dead (a connection that died
 * without a word: the phone changed networks, a NAT forgot it), and so is one that has not connected in 30 s.
 *
 * Collect (vibeos.UnifiedPush1) asks the server, on a connection of its own, for everything it holds after the last
 * message seen (?poll=1), hands it to the apps and says when that is done: a phone that woke on a timer can then
 * sleep again at once. */
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <time.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "vup.h"

#define BACKOFF_FIRST        1           /* s */
#define BACKOFF_MAX          300
#define BACKOFF_LIMITED      60          /* at least this after a 429 Too Many Requests */
#define STABLE_SECONDS       60          /* a stream up this long starts again from BACKOFF_FIRST */
#define CONNECT_TIMEOUT      30
#define WATCHDOG_SECONDS     10
#define SLEPT_THRESHOLD      (5 * G_USEC_PER_SEC)
#define RESTART_DELAY_MS     300         /* the topics changed: more registrations may follow */
#define NETWORK_SETTLE_MS    1000
#define COLLECT_TIMEOUT      20          /* s, for the whole of Collect */
#define LINE_MAX_BYTES       (64 * 1024)

static GSettings *settings;
static SoupSession *session;
static GNetworkMonitor *monitor;
static GDBusConnection *system_bus;
static guint sleep_subscription;

static VupState state = VUP_STATE_IDLE;
static char *error_text;

/* the stream */
static SoupMessage *stream_msg;
static GCancellable *stream_cancel;
static GDataInputStream *stream_in;
static char *stream_topics;
static gint64 last_data;                 /* monotonic µs of the last line, or of the start of the connection */
static gint64 opened_at;                 /* monotonic µs of its "open", 0 before */
static char *last_id;                    /* the newest message seen in this run */
static guint backoff = BACKOFF_FIRST;
static guint retry_id, restart_id, network_id, watchdog_id;
static gint64 asleep_offset;             /* CLOCK_BOOTTIME - CLOCK_MONOTONIC at the last watchdog tick, µs */

static char *remote_address, *local_address;
static guint16 remote_port, local_port;

/* Collect */
typedef struct {
  VupCollected done;
  gpointer data;
} Collector;
static GSList *collectors;
static SoupMessage *poll_msg;
static GCancellable *poll_cancel;
static GDataInputStream *poll_in;
static guint collect_timeout_id;
static guint collect_generation;        /* which Collect a late callback belongs to */

static void stream_start (void);

static guint
keepalive_interval (void)
{
  return g_settings_get_uint (settings, "keepalive-interval");
}

const char *
vup_stream_state_name (VupState s)
{
  switch (s)
    {
    case VUP_STATE_IDLE: return "idle";
    case VUP_STATE_OFFLINE: return "offline";
    case VUP_STATE_CONNECTING: return "connecting";
    case VUP_STATE_CONNECTED: return "connected";
    case VUP_STATE_WAITING: return "waiting";
    }
  return "idle";
}

VupState
vup_stream_get_state (void)
{
  return state;
}

const char *
vup_stream_get_error (void)
{
  return error_text ? error_text : "";
}

void
vup_stream_get_addresses (const char **remote, guint16 *remote_port_out, const char **local, guint16 *local_port_out)
{
  *remote = remote_address ? remote_address : "";
  *remote_port_out = remote_port;
  *local = local_address ? local_address : "";
  *local_port_out = local_port;
}

static void
set_state (VupState new_state, const char *error)
{
  if (state == new_state && g_strcmp0 (error_text, error) == 0)
    return;
  state = new_state;
  g_free (error_text);
  error_text = g_strdup (error);
  vup_notify_state ();
}

static void
clear_addresses (void)
{
  if (!remote_address && !local_address)
    return;
  g_clear_pointer (&remote_address, g_free);
  g_clear_pointer (&local_address, g_free);
  remote_port = local_port = 0;
  vup_notify_state ();
}

static void
set_address (GSocketAddress *address, char **text, guint16 *port)
{
  g_free (*text);
  *text = NULL;
  *port = 0;
  if (G_IS_INET_SOCKET_ADDRESS (address))
    {
      *text = g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (address)));
      *port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (address));
    }
}

/* the stream's TCP connection, for a sleep hook's wake-on-WLAN pattern */
static void
on_network_event (SoupMessage *msg, GSocketClientEvent event, GIOStream *connection, gpointer data)
{
  g_autoptr(GSocketAddress) remote = NULL, local = NULL;

  if (msg != stream_msg || event != G_SOCKET_CLIENT_CONNECTED || !G_IS_SOCKET_CONNECTION (connection))
    return;
  remote = g_socket_connection_get_remote_address (G_SOCKET_CONNECTION (connection), NULL);
  local = g_socket_connection_get_local_address (G_SOCKET_CONNECTION (connection), NULL);
  set_address (remote, &remote_address, &remote_port);
  set_address (local, &local_address, &local_port);
  vup_notify_state ();
}

/* --- the lines both the stream and Collect read */

static const char *
member_string (JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member (object, name);

  return node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
    ? json_node_get_string (node) : NULL;
}

static void
on_open (void)
{
  g_auto(GStrv) topics = g_strsplit (stream_topics, ",", -1);
  gboolean changed = FALSE;

  opened_at = g_get_monotonic_time ();
  g_message ("connected to the push server: %u topics", g_strv_length (topics));
  set_state (VUP_STATE_CONNECTED, NULL);
  /* the server now takes messages for these topics: the apps that are still waiting get their endpoints */
  for (guint i = 0; topics[i]; i++)
    {
      VupRegistration *reg = vup_store_by_topic (topics[i]);

      if (reg && !reg->active)
        {
          reg->active = TRUE;
          changed = TRUE;
          if (reg->endpoints_owed == 0)
            vup_deliver_new_endpoint (reg, FALSE);
          for (; reg->endpoints_owed > 0; reg->endpoints_owed--)
            vup_deliver_new_endpoint (reg, TRUE);
        }
    }
  if (changed)
    {
      vup_store_save ();
      vup_notify_registrations ();
    }
}

/* a line of the stream or of a poll; FALSE when it is not one of ntfy's */
static gboolean
handle_line (const char *line, gboolean from_stream)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GBytes) message = NULL;
  JsonNode *root;
  JsonObject *object;
  const char *event, *id, *topic, *text;

  if (!*line)
    return TRUE;
  if (!json_parser_load_from_data (parser, line, -1, NULL) || !(root = json_parser_get_root (parser))
      || !JSON_NODE_HOLDS_OBJECT (root))
    return FALSE;
  object = json_node_get_object (root);
  event = member_string (object, "event");
  if (g_strcmp0 (event, "open") == 0)
    {
      if (from_stream)
        on_open ();
      return TRUE;
    }
  if (g_strcmp0 (event, "message") != 0)
    return TRUE;    /* "keepalive", "poll_request" */

  id = member_string (object, "id");
  topic = member_string (object, "topic");
  text = member_string (object, "message");
  if (!id || !*id || !topic)
    return FALSE;
  if (vup_deliver_seen (id))
    return TRUE;
  g_free (last_id);
  last_id = g_strdup (id);
  if (!text || !(message = vup_decode_message (text, member_string (object, "encoding"))))
    {
      g_warning ("message %s for %s: no body, or broken base64: dropped", id, vup_redact (topic));
      message = g_bytes_new (NULL, 0);
    }
  if (json_object_has_member (object, "attachment"))
    {
      /* a body too large for a message, kept by the server as a file: not a push message */
      g_warning ("message %s for %s is an attachment: dropped", id, vup_redact (topic));
      g_clear_pointer (&message, g_bytes_unref);
      message = g_bytes_new (NULL, 0);
    }
  vup_deliver_received (id, topic, message);
  return TRUE;
}

static const char *
since_parameter (void)
{
  return last_id ? last_id : vup_store_get_since () ? vup_store_get_since () : "all";
}

/* --- the stream */

static void
stream_stop (void)
{
  if (stream_cancel)
    g_cancellable_cancel (stream_cancel);
  g_clear_object (&stream_cancel);
  g_clear_object (&stream_in);
  g_clear_object (&stream_msg);
  g_clear_pointer (&stream_topics, g_free);
  opened_at = 0;
  clear_addresses ();
}

static gboolean
on_retry (gpointer data)
{
  retry_id = 0;
  stream_start ();
  return G_SOURCE_REMOVE;
}

/* the stream failed: it is opened again after a pause */
static void
stream_failed (const char *why, guint at_least)
{
  guint wait;

  if (opened_at && g_get_monotonic_time () - opened_at >= STABLE_SECONDS * G_USEC_PER_SEC)
    backoff = BACKOFF_FIRST;
  stream_stop ();
  /* a little jitter, so that phones that lost the server together do not all come back in the same second */
  wait = MAX (backoff, at_least);
  wait = wait + g_random_int_range (0, wait / 5 + 1);
  backoff = MIN (backoff * 2, BACKOFF_MAX);
  g_warning ("push server: %s; trying again in %u s", why, wait);
  set_state (VUP_STATE_WAITING, why);
  g_clear_handle_id (&retry_id, g_source_remove);
  retry_id = g_timeout_add_seconds (wait, on_retry, NULL);
}

static void read_stream_line (void);

static void
on_stream_line (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GCancellable) cancel = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *line = NULL;
  gsize length = 0;

  line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (source), result, &length, &error);
  if (g_cancellable_is_cancelled (cancel))
    return;
  if (!line)
    {
      stream_failed (error ? error->message : "the server closed the stream", 0);
      return;
    }
  last_data = g_get_monotonic_time ();
  if (length > LINE_MAX_BYTES || !handle_line (line, TRUE))
    {
      stream_failed ("the server sent something that is not ntfy's JSON stream", 0);
      return;
    }
  read_stream_line ();
}

static void
read_stream_line (void)
{
  g_data_input_stream_read_line_async (stream_in, G_PRIORITY_DEFAULT, stream_cancel, on_stream_line,
                                       g_object_ref (stream_cancel));
}

static void
on_stream_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GCancellable) cancel = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GInputStream) body = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  guint status;

  if (g_cancellable_is_cancelled (cancel))
    return;
  if (!body)
    {
      stream_failed (error->message, 0);
      return;
    }
  status = soup_message_get_status (stream_msg);
  if (status != SOUP_STATUS_OK)
    {
      g_autofree char *why = g_strdup_printf ("%u %s", status, soup_message_get_reason_phrase (stream_msg));
      stream_failed (why, status == 429 ? BACKOFF_LIMITED : 0);
      return;
    }
  if (!remote_address)
    {
      set_address (soup_message_get_remote_address (stream_msg), &remote_address, &remote_port);
      vup_notify_state ();
    }
  last_data = g_get_monotonic_time ();
  stream_in = g_data_input_stream_new (body);
  g_data_input_stream_set_newline_type (stream_in, G_DATA_STREAM_NEWLINE_TYPE_LF);
  read_stream_line ();
}

static char *
topics_url (const char *topics, const char *extra)
{
  return g_strdup_printf ("%s/%s/json?up=1&%s", vup_store_get_server (), topics, extra);
}

static void
stream_start (void)
{
  g_autofree char *topics = vup_store_topics (), *since = NULL, *url = NULL;

  stream_stop ();
  g_clear_handle_id (&retry_id, g_source_remove);
  g_clear_handle_id (&restart_id, g_source_remove);
  if (!topics)
    {
      set_state (VUP_STATE_IDLE, NULL);
      return;
    }
  if (!g_network_monitor_get_network_available (monitor))
    {
      set_state (VUP_STATE_OFFLINE, NULL);
      return;
    }

  since = g_strdup_printf ("since=%s", since_parameter ());
  url = topics_url (topics, since);
  stream_msg = soup_message_new ("GET", url);
  if (!stream_msg)
    {
      set_state (VUP_STATE_WAITING, "the push server's URL is not one");
      return;
    }
  /* a connection of its own: with HTTP/2 a poll would otherwise share it, and be as dead as it is */
  soup_message_add_flags (stream_msg, SOUP_MESSAGE_NEW_CONNECTION);
  g_signal_connect (stream_msg, "network-event", G_CALLBACK (on_network_event), NULL);
  stream_cancel = g_cancellable_new ();
  stream_topics = g_steal_pointer (&topics);
  last_data = g_get_monotonic_time ();
  g_message ("connecting to %s (%s)", vup_store_get_server (), since);
  set_state (VUP_STATE_CONNECTING, NULL);
  soup_session_send_async (session, stream_msg, G_PRIORITY_DEFAULT, stream_cancel, on_stream_sent,
                           g_object_ref (stream_cancel));
}

void
vup_stream_reconnect (const char *why)
{
  g_message ("reconnecting: %s", why);
  backoff = BACKOFF_FIRST;
  stream_start ();
}

static gboolean
on_restart (gpointer data)
{
  g_autofree char *topics = vup_store_topics ();

  restart_id = 0;
  if (g_strcmp0 (topics, stream_topics) != 0 || !stream_msg)
    stream_start ();
  return G_SOURCE_REMOVE;
}

/* a registration came or went: the stream is opened again for the new set of topics, from where it was */
void
vup_stream_topics_changed (void)
{
  g_clear_handle_id (&restart_id, g_source_remove);
  restart_id = g_timeout_add (RESTART_DELAY_MS, on_restart, NULL);
}

/* --- what tells it to reconnect */

static gboolean
on_watchdog (gpointer data)
{
  struct timespec boot;
  gint64 now = g_get_monotonic_time (), offset;

  /* the monotonic clock stops while the system sleeps, the boot clock does not: a jump in the difference is a sleep */
  clock_gettime (CLOCK_BOOTTIME, &boot);
  offset = (boot.tv_sec * G_USEC_PER_SEC + boot.tv_nsec / 1000) - now;
  if (asleep_offset && offset - asleep_offset > SLEPT_THRESHOLD && state != VUP_STATE_IDLE)
    {
      asleep_offset = offset;
      vup_stream_reconnect ("the system slept");
      return G_SOURCE_CONTINUE;
    }
  asleep_offset = offset;

  if (state == VUP_STATE_CONNECTING && now - last_data > CONNECT_TIMEOUT * G_USEC_PER_SEC)
    {
      g_autofree char *why = g_strdup_printf ("could not connect in %u s", CONNECT_TIMEOUT);
      stream_failed (why, 0);
    }
  else if (state == VUP_STATE_CONNECTED
           && now - last_data > (2 * keepalive_interval () + 15) * (gint64) G_USEC_PER_SEC)
    {
      g_autofree char *why = g_strdup_printf ("nothing from the server for %" G_GINT64_FORMAT " s, not even a keepalive",
                                              (now - last_data) / G_USEC_PER_SEC);
      stream_failed (why, 0);
    }
  return G_SOURCE_CONTINUE;
}

/* the stream's own address is still on an interface that is up: a change elsewhere (another interface, an IPv6
 * address coming or going, a route) leaves the stream as it is */
static gboolean
local_address_present (void)
{
  struct ifaddrs *list, *i;
  gboolean found = FALSE;

  if (!local_address || getifaddrs (&list) < 0)
    return FALSE;
  for (i = list; i && !found; i = i->ifa_next)
    {
      g_autoptr(GSocketAddress) address = NULL;
      g_autofree char *text = NULL;

      if (!i->ifa_addr || !(i->ifa_flags & IFF_UP)
          || (i->ifa_addr->sa_family != AF_INET && i->ifa_addr->sa_family != AF_INET6))
        continue;
      address = g_socket_address_new_from_native (i->ifa_addr, i->ifa_addr->sa_family == AF_INET
                                                                 ? sizeof (struct sockaddr_in)
                                                                 : sizeof (struct sockaddr_in6));
      if (!G_IS_INET_SOCKET_ADDRESS (address))
        continue;
      text = g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (address)));
      found = g_strcmp0 (text, local_address) == 0;
    }
  freeifaddrs (list);
  return found;
}

static gboolean
on_network_settled (gpointer data)
{
  network_id = 0;
  if (!g_network_monitor_get_network_available (monitor))
    {
      if (state != VUP_STATE_IDLE && state != VUP_STATE_OFFLINE)
        g_message ("the network is gone");
      stream_stop ();
      g_clear_handle_id (&retry_id, g_source_remove);
      if (vup_store_count (NULL) > 0)
        set_state (VUP_STATE_OFFLINE, NULL);
      return G_SOURCE_REMOVE;
    }
  if (state == VUP_STATE_CONNECTED && local_address_present ())
    {
      g_debug ("the network changed; the stream's address is still there, so the stream stays");
      return G_SOURCE_REMOVE;
    }
  if (vup_store_count (NULL) > 0)
    vup_stream_reconnect (state == VUP_STATE_OFFLINE ? "the network is back" : "the network changed");
  return G_SOURCE_REMOVE;
}

static void
on_network_changed (GNetworkMonitor *network_monitor, gboolean available, gpointer data)
{
  g_clear_handle_id (&network_id, g_source_remove);
  network_id = g_timeout_add (NETWORK_SETTLE_MS, on_network_settled, NULL);
}

static void
on_prepare_for_sleep (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                      const char *signal, GVariant *parameters, gpointer data)
{
  gboolean going_to_sleep = FALSE;

  g_variant_get (parameters, "(b)", &going_to_sleep);
  if (!going_to_sleep && vup_store_count (NULL) > 0)
    vup_stream_reconnect ("the system resumed");
}

static void
on_server_changed (GSettings *changed_settings, const char *key, gpointer data)
{
  g_autofree char *url = g_settings_get_string (settings, "server-url");
  g_autofree char *server = vup_normalize_server (url);

  if (!server)
    {
      g_warning ("server-url \"%s\" is not an http or https URL: staying with %s", url, vup_store_get_server ());
      return;
    }
  if (!vup_store_set_server (server))
    return;
  /* every endpoint changes: the apps get the new ones once the new server has subscribed us */
  g_clear_pointer (&last_id, g_free);
  vup_notify_state ();
  vup_notify_registrations ();
  vup_stream_reconnect ("the push server changed");
}

/* --- Collect */

static void
collect_finish (GError *error)
{
  GSList *list = g_steal_pointer (&collectors);

  g_clear_handle_id (&collect_timeout_id, g_source_remove);
  if (poll_cancel)
    g_cancellable_cancel (poll_cancel);
  g_clear_object (&poll_cancel);
  g_clear_object (&poll_in);
  g_clear_object (&poll_msg);
  for (GSList *l = list; l; l = l->next)
    {
      Collector *collector = l->data;
      collector->done (error, collector->data);
      g_free (collector);
    }
  g_slist_free (list);
}

static gboolean
on_collect_timeout (gpointer data)
{
  g_autoptr(GError) error = g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "not done in %u s", COLLECT_TIMEOUT);

  collect_timeout_id = 0;
  collect_finish (error);
  return G_SOURCE_REMOVE;
}

static gboolean
on_delivered (gpointer data)
{
  if (GPOINTER_TO_UINT (data) == collect_generation)
    collect_finish (NULL);
  return G_SOURCE_REMOVE;
}

static void read_poll_line (void);

static void
on_poll_line (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GCancellable) cancel = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *line = NULL;
  gsize length = 0;

  line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (source), result, &length, &error);
  if (g_cancellable_is_cancelled (cancel))
    return;
  if (error)
    {
      collect_finish (error);
      return;
    }
  if (!line)
    {
      /* all the server held is here: done once the apps have it */
      g_clear_object (&poll_in);
      g_clear_object (&poll_msg);
      vup_deliver_when_idle (on_delivered, GUINT_TO_POINTER (collect_generation));
      return;
    }
  if (length <= LINE_MAX_BYTES)
    handle_line (line, FALSE);
  read_poll_line ();
}

static void
read_poll_line (void)
{
  g_data_input_stream_read_line_async (poll_in, G_PRIORITY_DEFAULT, poll_cancel, on_poll_line,
                                       g_object_ref (poll_cancel));
}

static void
on_poll_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GCancellable) cancel = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GInputStream) body = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  guint status;

  if (g_cancellable_is_cancelled (cancel))
    return;
  if (!body)
    {
      collect_finish (error);
      return;
    }
  status = soup_message_get_status (poll_msg);
  if (status != SOUP_STATUS_OK)
    {
      g_autoptr(GError) failed = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "the push server answered %u %s",
                                              status, soup_message_get_reason_phrase (poll_msg));
      collect_finish (failed);
      return;
    }
  poll_in = g_data_input_stream_new (body);
  g_data_input_stream_set_newline_type (poll_in, G_DATA_STREAM_NEWLINE_TYPE_LF);
  read_poll_line ();
}

void
vup_stream_collect (VupCollected done, gpointer data)
{
  g_autofree char *topics = vup_store_topics (), *since = NULL, *url = NULL;
  Collector *collector = g_new0 (Collector, 1);

  collector->done = done;
  collector->data = data;
  collectors = g_slist_append (collectors, collector);
  if (collect_timeout_id)
    return;     /* one is under way: this one ends with it */

  collect_generation++;
  collect_timeout_id = g_timeout_add_seconds (COLLECT_TIMEOUT, on_collect_timeout, NULL);
  if (!topics)
    {
      vup_deliver_when_idle (on_delivered, GUINT_TO_POINTER (collect_generation));
      return;
    }
  if (!g_network_monitor_get_network_available (monitor))
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE, "no network");
      collect_finish (error);
      return;
    }
  /* a stream that has been quiet longer than a keepalive may have died while the system slept */
  if (state != VUP_STATE_CONNECTED
      || g_get_monotonic_time () - last_data > (keepalive_interval () + 10) * (gint64) G_USEC_PER_SEC)
    vup_stream_reconnect ("collecting");

  since = g_strdup_printf ("poll=1&since=%s", since_parameter ());
  url = topics_url (topics, since);
  poll_msg = soup_message_new ("GET", url);
  soup_message_add_flags (poll_msg, SOUP_MESSAGE_NEW_CONNECTION);
  poll_cancel = g_cancellable_new ();
  g_message ("collecting (%s)", since);
  soup_session_send_async (session, poll_msg, G_PRIORITY_DEFAULT, poll_cancel, on_poll_sent,
                           g_object_ref (poll_cancel));
}

/* --- setting up */

void
vup_stream_init (GSettings *the_settings)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *url = NULL, *server = NULL;

  settings = the_settings;
  session = soup_session_new_with_options ("user-agent", "vibeos-unifiedpush/" VERSION,
                                           "timeout", 0, "idle-timeout", 0, NULL);

  url = g_settings_get_string (settings, "server-url");
  if (!(server = vup_normalize_server (url)))
    {
      g_autoptr(GVariant) fallback = g_settings_get_default_value (settings, "server-url");
      g_warning ("server-url \"%s\" is not an http or https URL: using the default", url);
      server = vup_normalize_server (g_variant_get_string (fallback, NULL));
    }
  if (vup_store_set_server (server) && vup_store_count (NULL) > 0)
    g_message ("a new push server: every app gets a new endpoint");
  g_signal_connect (settings, "changed::server-url", G_CALLBACK (on_server_changed), NULL);

  monitor = g_network_monitor_get_default ();
  g_signal_connect (monitor, "network-changed", G_CALLBACK (on_network_changed), NULL);

  if (!(system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error)))
    g_warning ("no system bus, so no resume from logind: %s", error->message);
  else
    sleep_subscription = g_dbus_connection_signal_subscribe (system_bus, "org.freedesktop.login1",
                                                             "org.freedesktop.login1.Manager", "PrepareForSleep",
                                                             "/org/freedesktop/login1", NULL,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, NULL,
                                                             NULL);
  watchdog_id = g_timeout_add_seconds (WATCHDOG_SECONDS, on_watchdog, NULL);
}

/* once the bus name is ours */
void
vup_stream_start (void)
{
  stream_start ();
}

void
vup_stream_shutdown (void)
{
  if (collectors)
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "the distributor is stopping");
      collect_finish (error);
    }
  stream_stop ();
  g_clear_handle_id (&retry_id, g_source_remove);
  g_clear_handle_id (&restart_id, g_source_remove);
  g_clear_handle_id (&network_id, g_source_remove);
  g_clear_handle_id (&watchdog_id, g_source_remove);
  if (sleep_subscription)
    g_dbus_connection_signal_unsubscribe (system_bus, sleep_subscription);
  g_clear_object (&system_bus);
  g_clear_object (&session);
}
