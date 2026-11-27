/* What of Navi runs with a route and without one, its window seen, hidden or closed (navi-awake.h): the real
 * NaviPosition against a GeoClue of the test's own and NaviAwake's sleep inhibitor against a logind of its own, both
 * on a D-Bus of the test's own (as the system bus), the window's part (drawing, the compass, the screen) recorded. */
#include "navi-awake.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <unistd.h>

/* ---- GeoClue and logind, as far as Navi asks them ---- */

static const char GEOCLUE_XML[] =
  "<node>"
  " <interface name='org.freedesktop.GeoClue2.Manager'>"
  "  <method name='CreateClient'><arg type='o' direction='out'/></method>"
  "  <method name='GetClient'><arg type='o' direction='out'/></method>"
  "  <method name='DeleteClient'><arg type='o' direction='in'/></method>"
  " </interface>"
  " <interface name='org.freedesktop.GeoClue2.Client'>"
  "  <method name='Start'/><method name='Stop'/>"
  "  <property name='DesktopId' type='s' access='readwrite'/>"
  "  <property name='RequestedAccuracyLevel' type='u' access='readwrite'/>"
  "  <property name='DistanceThreshold' type='u' access='readwrite'/>"
  "  <property name='TimeThreshold' type='u' access='readwrite'/>"
  " </interface>"
  " <interface name='org.freedesktop.login1.Manager'>"
  "  <method name='Inhibit'>"
  "   <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "   <arg type='s' direction='in'/><arg type='h' direction='out'/>"
  "  </method>"
  " </interface>"
  "</node>";

typedef struct {
  char *path;
  guint registration;
  gboolean started;
} Client;

static struct {
  GDBusConnection *conn;
  GDBusNodeInfo *info;
  GPtrArray *clients; /* the live ones */
  guint made, starts, stops, deletes;
  guint inhibits;
  GArray *inhibitors; /* the read ends of the inhibitors handed out */
  char *inhibit_args;
} fake;

static Client *
client_at (const char *path)
{
  for (guint i = 0; i < fake.clients->len; i++)
    if (!g_strcmp0 (((Client *) fake.clients->pdata[i])->path, path))
      return fake.clients->pdata[i];
  return NULL;
}

static guint
clients_started (void)
{
  guint n = 0;
  for (guint i = 0; i < fake.clients->len; i++)
    n += ((Client *) fake.clients->pdata[i])->started;
  return n;
}

static void client_call (GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *,
                         GDBusMethodInvocation *, gpointer);

static gboolean
client_set (GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *name,
            GVariant *value, GError **error, gpointer data)
{
  return TRUE;
}

static const GDBusInterfaceVTable client_vtable = { client_call, NULL, client_set };

static void
client_call (GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *method,
             GVariant *params, GDBusMethodInvocation *call, gpointer data)
{
  Client *c = client_at (path);
  if (c && !g_strcmp0 (method, "Start")) {
    c->started = TRUE;
    fake.starts++;
  } else if (c && !g_strcmp0 (method, "Stop")) {
    c->started = FALSE;
    fake.stops++;
  }
  g_dbus_method_invocation_return_value (call, NULL);
}

static void
client_free (gpointer data)
{
  Client *c = data;
  g_dbus_connection_unregister_object (fake.conn, c->registration);
  g_free (c->path);
  g_free (c);
}

static void
manager_call (GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *method,
              GVariant *params, GDBusMethodInvocation *call, gpointer data)
{
  if (!g_strcmp0 (method, "CreateClient") || !g_strcmp0 (method, "GetClient")) {
    Client *c = g_new0 (Client, 1);
    c->path = g_strdup_printf ("/org/freedesktop/GeoClue2/Client/%u", ++fake.made);
    c->registration = g_dbus_connection_register_object (
      conn, c->path, g_dbus_node_info_lookup_interface (fake.info, "org.freedesktop.GeoClue2.Client"),
      &client_vtable, NULL, NULL, NULL);
    g_ptr_array_add (fake.clients, c);
    g_dbus_method_invocation_return_value (call, g_variant_new ("(o)", c->path));
  } else if (!g_strcmp0 (method, "DeleteClient")) {
    const char *gone;
    g_variant_get (params, "(&o)", &gone);
    Client *c = client_at (gone);
    if (c) {
      fake.deletes++;
      g_ptr_array_remove (fake.clients, c);
    }
    g_dbus_method_invocation_return_value (call, NULL);
  }
}

