/* Notifications, Apps and Default apps. An app's notifications are GNOME's per-app settings
 * (org.gnome.desktop.notifications.application at .../application/<munged id>/, listed in application-children).
 * GNOME Shell follows them (messageTray.js NotificationApplicationPolicy): "enable" keeps the app's notifications
 * or drops them, "show-banners" (with the global one, Do not disturb) lets them pop up (VibeOS's headsup.js
 * asks the policy), "show-in-lock-screen" (with the global one) puts the app's icon on the lock screen
 * (keyguard.js and aod.js ask the policy). Emergency alerts are cellbroadcastd's (org.freedesktop.cbd). */
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#include <string.h>

#define NOTIFY_APP "org.gnome.desktop.notifications.application"

static gint
by_name (gconstpointer a, gconstpointer b, gpointer data)
{
  g_autofree char *x = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) a), -1);
  g_autofree char *y = g_utf8_casefold (g_app_info_get_display_name ((GAppInfo *) b), -1);
  return g_utf8_collate (x, y);
}

/* a relocatable schema at <prefix><munged id>/, or NULL when it is not installed */
static GSettings *
app_settings (const char *schema, const char *prefix, GAppInfo *app)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = src ? g_settings_schema_source_lookup (src, schema, TRUE) : NULL;
  if (!sc)
    return NULL;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_autofree char *path = g_strconcat (prefix, id, "/", NULL);
  return g_settings_new_full (sc, NULL, path);
}

/* GNOME Settings' way: an app's notification settings count once its id is in application-children */
static GSettings *
notify_settings (GAppInfo *app)
{
  GSettings *s = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (!s)
    return NULL;
  g_autoptr (GSettings) top = ns_settings ("org.gnome.desktop.notifications");
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  g_auto (GStrv) kids = top ? g_settings_get_strv (top, "application-children") : NULL;
  if (top && !g_strv_contains ((const char *const *) kids, id)) {
    GStrvBuilder *b = g_strv_builder_new ();
    g_strv_builder_addv (b, (const char **) kids);
    g_strv_builder_add (b, id);
    g_auto (GStrv) nk = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    g_settings_set_strv (top, "application-children", (const char *const *) nk);
    if (ns_has_key (s, "application-id"))
      g_settings_set_string (s, "application-id", g_app_info_get_id (app));
  }
  return s;
}

static gboolean
uses_notifications (GAppInfo *app, const char *const *kids)
{
  if (!G_IS_DESKTOP_APP_INFO (app) || !g_app_info_should_show (app))
    return FALSE;
  g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
  return g_desktop_app_info_get_boolean (G_DESKTOP_APP_INFO (app), "X-GNOME-UsesNotifications")
         || g_strv_contains (kids, id);
}

/* ---- an app's notifications ---- */

static void
app_notify_page (GtkWidget *row, gpointer app)
{
  GSettings *s = notify_settings (app);
  if (!s)
    return;
  GtkWidget *c;
  NsPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-notify", s, g_object_unref);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_switch (g, "Allow notifications", NULL, s, "enable");
  g = ns_group (c, NULL);
  GtkWidget *rows[4] = { NULL };
  guint n = 0;
  if (ns_has_key (s, "show-banners"))
    rows[n++] = ns_row_switch (g, "Pop-up", "Show a banner at the top of the screen", s, "show-banners");
  if (ns_has_key (s, "show-in-lock-screen"))
    rows[n++] = ns_row_switch (g, "Show on the lock screen", "The app's icon below the clock", s,
                               "show-in-lock-screen");
  /* No "Sound" switch (enable-sound-alerts): GNOME Shell honours it, but only for a notification that names a
   * sound, and no app here sends one (Sound and vibration says more). */
  for (guint i = 0; i < n; i++)
    g_settings_bind (s, "enable", rows[i], "sensitive", G_SETTINGS_BIND_GET);
  ns_push (page);
}

static void
allowed_changed (GSettings *s, const char *key, GtkWidget *row)
{
  ns_row_set_subtitle (row, g_settings_get_boolean (s, "enable") ? "Allowed" : "Off");
}

/* ---- Emergency alerts: cellbroadcastd notifies of the alert levels its "levels" flags name ---- */

/* the alerts come only from a modem with cell broadcast: the row shows once ModemManager says one has it */
static void
cbc_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  gboolean any = FALSE;
  if (v) {
    g_autoptr (GVariant) objs = g_variant_get_child_value (v, 0);
    GVariantIter it;
    GVariant *ifaces;
    g_variant_iter_init (&it, objs);
    while (!any && g_variant_iter_next (&it, "{&o@a{sa{sv}}}", NULL, &ifaces)) {
      g_autoptr (GVariant) cb = g_variant_lookup_value (ifaces, "org.freedesktop.ModemManager1.Modem.CellBroadcast",
                                                        NULL);
      any = cb != NULL;
      g_variant_unref (ifaces);
    }
  }
  gtk_widget_set_visible (row, any);
  g_object_unref (row);
}

static void
alerts_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Emergency alerts", &c);
  GSettings *cbd = ns_page_settings (page, "org.freedesktop.cbd");
  GtkWidget *g = ns_group (c, "Alerts from the mobile network");
  ns_row_flag (g, "Extreme threats", "To life and safety", cbd, "levels", "extreme");
  ns_row_flag (g, "Severe threats", "To life and safety", cbd, "levels", "severe");
  ns_row_flag (g, "Public safety", "What to do in a risky situation", cbd, "levels", "public-safety");
  ns_row_flag (g, "Amber alerts", "Child abduction emergencies", cbd, "levels", "amber");
  ns_row_flag (g, "Test alerts", "Periodic tests of the alert system", cbd, "levels", "test");
  ns_note (c, "Presidential alerts are always shown. Alerts need a SIM and a modem that receives them.");
  ns_push (page);
}

