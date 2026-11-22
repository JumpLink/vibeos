/* Security and privacy, Location and Accessibility. GSettings for most of it; the apps that may use the location
 * from the portal's permission store, asked in the background. (Accounts are ns-accounts.c's.) */
#include "ns-caps.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <gio/gdesktopappinfo.h>

static GtkWidget *
inverted (GtkWidget *g, const char *title, const char *sub, GSettings *s, const char *key)
{
  GtkWidget *r = ns_row_switch (g, title, sub, NULL, NULL);
  g_settings_bind (s, key, ns_row_get_switch (r), "active", G_SETTINGS_BIND_INVERT_BOOLEAN);
  return r;
}

static void
int_picked (GtkWidget *row, guint i, gpointer s)
{
  const int *values = g_object_get_data (G_OBJECT (row), "ns-values");
  const char *key = g_object_get_data (G_OBJECT (row), "ns-key");
  g_autoptr (GVariant) v = g_settings_get_value (s, key);
  if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32))
    g_settings_set_uint (s, key, values[i]);
  else
    g_settings_set_int (s, key, values[i]);
}

/* an integer key from a fixed list */
static GtkWidget *
int_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const int *values,
         const char *const *labels)
{
  g_autoptr (GVariant) v = g_settings_get_value (s, key);
  int cur = g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32) ? (int) g_variant_get_uint32 (v) : g_variant_get_int32 (v);
  guint sel = 0;
  for (guint i = 0; labels[i]; i++)
    if (values[i] == cur)
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, labels, sel, int_picked, s);
  g_object_set_data (G_OBJECT (r), "ns-values", (gpointer) values);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
  return r;
}

/* ---- Security and privacy ---- */

static const int ages[] = { -1, 1, 7, 30 };
static const char *const age_labels[] = { "Forever", "1 day", "7 days", "30 days", NULL };
static const int cleans[] = { 1, 7, 14, 30 };
static const char *const clean_labels[] = { "1 day", "7 days", "14 days", "30 days", NULL };

void
ns_page_privacy (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Security and privacy", &c);
  GtkWidget *g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, "system-lock-screen-symbolic", "Lock screen", "Screen lock type, notifications");
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  if (ns_settings_exists ("org.gnome.system.location")) {
    r = ns_row (g, "find-location-symbolic", "Location", "Location services, app access");
    ns_row_on_activate (r, ns_page_location, NULL);
  }
  r = ns_row (g, "system-users-symbolic", "Your account", "Name, password");
  ns_row_on_activate (r, ns_page_users, NULL);

  GSettings *p = ns_page_settings (page, "org.gnome.desktop.privacy");
  if (!p) {
    ns_push (page);
    return;
  }
  g = ns_group (c, "Permissions");
  if (ns_has_key (p, "disable-camera"))
    inverted (g, "Camera", "Apps that ask through the system may use it", p, "disable-camera");
  if (ns_has_key (p, "disable-microphone"))
    inverted (g, "Microphone", "Apps that ask through the system may use it", p, "disable-microphone");

  g = ns_group (c, "History");
  ns_row_switch (g, "Remember recent files", "Apps show what you opened lately", p, "remember-recent-files");
  r = int_row (g, "Keep the list for", p, "recent-files-max-age", ages, age_labels);
  g_settings_bind (p, "remember-recent-files", r, "sensitive", G_SETTINGS_BIND_GET);

  g = ns_group (c, "Clean up");
  ns_row_switch (g, "Empty the trash", "Delete old files from it automatically", p, "remove-old-trash-files");
  ns_row_switch (g, "Delete temporary files", "Old ones, automatically", p, "remove-old-temp-files");
  int_row (g, "Older than", p, "old-files-age", cleans, clean_labels);

  /* no Diagnostics: no crash reporter is installed, and only GNOME Software and Shell read the usage keys */
  ns_push (page);
}

/* ---- Location ---- */

static const char *const accuracy[] = { "country", "city", "neighborhood", "street", "exact", NULL };
static const char *const accuracy_labels[] = { "Country", "City", "Neighbourhood", "Street", "Exact", NULL };

static GDBusConnection *
session_bus (void)
{
  return g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
}