static void
login1_call (GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *method,
             GVariant *params, GDBusMethodInvocation *call, gpointer data)
{
  int ends[2];
  g_assert_no_errno (pipe (ends));
  fake.inhibits++;
  g_free (fake.inhibit_args);
  fake.inhibit_args = g_variant_print (params, FALSE);
  g_autoptr (GUnixFDList) fds = g_unix_fd_list_new_from_array (&ends[1], 1); /* takes the write end */
  g_array_append_val (fake.inhibitors, ends[0]);
  g_dbus_method_invocation_return_value_with_unix_fd_list (call, g_variant_new ("(h)", 0), fds);
}

static const GDBusInterfaceVTable manager_vtable = { manager_call, NULL, NULL };
static const GDBusInterfaceVTable login1_vtable = { login1_call, NULL, NULL };

/* how many of the inhibitors handed out someone still holds (their write end open) */
static guint
inhibitors_held (void)
{
  guint n = 0;
  for (guint i = 0; i < fake.inhibitors->len; i++) {
    struct pollfd p = { g_array_index (fake.inhibitors, int, i), POLLIN, 0 };
    g_assert_cmpint (poll (&p, 1, 0), >=, 0);
    n += !(p.revents & POLLHUP);
  }
  return n;
}

static void
fake_reset (void)
{
  g_ptr_array_set_size (fake.clients, 0);
  for (guint i = 0; i < fake.inhibitors->len; i++)
    close (g_array_index (fake.inhibitors, int, i));
  g_array_set_size (fake.inhibitors, 0);
  fake.made = fake.starts = fake.stops = fake.deletes = fake.inhibits = 0;
}

/* ---- the main loop ---- */

static void
spin_ms (guint ms)
{
  gint64 end = g_get_monotonic_time () + ms * 1000;
  while (g_get_monotonic_time () < end)
    if (!g_main_context_iteration (NULL, FALSE))
      g_usleep (2000);
}

/* until `cond`, or a few seconds (an emulated arm64 is slow): what it came to */
#define SPIN_UNTIL(cond)                                                                                              \
  G_STMT_START                                                                                                        \
  {                                                                                                                   \
    gint64 end_ = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;                                                      \
    while (!(cond) && g_get_monotonic_time () < end_)                                                                 \
      if (!g_main_context_iteration (NULL, FALSE))                                                                    \
        g_usleep (2000);                                                                                              \
  }                                                                                                                   \
  G_STMT_END

/* ---- the window's part, recorded ---- */

typedef struct {
  gboolean drawing, compass, screen_on;
  guint ticks; /* the test timer's */
} Window;

static void
on_drawing (gboolean on, gpointer data)
{
  ((Window *) data)->drawing = on;
}

static void
on_compass (gboolean on, gpointer data)
{
  ((Window *) data)->compass = on;
}

static void
on_screen_on (gboolean on, gpointer data)
{
  ((Window *) data)->screen_on = on;
}

static gboolean
tick (gpointer data)
{
  ((Window *) data)->ticks++;
  return G_SOURCE_CONTINUE;
}

typedef struct {
  NaviPosition *position;
  NaviAwake *awake;
  Window win;
} Navi;