/* a hidden row for the alerts, shown if the modem can receive them */
static void
alerts_row (GtkWidget *g)
{
  if (!ns_settings_exists ("org.freedesktop.cbd"))
    return;
  GtkWidget *r = ns_row (g, NULL, "Emergency alerts", "Cell broadcast warnings");
  ns_row_on_activate (r, alerts_page, NULL);
  gtk_widget_set_visible (r, FALSE);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.ModemManager1", "/org/freedesktop/ModemManager1",
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                            G_VARIANT_TYPE ("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, cbc_got,
                            g_object_ref (r));
}

/* ---- Notifications ---- */

static void
app_switched (GtkWidget *sw, GParamSpec *ps, GAppInfo *app)
{
  if (ns_switch_syncing (GTK_WIDGET (sw)))
    return;
  g_autoptr (GSettings) s = notify_settings (app);
  if (s)
    g_settings_set_boolean (s, "enable", ns_switch_get_active (sw));
}

static void app_notifications_page (GtkWidget *row, gpointer data);

void
ns_page_notifications (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Notifications", &c);
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  /* Android 17's groups: the apps' own settings on a page of their own, then what the lock screen shows */
  GtkWidget *g = ns_group (c, "Manage");
  GtkWidget *r = ns_row (g, NULL, "App notifications", "Control notifications from individual apps");
  ns_row_on_activate (r, app_notifications_page, NULL);
  g = ns_group (c, "Privacy");
  ns_row_switch (g, "Notifications on lock screen", "Their apps' icons below the clock", n, "show-in-lock-screen");
  g = ns_group (c, NULL);
  r = ns_row_switch (g, "Do not disturb", "No pop-ups: notifications wait in the list", NULL, NULL);
  g_settings_bind (n, "show-banners", ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  /* No "Wake the screen" (Phosh's wakeup-screen-* keys): vibeos leaves the screen off for a notification.
   * Besides the power key and a double tap, only a full-screen app (an incoming call, a ringing alarm: service.js
   * LaunchFullScreen) turns it on. */
  alerts_row (g);
  ns_push (page);
}

/* an app's switch follows its key: turned off on the app's own page, or elsewhere, the list says so when it is
 * shown again */
static void
app_enable_changed (GSettings *s, const char *key, GtkWidget *sw)
{
  ns_switch_quiet (sw, g_settings_get_boolean (s, key));
}

/* each app that notifies, with its switch; a tap opens the app's own page */
static void
app_notifications_page (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("App notifications", &c);
  GSettings *n = ns_page_settings (page, "org.gnome.desktop.notifications");
  GtkWidget *g = ns_group (c, NULL);
  g_auto (GStrv) kids = g_settings_get_strv (n, "application-children");
  GList *all = g_app_info_get_all ();
  all = g_list_sort_with_data (all, by_name, NULL);
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) sc = g_settings_schema_source_lookup (src, NOTIFY_APP, TRUE);
  for (GList *l = all; sc && l; l = l->next) {
    GAppInfo *app = l->data;
    if (!uses_notifications (app, (const char *const *) kids))
      continue;
    g_autofree char *id = ns_munge_app_id (g_app_info_get_id (app));
    g_autofree char *path = g_strconcat ("/org/gnome/desktop/notifications/application/", id, "/", NULL);
    GSettings *s = g_settings_new_full (sc, NULL, path);
    GtkWidget *ar = ns_row_app (g, app, NULL);
    GtkWidget *sw = ns_row_add_switch (ar, TRUE);
    ns_switch_quiet (sw, g_settings_get_boolean (s, "enable"));
    g_object_set_data_full (G_OBJECT (ar), "ns-notify", s, g_object_unref);
    g_signal_connect_object (s, "changed::enable", G_CALLBACK (app_enable_changed), sw, 0);
    g_object_set_data_full (G_OBJECT (ar), "ns-app", g_object_ref (app), g_object_unref);
    g_signal_connect (sw, "notify::active", G_CALLBACK (app_switched), app);
    ns_row_on_activate (ar, app_notify_page, app);
  }
  g_list_free_full (all, g_object_unref);
  ns_push (page);
}

/* ---- an app ---- */

static void
open_app (GtkWidget *row, gpointer app)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GAppLaunchContext) ctx = G_APP_LAUNCH_CONTEXT (
    gdk_display_get_app_launch_context (gtk_widget_get_display (row)));
  if (!g_app_info_launch (app, NULL, ctx, &err))
    ns_row_set_subtitle (row, err->message);
}

/* the desktop file its package installed: a sandboxed app's desktop file is vibeos-sandbox's copy, and names it */
static char *
package_file (GAppInfo *app)
{
  if (!G_IS_DESKTOP_APP_INFO (app))
    return NULL;
  char *source = g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (app), "X-VibeOS-Sandbox-Source");
  if (source && *source)
    return source;
  g_free (source);
  return g_strdup (g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app)));
}

/* the app id it runs sandboxed as, or NULL: not sandboxed */
static char *
sandbox_id (GAppInfo *app)
{
  char *id = G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_string (G_DESKTOP_APP_INFO (app), "X-VibeOS-Sandbox")
                                         : NULL;
  if (id && !*id)
    g_clear_pointer (&id, g_free);
  return id;
}