static void
app_access (GtkWidget *sw, GParamSpec *ps, gpointer data)
{
  const char *app = g_object_get_data (G_OBJECT (sw), "ns-app");
  g_autoptr (GDBusConnection) bus = session_bus ();
  const char *perm[] = { ns_switch_get_active (sw) ? "EXACT" : "NONE", "0", NULL };
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.impl.portal.PermissionStore",
                            "/org/freedesktop/impl/portal/PermissionStore",
                            "org.freedesktop.impl.portal.PermissionStore", "SetPermission",
                            g_variant_new ("(sbss^as)", "location", TRUE, "location", app, perm), NULL,
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
apps_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) perms = NULL;
  guint n = 0;
  if (v) {
    g_autoptr (GVariant) data = NULL;
    g_variant_get (v, "(@a{sas}v)", &perms, &data);
    GVariantIter it;
    const char *app;
    g_autofree const char **p = NULL;
    g_variant_iter_init (&it, perms);
    while (g_variant_iter_next (&it, "{&s^a&s}", &app, &p)) {
      g_autofree char *file = g_strconcat (app, ".desktop", NULL);
      g_autoptr (GDesktopAppInfo) info = *app ? g_desktop_app_info_new (file) : NULL;
      GtkWidget *r = info ? ns_row_app (group, G_APP_INFO (info), NULL) : ns_row (group, NULL, app, NULL);
      GtkWidget *sw = ns_row_add_switch (r, FALSE);
      ns_switch_set_active (sw, p && p[0] && g_strcmp0 (p[0], "NONE"));
      g_object_set_data_full (G_OBJECT (sw), "ns-app", g_strdup (app), g_free);
      g_signal_connect (sw, "notify::active", G_CALLBACK (app_access), NULL);
      g_clear_pointer (&p, g_free);
      n++;
    }
  }
  ns_group_set_visible (group, n > 0);
  gtk_widget_set_visible (g_object_get_data (group, "ns-none"), n == 0);
  g_object_unref (group);
}

void
ns_page_location (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Location", &c);
  GSettings *loc = ns_page_settings (page, "org.gnome.system.location");
  if (!loc)
    return;
  GtkWidget *g = ns_group (c, NULL);
  g_autofree char *where = g_strdup_printf ("Apps may ask where the %s is", ns_device_noun ());
  ns_row_switch (g, "Use location", where, loc, "enabled");
  if (ns_has_key (loc, "max-accuracy-level")) {
    GtkWidget *r = ns_row_choice (g, "Accuracy", loc, "max-accuracy-level", accuracy, accuracy_labels);
    g_settings_bind (loc, "enabled", r, "sensitive", G_SETTINGS_BIND_GET);
  }
  g = ns_group (c, "App access");
  ns_group_set_visible (g, FALSE);
  GtkWidget *none = ns_note (c, "No app has asked for the location yet.");
  gtk_widget_set_visible (none, FALSE);
  g_object_set_data (G_OBJECT (g), "ns-none", none);
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (bus)
    g_dbus_connection_call (bus, "org.freedesktop.impl.portal.PermissionStore",
                            "/org/freedesktop/impl/portal/PermissionStore",
                            "org.freedesktop.impl.portal.PermissionStore", "Lookup",
                            g_variant_new ("(ss)", "location", "location"), G_VARIANT_TYPE ("(a{sas}v)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, apps_got, g_object_ref (g));
  ns_push (page);
}

/* ---- Accessibility ---- */

static const int cursors[] = { 24, 32, 48, 64, 96 };
static const char *const cursor_labels[] = { "Default", "Medium", "Large", "Larger", "Largest", NULL };

void
ns_page_accessibility (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Accessibility", &c);
  GSettings *a11y = ns_page_settings (page, "org.gnome.desktop.a11y.interface");
  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  GSettings *apps = ns_page_settings (page, "org.gnome.desktop.a11y.applications");
  GSettings *keys = ns_page_settings (page, "org.gnome.desktop.a11y.keyboard");

  /* No "High contrast in bright light" (Phosh's automatic-high-contrast): nothing switches it by the light now */
  GtkWidget *g = ns_group (c, "Vision");
  if (ns_has_key (a11y, "high-contrast"))
    ns_row_switch (g, "High contrast", "Stronger colours and outlines", a11y, "high-contrast");
  if (ns_has_key (iface, "enable-animations"))
    inverted (g, "Reduce animations", "Less movement on screen", iface, "enable-animations");
  GtkWidget *r = ns_row (g, NULL, "Display size and text", "Font size, display size, bold text");
  ns_row_on_activate (r, ns_page_display_size, NULL);
  if (ns_has_key (iface, "cursor-size"))
    int_row (g, "Pointer size", iface, "cursor-size", cursors, cursor_labels);
  /* the screen reader is Orca: only when it is installed */
  g_autofree char *orca = g_find_program_in_path ("orca");
  if (orca && ns_has_key (apps, "screen-reader-enabled"))
    ns_row_switch (g, "Screen reader", "Orca reads out what is on screen", apps, "screen-reader-enabled");

  g = ns_group (c, "Interaction");
  /* GNOME Shell's keyboard opens by itself for the touchscreen; the key keeps it on for other input too */
  if (ns_has_key (apps, "screen-keyboard-enabled"))
    ns_row_switch (g, "Show on-screen keyboard", "Also while a hardware keyboard or a mouse is used", apps,
                   "screen-keyboard-enabled");

  /* the hardware keyboard's helpers: mutter does them (its a11y keyboard settings, read live) */
  if (ns_has_key (keys, "stickykeys-enable")) {
    g = ns_group (c, "Hardware keyboard");
    ns_row_switch (g, "Sticky keys", "Shift, Ctrl and Alt stay pressed until the next key", keys,
                   "stickykeys-enable");
    ns_row_switch (g, "Slow keys", "A key counts only when held a moment", keys, "slowkeys-enable");
    ns_row_switch (g, "Bounce keys", "A key pressed again at once counts once", keys, "bouncekeys-enable");
  }
  ns_push (page);
}