static void
navi_up (Navi *n)
{
  static const NaviAwakeHooks hooks = { on_drawing, on_compass, on_screen_on };
  fake_reset ();
  *n = (Navi) { 0 };
  n->position = navi_position_new ();
  n->awake = navi_awake_new (n->position, &hooks, &n->win);
  navi_awake_add_timer (n->awake, 30, tick, &n->win); /* the minute's check, the stats' second: a timer of its */
}

static void
navi_down (Navi *n)
{
  g_clear_pointer (&n->awake, navi_awake_free);
  g_clear_object (&n->position);
  spin_ms (200); /* the last calls answered */
}

/* the timers stopped: none on the main loop, and none fires */
static void
assert_timers_stopped (Navi *n)
{
  g_assert_cmpuint (navi_awake_timers_attached (n->awake), ==, 0);
  guint before = n->win.ticks;
  spin_ms (200);
  g_assert_cmpuint (n->win.ticks, ==, before);
}

static void
assert_timers_running (Navi *n)
{
  g_assert_cmpuint (navi_awake_timers_attached (n->awake), ==, 1);
  guint before = n->win.ticks;
  SPIN_UNTIL (n->win.ticks > before);
  g_assert_cmpuint (n->win.ticks, >, before);
}

/* ---- the cases ---- */

static void
test_table (void)
{
  for (NaviMode m = NAVI_MODE_BROWSE; m <= NAVI_MODE_ARRIVED; m++) {
    gboolean driving = m == NAVI_MODE_DRIVE;
    NaviRun seen = navi_run_for (m, NAVI_SEEN), hidden = navi_run_for (m, NAVI_UNSEEN);
    NaviRun closed = navi_run_for (m, NAVI_CLOSED);
    g_assert_true (seen.position && seen.compass && seen.drawing && seen.timers);
    g_assert_cmpint (seen.sleep_block, ==, driving);
    g_assert_cmpint (seen.screen_on, ==, driving || m == NAVI_MODE_ARRIVED);
    /* out of sight: the position and the inhibitor with a route only; never the frames, the compass, the timers */
    g_assert_cmpint (hidden.position, ==, driving);
    g_assert_cmpint (hidden.sleep_block, ==, driving);
    g_assert_false (hidden.compass || hidden.drawing || hidden.timers || hidden.screen_on);
    g_assert_false (closed.position || closed.compass || closed.drawing || closed.timers || closed.screen_on ||
                    closed.sleep_block);
  }
  /* seen: open, mapped, not suspended and in front, or never in front yet (a seat without a keyboard) */
  g_assert_cmpint (navi_seen_from (TRUE, TRUE, FALSE, TRUE, TRUE), ==, NAVI_SEEN);
  g_assert_cmpint (navi_seen_from (TRUE, TRUE, FALSE, FALSE, FALSE), ==, NAVI_SEEN);
  g_assert_cmpint (navi_seen_from (TRUE, TRUE, FALSE, FALSE, TRUE), ==, NAVI_UNSEEN); /* Recents, the shade, locked */
  g_assert_cmpint (navi_seen_from (TRUE, TRUE, TRUE, TRUE, TRUE), ==, NAVI_UNSEEN);   /* minimized: suspended */
  g_assert_cmpint (navi_seen_from (TRUE, FALSE, FALSE, TRUE, TRUE), ==, NAVI_UNSEEN); /* hidden */
  g_assert_cmpint (navi_seen_from (FALSE, TRUE, FALSE, TRUE, TRUE), ==, NAVI_CLOSED);
}

/* a route, the window hidden: GeoClue's client keeps running and the phone may not sleep; nothing draws, no compass,
 * no timer; seen again, it is all back, on the same client */