/* its package and version: dpkg-query, in the background */
static void
version_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autofree char *out = NULL;
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out) {
    /* "pkg: /usr/share/applications/x.desktop" then the version, both from one shell */
    g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", 2);
    if (l[0] && l[1])
      ns_row_set_subtitle (row, g_strstrip (l[1]));
  }
  g_object_unref (row);
}

static void
version_row (GtkWidget *g, GAppInfo *app)
{
  g_autofree char *file = package_file (app);
  if (!file || !g_str_has_prefix (file, "/usr/"))
    return;
  GtkWidget *r = ns_row_info (g, "Version", NULL);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "echo \"$p\" && dpkg-query -W -f='${Version}' \"$p\"",
                                                "sh", file, NULL);
  if (p)
    g_subprocess_communicate_utf8_async (p, NULL, NULL, version_got, g_object_ref (r));
}

/* Uninstall: only an app the user installed (from Software or App Installer: those run sandboxed); VibeOS's own apps
 * and the rest of the system get no button, as system apps on Android. The package that owns the app's desktop file
 * goes through App Installer (vibeos.Installer1 RemovePackage, as Software's Uninstall does), which does the rest by
 * its own rules: its sheet over this window asks first, naming every package that goes with it and saying the app's
 * data is deleted (vibeos-sandbox deletes it once dpkg has removed the package); it refuses part of VibeOS; polkit
 * asks for the password; PackageKit removes it. The button follows the job to its end. */
#define INSTALLER_NAME  "vibeos.Installer"
#define INSTALLER_PATH  "/vibeos/Installer"
#define INSTALLER_IFACE "vibeos.Installer1"

typedef struct {
  GtkWidget       *button;
  char            *pkg;
  GDBusConnection *bus;
  guint            job, sub;
} Uninstall;

static void
uninstall_free (Uninstall *u)
{
  if (u->sub)
    g_dbus_connection_signal_unsubscribe (u->bus, u->sub);
  g_clear_object (&u->bus);
  g_object_unref (u->button);
  g_free (u->pkg);
  g_free (u);
}

/* the job ended: "done" leaves the app's page (the app is gone), anything else gives the button back */
static void
uninstall_end (Uninstall *u, const char *result, const char *message)
{
  if (!g_strcmp0 (result, "done")) {
    if (gtk_widget_get_root (u->button))
      ns_pop (u->button);
  } else {
    /* "cancelled": the sheet or the password was declined, nothing to report */
    gtk_button_set_label (GTK_BUTTON (u->button),
                          !g_strcmp0 (result, "cancelled") || !message || !*message ? "Uninstall" : message);
    gtk_widget_set_sensitive (u->button, TRUE);
  }
  uninstall_free (u);
}

static void
uninstall_signal (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
                  GVariant *params, gpointer data)
{
  Uninstall *u = data;
  guint job;
  const char *result, *message;
  if (!u->job || strcmp (signal, "Finished") || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(uss)")))
    return;
  g_variant_get (params, "(u&s&s)", &job, &result, &message);
  if (job == u->job)
    uninstall_end (u, result, message);
}

static void
uninstall_started (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GVariant) r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
  if (!r) {
    gboolean missing = g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN);
    uninstall_end (u, "failed", missing ? "App Installer is not installed: it uninstalls apps"
                                        : "Could not uninstall, try again");
    return;
  }
  g_variant_get (r, "(u)", &u->job);
}

static void
uninstall_call (Uninstall *u, const char *handle)
{
  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  if (handle) {
    g_autofree char *parent = g_strconcat ("wayland:", handle, NULL);
    g_variant_builder_add (&options, "{sv}", "parent-window", g_variant_new_string (parent));
  }
  /* the job's signals go to the caller alone, from App Installer's path: subscribed before the call, so Finished
   * cannot come first. App Installer is D-Bus activatable: the call starts it */
  u->sub = g_dbus_connection_signal_subscribe (u->bus, INSTALLER_NAME, INSTALLER_IFACE, NULL, INSTALLER_PATH, NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE, uninstall_signal, u, NULL);
  g_dbus_connection_call (u->bus, INSTALLER_NAME, INSTALLER_PATH, INSTALLER_IFACE, "RemovePackage",
                          g_variant_new ("(sa{sv})", u->pkg, &options), G_VARIANT_TYPE ("(u)"),
                          G_DBUS_CALL_FLAGS_NONE, 60000, NULL, uninstall_started, u);
}

#ifdef GDK_WINDOWING_WAYLAND
static void
uninstall_exported (GdkToplevel *toplevel, const char *handle, gpointer data)
{
  /* the handle stays exported while the window lives: App Installer's sheet is made transient for it */
  uninstall_call (data, handle);
}
#endif

static void
uninstall_tapped (GtkButton *b, Uninstall *proto)
{
  g_autoptr (GError) err = NULL;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    gtk_button_set_label (b, err->message);
    return;
  }
  Uninstall *u = g_new0 (Uninstall, 1);
  u->button = g_object_ref (proto->button);
  u->pkg = g_strdup (proto->pkg);
  u->bus = bus;
  gtk_button_set_label (b, "Uninstalling…");
  gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);