static void
test_route_hidden (void)
{
  Navi n;
  navi_up (&n);
  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  SPIN_UNTIL (clients_started () == 1 && inhibitors_held () == 1);
  g_assert_cmpuint (clients_started (), ==, 1);
  g_assert_cmpuint (inhibitors_held (), ==, 1);
  g_assert_true (n.win.drawing && n.win.compass && n.win.screen_on);
  assert_timers_running (&n);

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_UNSEEN);
  g_assert_false (n.win.drawing || n.win.compass || n.win.screen_on);
  assert_timers_stopped (&n);
  g_assert_cmpuint (fake.stops, ==, 0);
  g_assert_cmpuint (clients_started (), ==, 1);
  g_assert_cmpuint (inhibitors_held (), ==, 1);
  g_assert_true (navi_awake_sleep_blocked (n.awake));

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  g_assert_true (n.win.drawing && n.win.compass && n.win.screen_on);
  assert_timers_running (&n);
  g_assert_cmpuint (fake.made, ==, 1);
  g_assert_cmpuint (fake.inhibits, ==, 1);
  navi_down (&n);
}

/* a route, the window closed: the drive is over, GeoClue's client stopped and deleted, the inhibitor let go */
static void
test_route_closed (void)
{
  Navi n;
  navi_up (&n);
  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  SPIN_UNTIL (clients_started () == 1 && inhibitors_held () == 1);
  g_assert_cmpuint (inhibitors_held (), ==, 1);

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_CLOSED);
  SPIN_UNTIL (fake.clients->len == 0 && inhibitors_held () == 0);
  g_assert_cmpuint (fake.clients->len, ==, 0);
  g_assert_cmpuint (fake.stops, ==, 1);
  g_assert_cmpuint (fake.deletes, ==, 1);
  g_assert_cmpuint (inhibitors_held (), ==, 0);
  g_assert_false (navi_awake_sleep_blocked (n.awake));
  g_assert_false (n.win.drawing || n.win.compass || n.win.screen_on);
  assert_timers_stopped (&n);
  navi_down (&n);
}

/* no route, the window hidden: GeoClue's client stopped and deleted, no compass, no timer, nothing drawn, and no
 * inhibitor at any time; seen again, a new client */
static void
test_no_route_hidden (void)
{
  Navi n;
  navi_up (&n);
  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN);
  SPIN_UNTIL (clients_started () == 1);
  g_assert_cmpuint (clients_started (), ==, 1);
  assert_timers_running (&n);

  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_UNSEEN);
  SPIN_UNTIL (fake.clients->len == 0);
  g_assert_cmpuint (fake.clients->len, ==, 0);
  g_assert_cmpuint (fake.stops, ==, 1);
  g_assert_cmpuint (fake.deletes, ==, 1);
  g_assert_false (n.win.drawing || n.win.compass || n.win.screen_on);
  assert_timers_stopped (&n);

  navi_awake_update (n.awake, NAVI_MODE_ROUTES, NAVI_SEEN); /* a route looked at is not one being driven */
  SPIN_UNTIL (clients_started () == 1);
  g_assert_cmpuint (clients_started (), ==, 1);
  g_assert_cmpuint (fake.made, ==, 2);
  assert_timers_running (&n);
  navi_awake_update (n.awake, NAVI_MODE_ROUTES, NAVI_UNSEEN);
  SPIN_UNTIL (fake.clients->len == 0);
  g_assert_cmpuint (fake.clients->len, ==, 0);
  assert_timers_stopped (&n);
  g_assert_cmpuint (fake.inhibits, ==, 0);
  navi_down (&n);
}

/* no route, the window closed */
static void
test_no_route_closed (void)
{
  Navi n;
  navi_up (&n);
  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN);
  SPIN_UNTIL (clients_started () == 1);
  g_assert_cmpuint (clients_started (), ==, 1);

  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_CLOSED);
  SPIN_UNTIL (fake.clients->len == 0);
  g_assert_cmpuint (fake.clients->len, ==, 0);
  g_assert_cmpuint (fake.stops, ==, 1);
  assert_timers_stopped (&n);
  g_assert_false (n.win.drawing || n.win.compass || n.win.screen_on);
  g_assert_cmpuint (fake.inhibits, ==, 0);
  navi_down (&n);
}

/* Recents opened and closed fast: the starts and stops in between leave no client behind, and one at the end */
static void
test_seen_quickly (void)
{
  Navi n;
  navi_up (&n);
  for (int i = 0; i < 4; i++) {
    navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN);
    spin_ms (i * 15); /* stopped at each step of the client's making */
    navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_UNSEEN);
  }
  spin_ms (300);
  SPIN_UNTIL (fake.clients->len == 0);
  g_assert_cmpuint (fake.clients->len, ==, 0);
  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN);
  SPIN_UNTIL (clients_started () == 1);
  spin_ms (200);
  g_assert_cmpuint (fake.clients->len, ==, 1);
  g_assert_cmpuint (clients_started (), ==, 1);
  navi_down (&n);
  g_assert_cmpuint (fake.clients->len, ==, 0);
}

/* held: logind was asked once more, and Navi has the inhibitor; let go: neither Navi nor anyone else has one */
#define ASSERT_HELD(asked)                                                                                            \
  G_STMT_START                                                                                                        \
  {                                                                                                                   \
    SPIN_UNTIL (fake.inhibits == (asked) && navi_awake_sleep_blocked (n.awake));                                     \
    g_assert_cmpuint (fake.inhibits, ==, (asked));                                                                    \
    g_assert_true (navi_awake_sleep_blocked (n.awake));                                                               \
    g_assert_cmpuint (inhibitors_held (), ==, 1);                                                                     \
  }                                                                                                                   \
  G_STMT_END
#define ASSERT_LET_GO()                                                                                               \
  G_STMT_START                                                                                                        \
  {                                                                                                                   \
    SPIN_UNTIL (inhibitors_held () == 0);                                                                             \
    g_assert_cmpuint (inhibitors_held (), ==, 0);                                                                     \
    g_assert_false (navi_awake_sleep_blocked (n.awake));                                                              \
  }                                                                                                                   \
  G_STMT_END

/* logind's sleep inhibitor: held while a route is driven, seen or not; let go on arrival, when the drive ends or the
 * route is dropped, and when Navi quits; never without a route */
static void
test_sleep_block (void)
{
  Navi n;
  navi_up (&n);
  for (NaviMode m = NAVI_MODE_BROWSE; m <= NAVI_MODE_ROUTES; m++)
    navi_awake_update (n.awake, m, NAVI_SEEN);
  spin_ms (200);
  g_assert_cmpuint (fake.inhibits, ==, 0);

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN); /* Go */
  ASSERT_HELD (1);
  g_assert_cmpstr (fake.inhibit_args, ==, "('sleep', 'VibeOS Navi', 'Guiding to a destination', 'block')");
  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_UNSEEN); /* in the background, or the screen off */
  spin_ms (100);
  ASSERT_HELD (1);

  navi_awake_update (n.awake, NAVI_MODE_ARRIVED, NAVI_UNSEEN); /* arrived */
  ASSERT_LET_GO ();

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  ASSERT_HELD (2);
  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN); /* End navigation */
  ASSERT_LET_GO ();

  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  ASSERT_HELD (3);
  navi_awake_update (n.awake, NAVI_MODE_PLACE, NAVI_SEEN); /* the route dropped (a new destination) */
  ASSERT_LET_GO ();

  /* driving and done before logind is asked, or before it answers: nothing held once it is all answered */
  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  navi_awake_update (n.awake, NAVI_MODE_BROWSE, NAVI_SEEN);
  spin_ms (300);
  ASSERT_LET_GO ();
  g_assert_cmpuint (fake.inhibits, <=, 4);

  guint asked = fake.inhibits + 1;
  navi_awake_update (n.awake, NAVI_MODE_DRIVE, NAVI_SEEN);
  ASSERT_HELD (asked);
  navi_down (&n); /* quit while driving */
  SPIN_UNTIL (inhibitors_held () == 0);
  g_assert_cmpuint (inhibitors_held (), ==, 0);
  g_assert_cmpuint (fake.inhibits, ==, asked);
}