#ifdef GDK_WINDOWING_WAYLAND
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (b));
  GdkSurface *surface = native ? gtk_native_get_surface (native) : NULL;
  if (surface && GDK_IS_WAYLAND_TOPLEVEL (surface) &&
      gdk_wayland_toplevel_export_handle (GDK_TOPLEVEL (surface), uninstall_exported, u, NULL))
    return;
#endif
  uninstall_call (u, NULL);
}

static void
uninstall_proto_free (Uninstall *u)
{
  g_object_unref (u->button);
  g_free (u->pkg);
  g_free (u);
}

static void
uninstall_got (GObject *src, GAsyncResult *res, gpointer data)
{
  Uninstall *u = data;
  g_autofree char *out = NULL;
  /* the package that owns the desktop file */
  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL) && out &&
      *g_strstrip (out) && !strchr (out, '\n')) {
    u->pkg = g_strdup (out);
    g_signal_connect_data (u->button, "clicked", G_CALLBACK (uninstall_tapped), u,
                           (GClosureNotify) (void (*) (void)) uninstall_proto_free, 0);
    gtk_widget_set_visible (u->button, TRUE);
    return;
  }
  uninstall_proto_free (u);
}

static void
uninstall_button (GtkWidget *c, GAppInfo *app)
{
  g_autofree char *sandboxed = sandbox_id (app);
  g_autofree char *file = package_file (app);
  if (!sandboxed || !file || !g_str_has_prefix (file, "/usr/share/"))
    return;
  GtkWidget *b = ns_button_new ("Uninstall", FALSE);
  gtk_widget_add_css_class (b, "ns-wide");
  gtk_widget_set_visible (b, FALSE);
  gtk_box_append (GTK_BOX (c), b);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1",
                                                "sh", file, NULL);
  if (!p)
    return;
  Uninstall *u = g_new0 (Uninstall, 1);
  u->button = g_object_ref (b);
  g_subprocess_communicate_utf8_async (p, NULL, NULL, uninstall_got, u);
}

/* ---- Storage and cache: what the app takes, as on Android: its package (dpkg's Installed-Size; not the libraries it
 * pulled in, which only apt could tell, and slowly) and, for an app that runs sandboxed, its home in
 * ~/.local/share/vibeos-sandbox/data/<package>, the cache in it (.cache) apart. du measures them in the background;
 * the rows read "Calculating…" until then. VibeOS's own apps have no home of their own there: the package alone. ---- */

typedef struct {
  char      *file;               /* the desktop file its package installed */
  gboolean   sandboxed;
  gboolean   known;
  gboolean   measuring, again;   /* du is running; something changed meanwhile: measure once more */
  char      *pkg;
  guint64    app, data, cache;   /* bytes; data without the cache */
  GtkWidget *row;                /* the app page's row (weak) */
  GtkWidget *rows[4];            /* the open storage page's App size, User data, Cache, Total (weak) */
  GtkWidget *clear;              /* its Clear cache (weak) */
} Storage;

/* keeps *where pointing at `w` while it lives */
static void
track (GtkWidget **where, GtkWidget *w)
{
  if (*where)
    g_object_remove_weak_pointer (G_OBJECT (*where), (gpointer *) where);
  *where = w;
  if (w)
    g_object_add_weak_pointer (G_OBJECT (w), (gpointer *) where);
}

static void
storage_free (Storage *st)
{
  track (&st->row, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (st->rows); i++)
    track (&st->rows[i], NULL);
  track (&st->clear, NULL);
  g_free (st->file);
  g_free (st->pkg);
  g_free (st);
}

static void
storage_show (Storage *st)
{
  if (!st->known)
    return;
  guint64 total = st->app + st->data + st->cache;
  if (st->row) {
    g_autofree char *size = g_format_size (total);
    g_autofree char *used = g_strdup_printf ("%s used", size);
    ns_row_set_subtitle (st->row, used);
  }
  guint64 values[4] = { st->app, st->data, st->cache, total };
  for (guint i = 0; i < G_N_ELEMENTS (st->rows); i++)
    if (st->rows[i]) {
      g_autofree char *v = g_format_size (values[i]);
      ns_row_set_subtitle (st->rows[i], v);
    }
  if (st->clear) {
    gtk_button_set_label (GTK_BUTTON (st->clear), "Clear cache");
    gtk_widget_set_sensitive (st->clear, st->cache > 0);
  }
}

static void measure (GObject *owner);

/* the package's name, its Installed-Size (KiB), then the home's and its cache's (KiB, du) when sandboxed */
static void
measured (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GObject) owner = data;
  Storage *st = g_object_get_data (owner, "ns-storage");
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, NULL);
  if (!st)
    return;
  st->measuring = FALSE;
  if (st->again) {
    st->again = FALSE;
    measure (owner);
    return;
  }
  if (!ok || !out)
    return;
  g_auto (GStrv) l = g_strsplit (g_strstrip (out), "\n", -1);
  if (g_strv_length (l) < 2 || !*l[0]) {
    if (st->row)
      ns_row_set_subtitle (st->row, "–");
    return;
  }
  g_free (st->pkg);
  st->pkg = g_strdup (l[0]);
  st->app = g_ascii_strtoull (l[1], NULL, 10) * 1024;
  guint64 home = l[2] ? g_ascii_strtoull (l[2], NULL, 10) * 1024 : 0;
  st->cache = l[2] && l[3] ? g_ascii_strtoull (l[3], NULL, 10) * 1024 : 0;
  st->data = home > st->cache ? home - st->cache : 0;
  st->known = TRUE;
  storage_show (st);
}

/* in the background, once at a time; the row and an open storage page show the answer */
static void
measure (GObject *owner)
{
  Storage *st = g_object_get_data (owner, "ns-storage");
  if (!st)
    return;
  if (st->measuring) {
    st->again = TRUE;
    return;
  }
  g_autofree char *homes = st->sandboxed ? g_build_filename (g_get_user_data_dir (), "vibeos-sandbox", "data", NULL)
                                         : g_strdup ("");
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "sh", "-c",
                                                "p=$(dpkg-query -S \"$1\" 2>/dev/null | head -n1 | cut -d: -f1) && "
                                                "[ -n \"$p\" ] || exit 1; echo \"$p\"; "
                                                "s=$(dpkg-query -W -f='${Installed-Size}\\n' \"$p\" | head -n1); "
                                                "echo \"${s:-0}\"; [ -n \"$2\" ] || exit 0; "
                                                "for d in \"$2/$p\" \"$2/$p/.cache\"; do "
                                                "if [ -d \"$d\" ] && [ ! -L \"$d\" ]; then du -skx \"$d\" | cut -f1; "
                                                "else echo 0; fi; done",
                                                "sh", st->file, homes, NULL);
  if (!p)
    return;
  st->measuring = TRUE;
  g_subprocess_communicate_utf8_async (p, NULL, NULL, measured, g_object_ref (owner));
}

static void
cache_cleared (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GObject) owner = data;
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
  measure (owner);
}

static void
clear_cache (GtkButton *b, GObject *owner)
{
  Storage *st = g_object_get_data (owner, "ns-storage");
  if (!st || !st->pkg)
    return;
  gtk_button_set_label (b, "Clearing…");
  gtk_widget_set_sensitive (GTK_WIDGET (b), FALSE);
  g_autoptr (GSubprocess) p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                NULL, "vibeos-sandbox", "clear-cache", st->pkg, NULL);
  if (p)
    g_subprocess_wait_async (p, NULL, cache_cleared, g_object_ref (owner));
  else
    measure (owner);
}

static void
storage_page (GtkWidget *row, gpointer data)
{
  GObject *owner = data;
  Storage *st = g_object_get_data (owner, "ns-storage");
  GtkWidget *c;
  NsPage *page = ns_page_new ("Storage and cache", &c);
  /* the storage page keeps the app's page (and the Storage) while it is open */
  g_object_set_data_full (G_OBJECT (page), "ns-storage-owner", g_object_ref (owner), g_object_unref);
  GtkWidget *g = ns_group (c, "Space used");
  static const char *const titles[] = { "App size", "User data", "Cache", "Total" };
  for (guint i = 0; i < G_N_ELEMENTS (titles); i++)
    track (&st->rows[i], ns_row_info (g, titles[i], "Calculating…"));
  GtkWidget *b = ns_button_new ("Clear cache", FALSE);
  gtk_widget_add_css_class (b, "ns-wide");
  gtk_widget_set_sensitive (b, FALSE);
  g_signal_connect_object (b, "clicked", G_CALLBACK (clear_cache), owner, 0);
  gtk_box_append (GTK_BOX (c), b);
  track (&st->clear, b);
  storage_show (st);
  ns_push (page);
  measure (owner);
}

static void
storage_row (GtkWidget *g, GAppInfo *app, GtkWidget *owner)
{
  g_autofree char *file = package_file (app);
  if (!file || !g_str_has_prefix (file, "/usr/"))
    return;
  g_autofree char *sandboxed = sandbox_id (app);
  Storage *st = g_new0 (Storage, 1);
  st->file = g_steal_pointer (&file);
  st->sandboxed = sandboxed != NULL;
  g_object_set_data_full (G_OBJECT (owner), "ns-storage", st, (GDestroyNotify) storage_free);
  track (&st->row, ns_row (g, "drive-harddisk-symbolic", "Storage and cache", "Calculating…"));
  /* only a sandboxed app has data and a cache of its own to show and clear */
  if (st->sandboxed)
    ns_row_on_activate (st->row, storage_page, owner);
  measure (G_OBJECT (owner));
}

/* the app's page is on top again (back from its permissions or its storage): what they are now */
static void
app_page_back (NsPage *page)
{
  measure (G_OBJECT (page));
  ns_app_permissions_refresh (page);
}