/* Navi as the launcher starts it (D-Bus activation: --gapplication-service) quits as soon as nothing holds it: the
 * window's close lets go of the last hold (GtkApplication holds the application for each window), so a closed Navi
 * does not live on. A minimized one is not closed: its window still holds it (navi-awake.h puts it to sleep). */
static gint64 released_at;

static gboolean
release_later (gpointer app)
{
  released_at = g_get_monotonic_time ();
  g_application_release (app);
  return G_SOURCE_REMOVE;
}

static void
on_activate (GApplication *app)
{
  g_application_hold (app); /* the window */
  g_timeout_add (100, release_later, app);
}

static gboolean
activate_app (gpointer data)
{
  g_dbus_connection_call (fake.conn, "vibeos.NaviAwakeTest", "/vibeos/NaviAwakeTest", "org.freedesktop.Application",
                          "Activate", g_variant_new ("(a{sv})", NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                          NULL);
  return G_SOURCE_REMOVE;
}

static void
test_service_quits (void)
{
  g_autoptr (GApplication) app = g_application_new ("vibeos.NaviAwakeTest", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  char *argv[] = { "vibeos-navi", "--gapplication-service", NULL };
  released_at = 0;
  g_timeout_add (100, activate_app, NULL);
  g_assert_cmpint (g_application_run (app, 2, argv), ==, 0);
  g_assert_cmpint (released_at, >, 0);
  g_assert_cmpint (g_get_monotonic_time () - released_at, <, 2 * G_USEC_PER_SEC); /* not GLib's 10 s wait */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  /* a bus of the test's own, as the session's and the system's: the fake GeoClue and logind are on it */
  g_autoptr (GTestDBus) bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address (bus), TRUE);
  g_autoptr (GError) error = NULL;
  fake.conn = g_dbus_connection_new_for_address_sync (g_test_dbus_get_bus_address (bus),
                                                      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                      NULL, NULL, &error);
  g_assert_no_error (error);
  fake.info = g_dbus_node_info_new_for_xml (GEOCLUE_XML, &error);
  g_assert_no_error (error);
  fake.clients = g_ptr_array_new_with_free_func (client_free);
  fake.inhibitors = g_array_new (FALSE, FALSE, sizeof (int));
  g_dbus_connection_register_object (fake.conn, "/org/freedesktop/GeoClue2/Manager",
                                     g_dbus_node_info_lookup_interface (fake.info, "org.freedesktop.GeoClue2.Manager"),
                                     &manager_vtable, NULL, NULL, &error);
  g_assert_no_error (error);
  g_dbus_connection_register_object (fake.conn, "/org/freedesktop/login1",
                                     g_dbus_node_info_lookup_interface (fake.info, "org.freedesktop.login1.Manager"),
                                     &login1_vtable, NULL, NULL, &error);
  g_assert_no_error (error);
  for (int i = 0; i < 2; i++) {
    g_autoptr (GVariant) owned = g_dbus_connection_call_sync (
      fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new ("(su)", i ? "org.freedesktop.login1" : "org.freedesktop.GeoClue2", 0), G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_assert_no_error (error);
  }

  g_test_add_func ("/awake/table", test_table);
  g_test_add_func ("/awake/route-hidden", test_route_hidden);
  g_test_add_func ("/awake/route-closed", test_route_closed);
  g_test_add_func ("/awake/no-route-hidden", test_no_route_hidden);
  g_test_add_func ("/awake/no-route-closed", test_no_route_closed);
  g_test_add_func ("/awake/seen-quickly", test_seen_quickly);
  g_test_add_func ("/awake/sleep-block", test_sleep_block);
  g_test_add_func ("/awake/service-quits", test_service_quits);
  int status = g_test_run ();
  fake_reset ();
  g_object_unref (fake.conn);
  g_test_dbus_down (bus);
  return status;
}