static void
app_page (GAppInfo *app)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (g_app_info_get_display_name (app), &c);
  g_object_set_data_full (G_OBJECT (page), "ns-app", g_object_ref (app), g_object_unref);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (head, "ns-page-head");
  GIcon *gi = g_app_info_get_icon (app);
  GtkWidget *img = gi ? gtk_image_new_from_gicon (gi) : gtk_image_new_from_icon_name ("application-x-executable");
  gtk_image_set_pixel_size (GTK_IMAGE (img), 72);
  GtkWidget *name = gtk_label_new (g_app_info_get_display_name (app));
  gtk_widget_add_css_class (name, "ns-big");
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_box_append (GTK_BOX (head), img);
  gtk_box_append (GTK_BOX (head), name);
  const char *desc = g_app_info_get_description (app);
  if (desc) {
    GtkWidget *d = gtk_label_new (desc);
    gtk_label_set_wrap (GTK_LABEL (d), TRUE);
    gtk_label_set_justify (GTK_LABEL (d), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (d, "ns-sub");
    gtk_box_append (GTK_BOX (head), d);
  }
  gtk_box_append (GTK_BOX (c), head);

  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Open", FALSE, open_app, app);

  g = ns_group (c, NULL);
  GSettings *ns = app_settings (NOTIFY_APP, "/org/gnome/desktop/notifications/application/", app);
  if (ns) {
    g_object_set_data_full (G_OBJECT (page), "ns-notify", ns, g_object_unref);
    GtkWidget *r = ns_row (g, "preferences-system-notifications-symbolic", "Notifications", NULL);
    allowed_changed (ns, NULL, r);
    g_signal_connect_object (ns, "changed::enable", G_CALLBACK (allowed_changed), r, 0);
    ns_row_on_activate (r, app_notify_page, app);
  }
  ns_app_permissions_row (g, app, page);
  storage_row (g, app, page);
  ns_page_on_back (page, app_page_back);
  /* No per-app "Sound and vibration" (feedbackd's per-app profile): feedbackd applies it to the events an app
   * sends it, and apps here send none (the Phone and the Clock use its Haptic interface, which the profile does
   * not touch). No "Scale to fit" either: that was phoc's; mutter has no such thing. */

  g = ns_group (c, NULL);
  version_row (g, app);
  ns_row_info (g, "App ID", g_app_info_get_id (app));
  uninstall_button (c, app);
  ns_push (page);
}

/* ---- Apps: one list, built whole: a list view binds its recycled rows again as they scroll in (labels laid out,
 * icons looked up), which a fling cannot keep up with on the phone; the icons still load only once shown ---- */

/* one app's page straight away (the launcher's "App info", GNOME Settings' "applications" panel with an app id) */
void
ns_page_app (const char *id)
{
  g_autofree char *file = g_str_has_suffix (id, ".desktop") ? g_strdup (id) : g_strconcat (id, ".desktop", NULL);
  g_autoptr (GDesktopAppInfo) app = g_desktop_app_info_new (file);
  if (app)
    app_page (G_APP_INFO (app));
  else
    ns_page_apps (NULL, NULL);
}

/* App icons as textures, each drawn once at the screen's size: GTK draws a scalable (SVG) icon from its paths on
 * every frame it shows, rasterised and uploaded again, which a fling past a screen full of them cannot afford. In
 * idle time after the page is up, a few at a time. */
typedef struct {
  GPtrArray   *images;      /* GtkImage, a weak ref each */
  GskRenderer *renderer;
  guint        next;
} IconBake;

static void
icon_bake_free (IconBake *b)
{
  g_ptr_array_unref (b->images);
  if (b->renderer) {
    gsk_renderer_unrealize (b->renderer);
    g_object_unref (b->renderer);
  }
  g_free (b);
}

static gboolean
icon_bake_step (gpointer data)
{
  IconBake *b = data;
  for (int n = 0; n < 4 && b->next < b->images->len; b->next++) {
    GWeakRef *ref = b->images->pdata[b->next];
    g_autoptr (GtkWidget) image = g_weak_ref_get (ref);
    if (!image || gtk_image_get_storage_type (GTK_IMAGE (image)) != GTK_IMAGE_GICON)
      continue;
    GdkDisplay *display = gtk_widget_get_display (image);
    if (!b->renderer) {
      b->renderer = gsk_cairo_renderer_new ();
      gsk_renderer_realize_for_display (b->renderer, display, NULL);
    }
    GListModel *monitors = gdk_display_get_monitors (display);
    g_autoptr (GdkMonitor) monitor = g_list_model_get_n_items (monitors) ? g_list_model_get_item (monitors, 0) : NULL;
    double scale = monitor ? gdk_monitor_get_scale (monitor) : 1;
    int size = gtk_image_get_pixel_size (GTK_IMAGE (image));
    float px = ceil (size * scale);
    g_autoptr (GtkIconPaintable) icon = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_for_display (display),
                                                                       gtk_image_get_gicon (GTK_IMAGE (image)), size,
                                                                       (int) ceil (scale), GTK_TEXT_DIR_NONE, 0);
    GtkSnapshot *s = gtk_snapshot_new ();
    gdk_paintable_snapshot (GDK_PAINTABLE (icon), s, px, px);
    g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (s);
    if (!node)
      continue;
    g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (b->renderer, node, &GRAPHENE_RECT_INIT (0, 0, px, px));
    gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
    gtk_image_set_pixel_size (GTK_IMAGE (image), size);
    n++;
  }
  return b->next < b->images->len ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
default_apps_tapped (GtkWidget *row, gpointer data)
{
  ns_page_default_apps (row, NULL);
}

static void
app_tapped (GtkWidget *row, gpointer app)
{
  app_page (app);
}

/* an app's row that opens its page, its icon baked later with the others */
static GtkWidget *
app_row (GtkWidget *g, GAppInfo *app, const char *subtitle, IconBake *bake)
{
  GtkWidget *ar = ns_row_app (g, app, subtitle);
  /* the row holds the app its page opens */
  g_object_set_data_full (G_OBJECT (ar), "ns-app", g_object_ref (app), g_object_unref);
  ns_row_on_activate (ar, app_tapped, app);
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, g_object_get_data (G_OBJECT (ar), "ns-icon"));
  g_ptr_array_add (bake->images, ref);
  return ar;
}

static IconBake *
icon_bake_new (void)
{
  IconBake *bake = g_new0 (IconBake, 1);
  bake->images = g_ptr_array_new_with_free_func ((GDestroyNotify) weak_ref_free);
  return bake;
}

static void
icon_bake_start (IconBake *bake)
{
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, icon_bake_step, bake, (GDestroyNotify) icon_bake_free);
}

/* the apps a launcher shows, by name */
static GList *
shown_apps (guint *n)
{
  GList *all = g_list_sort_with_data (g_app_info_get_all (), by_name, NULL), *shown = NULL;
  *n = 0;
  for (GList *l = all; l; l = l->next)
    if (g_app_info_should_show (l->data)) {
      shown = g_list_prepend (shown, g_object_ref (l->data));
      (*n)++;
    }
  g_list_free_full (all, g_object_unref);
  return g_list_reverse (shown);
}

/* ---- the apps opened last: GNOME Shell's record of when each app was last in use ---- */

typedef struct {
  char  *id;
  gint64 seen;
} Seen;

static void
seen_free (Seen *s)
{
  if (!s)
    return;             /* moved to the recent list */
  g_free (s->id);
  g_free (s);
}

static void
state_element (GMarkupParseContext *ctx, const char *name, const char **keys, const char **values, gpointer data,
               GError **error)
{
  if (g_strcmp0 (name, "application"))
    return;
  const char *id = NULL, *seen = NULL;
  for (guint i = 0; keys[i]; i++) {
    if (!g_strcmp0 (keys[i], "id"))
      id = values[i];
    else if (!g_strcmp0 (keys[i], "last-seen"))
      seen = values[i];
  }
  if (!id || !seen)
    return;
  Seen *s = g_new0 (Seen, 1);
  s->id = g_strdup (id);
  s->seen = g_ascii_strtoll (seen, NULL, 10);
  g_ptr_array_add (data, s);
}

static int
by_seen (gconstpointer a, gconstpointer b)
{
  gint64 x = (*(Seen **) a)->seen, y = (*(Seen **) b)->seen;
  return x < y ? 1 : x > y ? -1 : 0;
}

/* "2 minutes ago", as Android's list has it */
static char *
seen_ago (gint64 seen)
{
  gint64 s = MAX (g_get_real_time () / G_USEC_PER_SEC - seen, 0);
  if (s < 3600) {
    int m = (int) (s / 60);
    return g_strdup_printf (m == 1 ? "%d minute ago" : "%d minutes ago", m);
  }
  if (s < 86400) {
    int h = (int) (s / 3600);
    return g_strdup_printf (h == 1 ? "%d hour ago" : "%d hours ago", h);
  }
  int d = (int) (s / 86400);
  return g_strdup_printf (d == 1 ? "%d day ago" : "%d days ago", d);
}

/* up to `max` apps, newest first, that a launcher shows; Settings itself is left out, as Android's is */
static GPtrArray *
recent_apps (guint max)
{
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), "gnome-shell", "application_state", NULL);
  g_autofree char *text = NULL;
  gsize len;
  GPtrArray *recent = g_ptr_array_new_with_free_func ((GDestroyNotify) seen_free);
  if (!g_file_get_contents (path, &text, &len, NULL))
    return recent;
  g_autoptr (GPtrArray) all = g_ptr_array_new_with_free_func ((GDestroyNotify) seen_free);
  GMarkupParser parser = { .start_element = state_element };
  g_autoptr (GMarkupParseContext) ctx = g_markup_parse_context_new (&parser, 0, all, NULL);
  g_markup_parse_context_parse (ctx, text, len, NULL);
  g_ptr_array_sort (all, by_seen);
  const char *self = g_application_get_application_id (g_application_get_default ());
  g_autofree char *self_desktop = self ? g_strconcat (self, ".desktop", NULL) : NULL;
  for (guint i = 0; i < all->len && recent->len < max; i++) {
    Seen *s = all->pdata[i];
    if (s->seen <= 0 || !g_strcmp0 (s->id, self_desktop))
      continue;
    g_autoptr (GDesktopAppInfo) app = g_desktop_app_info_new (s->id);
    if (!app || !g_app_info_should_show (G_APP_INFO (app)))
      continue;
    g_ptr_array_add (recent, g_steal_pointer (&all->pdata[i]));
  }
  return recent;
}

static void ns_page_all_apps (GtkWidget *row, gpointer data);
static void apps_back (NsPage *page);

/* Android 17's: the apps opened last and a row to all of them, then the general settings */
void
ns_page_apps (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Apps", &c);
  IconBake *bake = icon_bake_new ();
  g_autoptr (GPtrArray) recent = recent_apps (3);
  guint n;
  GList *all = shown_apps (&n);
  g_list_free_full (all, g_object_unref);

  GtkWidget *g = ns_group (c, recent->len ? "Recently opened apps" : NULL);
  for (guint i = 0; i < recent->len; i++) {
    Seen *s = recent->pdata[i];
    g_autoptr (GDesktopAppInfo) app = g_desktop_app_info_new (s->id);
    g_autofree char *ago = seen_ago (s->seen);
    app_row (g, G_APP_INFO (app), ago, bake);
  }
  g_autofree char *see = g_strdup_printf ("See all %u apps", n);
  GtkWidget *r = ns_row (g, "go-next-symbolic", see, NULL);
  ns_row_on_activate (r, ns_page_all_apps, NULL);

  g = ns_group (c, "General");
  r = ns_row (g, NULL, "Default apps", "Browser, email, music, photos");
  ns_row_on_activate (r, default_apps_tapped, NULL);
  icon_bake_start (bake);
  ns_page_on_back (page, apps_back);
  ns_push (page);
}

/* Back from an app's page that uninstalled it: its row goes from the lists under it (an app's row holds the app,
 * "ns-app"; the desktop file is what is gone, GIO's own list may not know it yet) */
static void
hide_gone (GtkWidget *w)
{
  for (GtkWidget *c = gtk_widget_get_first_child (w); c; c = gtk_widget_get_next_sibling (c)) {
    GObject *app = g_object_get_data (G_OBJECT (c), "ns-app");
    const char *file = app && G_IS_DESKTOP_APP_INFO (app) ? g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (app))
                                                          : NULL;
    if (file && !g_file_test (file, G_FILE_TEST_EXISTS))
      gtk_widget_set_visible (c, FALSE);
    else
      hide_gone (c);
  }
}

static void
apps_back (NsPage *page)
{
  hide_gone (GTK_WIDGET (page));
}

/* every app a launcher shows, by name */
static void
ns_page_all_apps (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("All apps", &c);
  GtkWidget *g = ns_group (c, NULL);
  IconBake *bake = icon_bake_new ();
  guint n;
  GList *all = shown_apps (&n);
  for (GList *l = all; l; l = l->next)
    app_row (g, l->data, NULL, bake);
  icon_bake_start (bake);
  g_list_free_full (all, g_object_unref);
  ns_page_on_back (page, apps_back);
  ns_push (page);
}

void
ns_page_app_row (GtkWidget *row, gpointer id)
{
  ns_page_app (id);
}

/* ---- Default apps ---- */

typedef struct {
  const char *title, *const *types;
} Kind;

static const char *const web[] = { "x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                                   "x-scheme-handler/about", "x-scheme-handler/unknown", NULL };
static const char *const mail[] = { "x-scheme-handler/mailto", NULL };
static const char *const cal[] = { "text/calendar", NULL };
static const char *const music[] = { "audio/x-vorbis+ogg", "audio/mpeg", "audio/x-wav", "audio/x-flac", "audio/ogg",
                                     "audio/mp4", "audio/x-opus+ogg", NULL };
static const char *const video[] = { "video/x-ogm+ogg", "video/mp4", "video/webm", "video/x-matroska",
                                     "video/quicktime", "video/mpeg", NULL };
static const char *const photos[] = { "image/jpeg", "image/png", "image/gif", "image/webp", "image/heic",
                                      "image/tiff", NULL };

static const Kind kinds[] = {
  { "Browser", web }, { "Email", mail }, { "Calendar", cal }, { "Music", music }, { "Video", video },
  { "Photos", photos },
};

/* The app picked becomes the default for the kind's types it opens, as GNOME Settings does it (its
 * extra_type_filter): a music player that plays no FLAC is not made FLAC's. The browser is the exception: it is
 * also made the handler of about: and of the schemes nothing else handles, as GNOME's is. */
static void
default_picked (GtkWidget *row, guint i, gpointer kind)
{
  const Kind *k = kind;
  GPtrArray *apps = g_object_get_data (G_OBJECT (row), "ns-apps");
  GAppInfo *app = apps->pdata[i];
  const char *const *supported = g_app_info_get_supported_types (app);
  for (guint t = 0; k->types[t]; t++)
    if (k->types == web || (supported && g_strv_contains (supported, k->types[t])))
      g_app_info_set_as_default_for_type (app, k->types[t], NULL);
}

/* the default for the kind: its first type's, or the first of the others that has one */
static GAppInfo *
default_for_kind (const Kind *k)
{
  for (guint t = 0; k->types[t]; t++) {
    GAppInfo *def = g_app_info_get_default_for_type (k->types[t], FALSE);
    if (def)
      return def;
  }
  return NULL;
}

void
ns_page_default_apps (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Default apps", &c);
  GtkWidget *g = ns_group (c, NULL);
  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++) {
    g_autoptr (GAppInfo) def = default_for_kind (&kinds[k]);
    GPtrArray *apps = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GPtrArray) labels = g_ptr_array_new ();
    g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
    guint sel = G_MAXUINT;
    /* the apps that open any of the kind's types (a player of MP3s alone is a music player too), once each */
    for (guint t = 0; kinds[k].types[t]; t++) {
      GList *all = g_app_info_get_all_for_type (kinds[k].types[t]);
      for (GList *l = all; l; l = l->next) {
        const char *id = g_app_info_get_id (l->data);
        if (!g_app_info_should_show (l->data) || !id || g_hash_table_contains (seen, id))
          continue;
        if (def && g_app_info_equal (def, l->data))
          sel = apps->len;
        g_ptr_array_add (labels, (gpointer) g_app_info_get_display_name (l->data));
        g_ptr_array_add (apps, g_object_ref (l->data));
        /* the id stays valid: the app is in `apps` */
        g_hash_table_add (seen, (gpointer) g_app_info_get_id (apps->pdata[apps->len - 1]));
      }
      g_list_free_full (all, g_object_unref);
    }
    g_ptr_array_add (labels, NULL);
    GtkWidget *r;
    if (apps->len) {
      r = ns_row_pick (g, kinds[k].title, (const char *const *) labels->pdata, sel, default_picked,
                       (gpointer) &kinds[k]);
      if (sel == G_MAXUINT)
        ns_row_set_subtitle (r, "None chosen");
    } else {
      r = ns_row_info (g, kinds[k].title, "No app installed");
    }
    g_object_set_data_full (G_OBJECT (r), "ns-apps", apps, (GDestroyNotify) g_ptr_array_unref);
  }
  ns_push (page);
}
