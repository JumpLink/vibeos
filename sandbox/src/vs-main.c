/* vibeos-sandbox: runs the apps installed from Software in a sandbox, and keeps what makes them start there.
 *
 *   vibeos-sandbox run [--file-forwarding] APP-ID -- COMMAND…   run COMMAND as the app, in its sandbox
 *   vibeos-sandbox add [--origin=store|file] PACKAGE…           sandbox the apps of installed packages
 *   vibeos-sandbox remove PACKAGE…                              stop sandboxing them (their files stay)
 *   vibeos-sandbox sync [--wait] [--history-client=TEXT]         follow what was installed, updated, uninstalled
 *                                                               (an uninstalled app's data goes with it)
 *   vibeos-sandbox list                                         the sandboxed packages and their apps
 *   vibeos-sandbox permissions APP-ID                           what the app may do, and asks for
 *   vibeos-sandbox set APP-ID PERMISSION allow|ask|deny         change it (taking one away stops the app)
 *   vibeos-sandbox clear-cache PACKAGE                          delete what its apps keep in their cache
 *   vibeos-sandbox ps                                           the sandboxes running
 *   vibeos-sandbox kill APP-ID                                  stop them */
#include "vs-bwrap.h"
#include "vs-perms.h"
#include "vs-registry.h"
#include "vs-run.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

/* what App Installer's PackageKit transactions have in their command line */
#define INSTALLER_CLIENT "vibeos-installer"

static int
usage (void)
{
  g_printerr ("Usage:\n"
              "  vibeos-sandbox run [--file-forwarding] APP-ID -- COMMAND…\n"
              "  vibeos-sandbox add [--origin=store|file] PACKAGE…\n"
              "  vibeos-sandbox remove PACKAGE…\n"
              "  vibeos-sandbox sync [--wait] [--history-client=TEXT]\n"
              "  vibeos-sandbox list\n"
              "  vibeos-sandbox permissions APP-ID\n"
              "  vibeos-sandbox set APP-ID PERMISSION allow|ask|deny\n"
              "  vibeos-sandbox clear-cache PACKAGE\n"
              "  vibeos-sandbox ps\n"
              "  vibeos-sandbox kill APP-ID\n");
  return 2;
}

static GDBusConnection *
session_bus (void)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus)
    g_printerr ("vibeos-sandbox: %s\n", error->message);
  return bus;
}

/* tells the session bus and systemd what changed on the user's side */
static void
apply_changes (const VsChanges *changes)
{
  g_autoptr (GDBusConnection) bus = NULL;
  if (!changes->services && !changes->units)
    return;
  if (!(bus = session_bus ()))
    return;
  g_autoptr (GError) error = NULL;
  if (changes->services) {
    g_autoptr (GVariant) r = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                          "org.freedesktop.DBus", "ReloadConfig", NULL, NULL,
                                                          G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    if (!r)
      g_printerr ("vibeos-sandbox: the session bus did not reload: %s\n", error->message);
    g_clear_error (&error);
  }
  if (changes->units) {
    g_autoptr (GVariant) r = g_dbus_connection_call_sync (bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                                          "org.freedesktop.systemd1.Manager", "Reload", NULL, NULL,
                                                          G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);
    if (!r)
      g_printerr ("vibeos-sandbox: systemd did not reload: %s\n", error->message);
  }
}

/* the autostart entries of a package run only when every app of it may run in the background */
static gboolean
may_autostart (VsStore *store, VsPackage *p)
{
  if (!store || !p->apps->len)
    return FALSE;
  for (guint i = 0; i < p->apps->len; i++)
    if (vs_perm_get (store, ((VsApp *) p->apps->pdata[i])->app_id, VS_PERM_BACKGROUND) != VS_GRANT_ALLOW)
      return FALSE;
  return TRUE;
}

/* what an app does not ask for (its desktop file's X-VibeOS-Permissions) is not allowed in the store either, so
 * that the portals refuse it without asking and WirePlumber hides what it would need */
static void
deny_unrequested (VsStore *store, VsPackage *p)
{
  for (guint i = 0; store && i < p->apps->len; i++) {
    VsApp *app = p->apps->pdata[i];
    guint requests = vs_app_requests (app);
    for (int perm = 0; perm < VS_N_PERMS; perm++)
      if (!(requests & (1u << perm)) && vs_perm_get (store, app->app_id, perm) != VS_GRANT_DENY)
        vs_perm_set (store, app->app_id, perm, VS_GRANT_DENY, NULL);
  }
}

/* a new app is asked before it runs in the background */
static void
background_asks (VsStore *store, VsPackage *p)
{
  for (guint i = 0; store && i < p->apps->len; i++) {
    const char *app_id = ((VsApp *) p->apps->pdata[i])->app_id;
    g_auto (GStrv) value = store->get (store, "background", "background", app_id, NULL);
    if (!value)
      vs_perm_set (store, app_id, VS_PERM_BACKGROUND, VS_GRANT_ASK, NULL);
  }
}

static gboolean
register_package (const VsDirs *dirs, GHashTable *installed, VsStore *store, const char *name, const char *origin,
                  VsChanges *changes, gboolean quiet)
{
  g_autoptr (GError) error = NULL;
  if (vs_package_is_system (name)) {
    g_printerr ("vibeos-sandbox: %s is part of the system\n", name);
    return FALSE;
  }
  g_autoptr (VsPackage) p = vs_package_scan (dirs, g_hash_table_lookup (installed, name), &error);
  if (!p) {
    if (error) {
      g_printerr ("vibeos-sandbox: %s: %s\n", name, error->message);
      return FALSE;
    }
    if (!quiet)
      g_print ("%s: no app to sandbox\n", name);
    return TRUE;
  }
  g_autoptr (VsPackage) before = vs_registry_load (dirs, name);
  p->origin = g_strdup (before && before->origin ? before->origin : origin);
  if (!before)
    background_asks (store, p);
  deny_unrequested (store, p);
  if (!vs_registry_write (dirs, p, may_autostart (store, p), changes, &error)) {
    g_printerr ("vibeos-sandbox: %s: %s\n", name, error->message);
    return FALSE;
  }
  for (guint i = 0; !quiet && i < p->apps->len; i++)
    g_print ("%s: %s\n", name, ((VsApp *) p->apps->pdata[i])->app_id);
  return TRUE;
}

/* one change to the records at a time (App Installer's add, the syncs dpkg starts): held until the process ends */
static int
lock_registry (const VsDirs *dirs)
{
  g_autofree char *dir = g_build_filename (dirs->data_home, "vibeos-sandbox", NULL);
  g_autofree char *path = g_build_filename (dir, ".lock", NULL);
  g_mkdir_with_parents (dir, 0700);
  int fd = open (path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0 || flock (fd, LOCK_EX) < 0)
    g_printerr ("vibeos-sandbox: %s: %s\n", path, g_strerror (errno));
  return fd;
}

static int
cmd_add (int argc, char **argv)
{
  const char *origin = "store";
  int first = 0;
  if (argc > 0 && g_str_has_prefix (argv[0], "--origin=")) {
    origin = argv[0] + 9;
    first = 1;
  }
  if (first >= argc)
    return usage ();
  g_autoptr (GError) error = NULL;
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  lock_registry (dirs);
  g_autoptr (GHashTable) installed = vs_dpkg_installed (dirs, &error);
  if (!installed) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 1;
  }
  g_autoptr (GDBusConnection) bus = session_bus ();
  g_autoptr (VsStore) store = bus ? vs_store_new_dbus (bus) : NULL;
  VsChanges changes = { 0 };
  int status = 0;
  for (int i = first; i < argc; i++)
    if (!register_package (dirs, installed, store, argv[i], origin, &changes, FALSE))
      status = 1;
  apply_changes (&changes);
  return status;
}

static int
cmd_remove (int argc, char **argv)
{
  if (argc < 1)
    return usage ();
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  lock_registry (dirs);
  VsChanges changes = { 0 };
  int status = 0;
  for (int i = 0; i < argc; i++) {
    g_autoptr (GError) error = NULL;
    g_autoptr (VsPackage) p = vs_registry_load (dirs, argv[i]);
    for (guint j = 0; p && j < p->apps->len; j++)
      vs_kill_app (((VsApp *) p->apps->pdata[j])->app_id);
    if (!vs_registry_remove (dirs, argv[i], &changes, &error)) {
      g_printerr ("vibeos-sandbox: %s\n", error->message);
      status = 1;
    }
  }
  apply_changes (&changes);
  return status;
}

/* a sandboxed package's cache (.cache in its apps' home): what Settings' "Clear cache" does */
static int
cmd_clear_cache (int argc, char **argv)
{
  if (argc != 1)
    return usage ();
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  g_autoptr (VsPackage) p = vs_registry_load (dirs, argv[0]);
  if (!p) {
    g_printerr ("vibeos-sandbox: %s is not sandboxed\n", argv[0]);
    return 1;
  }
  g_autoptr (GError) error = NULL;
  if (!vs_app_cache_clear (dirs, p->name, &error)) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 1;
  }
  return 0;
}

/* the Exec line for a background portal's autostart entry (flatpak run [--command=CMD] APP [ARG...]): through
 * vibeos-sandbox, with the app's own command when it names none; NULL when there is none */
static char *
portal_exec (const VsDirs *dirs, VsPackage *p, const char *app_id, char **argv)
{
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (out, g_shell_quote (dirs->launcher));
  g_ptr_array_add (out, g_strdup ("run"));
  g_ptr_array_add (out, g_shell_quote (app_id));
  g_ptr_array_add (out, g_strdup ("--"));
  guint i = 2;
  if (g_str_has_prefix (argv[i], "--command=")) {
    g_ptr_array_add (out, g_shell_quote (argv[i] + strlen ("--command=")));
    i++;
  } else {
    /* no command: the app's own, from its desktop file */
    g_auto (GStrv) app_argv = NULL;
    for (guint j = 0; j < p->apps->len && !app_argv; j++) {
      VsApp *app = p->apps->pdata[j];
      g_autoptr (GKeyFile) src = g_key_file_new ();
      g_autofree char *app_exec = NULL;
      if (strcmp (app->app_id, app_id) || !app->source
          || !g_key_file_load_from_file (src, app->source, G_KEY_FILE_NONE, NULL)
          || !(app_exec = g_key_file_get_string (src, G_KEY_FILE_DESKTOP_GROUP, "Exec", NULL)))
        continue;
      g_shell_parse_argv (app_exec, NULL, &app_argv, NULL);
    }
    if (!app_argv)
      return NULL;
    for (guint j = 0; app_argv[j]; j++)
      if (app_argv[j][0] != '%')
        g_ptr_array_add (out, g_shell_quote (app_argv[j]));
  }
  /* argv[i] is the app id; the arguments follow it */
  for (i++; i < g_strv_length (argv); i++)
    g_ptr_array_add (out, g_shell_quote (argv[i]));
  g_ptr_array_add (out, NULL);
  return g_strjoinv (" ", (char **) out->pdata);
}

/* The background portal writes an autostart entry for an app that asks to start with the session, as for a Flatpak
 * app (X-Flatpak=APP, Exec=flatpak run …). A sandboxed app's starts it through vibeos-sandbox instead, and stays
 * hidden while the app may not run in the background (taken away after it asked). An entry whose app is gone goes
 * with it. */
static void
fix_portal_autostart (const VsDirs *dirs, VsStore *store)
{
  g_autofree char *dir = g_build_filename (dirs->config_home, "autostart", NULL);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d))) {
    if (!g_str_has_suffix (name, ".desktop"))
      continue;
    g_autofree char *path = g_build_filename (dir, name, NULL);
    g_autoptr (GKeyFile) k = g_key_file_new ();
    if (!g_key_file_load_from_file (k, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
      continue;
    g_autofree char *app_id = g_key_file_get_string (k, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL);
    g_autofree char *exec = g_key_file_get_string (k, G_KEY_FILE_DESKTOP_GROUP, "Exec", NULL);
    if (!app_id)
      continue;
    gboolean ours = g_key_file_has_key (k, G_KEY_FILE_DESKTOP_GROUP, VS_MARK_KEY, NULL);
    g_auto (GStrv) argv = NULL;
    gboolean from_portal = exec && g_shell_parse_argv (exec, NULL, &argv, NULL) && g_strv_length (argv) >= 3
                           && !strcmp (argv[0], "flatpak") && !strcmp (argv[1], "run");
    if (!ours && !from_portal)
      continue;
    g_autoptr (VsPackage) p = vs_registry_find_app (dirs, app_id);
    if (!p) {
      if (ours)
        g_unlink (path);
      continue;
    }
    gboolean changed = FALSE;
    if (from_portal) {
      g_autofree char *line = portal_exec (dirs, p, app_id, argv);
      if (!line)
        continue;
      g_key_file_set_string (k, G_KEY_FILE_DESKTOP_GROUP, "Exec", line);
      g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "TryExec", NULL);
      g_key_file_set_string (k, G_KEY_FILE_DESKTOP_GROUP, VS_MARK_KEY, app_id);
      changed = TRUE;
    }
    gboolean hidden = !store || vs_perm_get (store, app_id, VS_PERM_BACKGROUND) != VS_GRANT_ALLOW;
    if (g_key_file_get_boolean (k, G_KEY_FILE_DESKTOP_GROUP, "Hidden", NULL) != hidden) {
      if (hidden)
        g_key_file_set_boolean (k, G_KEY_FILE_DESKTOP_GROUP, "Hidden", TRUE);
      else
        g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "Hidden", NULL);
      changed = TRUE;
    }
    if (changed)
      g_key_file_save_to_file (k, path, NULL);
  }
}

/* one version of dpkg's status file from the next: dpkg writes a new one and renames it over the old */
typedef struct {
  dev_t           dev;
  ino_t           ino;
  off_t           size;
  struct timespec mtime;
} StatusStamp;

static StatusStamp
status_stamp (const VsDirs *dirs)
{
  StatusStamp stamp = { 0 };
  g_autofree char *path = g_build_filename (dirs->dpkg_admin, "status", NULL);
  struct stat st;
  if (stat (path, &st) == 0) {
    stamp.dev = st.st_dev;
    stamp.ino = st.st_ino;
    stamp.size = st.st_size;
    stamp.mtime = st.st_mtim;
  }
  return stamp;
}

static gboolean
stamp_same (const StatusStamp *a, const StatusStamp *b)
{
  return a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mtime.tv_sec == b->mtime.tv_sec &&
         a->mtime.tv_nsec == b->mtime.tv_nsec;
}

/* until dpkg's status has stayed the same for two seconds (it writes it as it goes), while *waited_ms < limit */
static void
settle (const VsDirs *dirs, guint *waited_ms, guint limit)
{
  StatusStamp last = status_stamp (dirs);
  for (guint quiet = 0; quiet < 2000 && *waited_ms < limit; *waited_ms += 250) {
    g_usleep (250 * 1000);
    StatusStamp now = status_stamp (dirs);
    quiet = stamp_same (&now, &last) ? quiet + 250 : 0;
    last = now;
  }
}

/* a package that was uninstalled: its apps stop, and what they kept goes with it as on Android: their home (their
 * files and settings) and their permissions */
static gboolean
forget_package (const VsDirs *dirs, VsStore *store, VsPackage *p)
{
  gboolean ok = TRUE;
  for (guint j = 0; j < p->apps->len; j++) {
    const char *app_id = ((VsApp *) p->apps->pdata[j])->app_id;
    g_autoptr (GError) e = NULL;
    vs_kill_app (app_id);
    if (store && !vs_perm_forget (store, app_id, &e)) {
      g_printerr ("vibeos-sandbox: %s's permissions: %s\n", app_id, e->message);
      ok = FALSE;
    }
  }
  g_autoptr (GError) e = NULL;
  if (!vs_app_data_remove (dirs, p->name, &e)) {
    g_printerr ("vibeos-sandbox: %s's data: %s\n", p->name, e->message);
    ok = FALSE;
  }
  return ok;
}

/* one look at dpkg's status: the packages that came and went since the last; `busy` gets the ones dpkg is half way
 * through (left as they are) */
static int
sync_once (const VsDirs *dirs, const char *client, gboolean import, GPtrArray *busy)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) states = vs_dpkg_states (dirs, NULL);
  if (!states) {
    g_printerr ("vibeos-sandbox: dpkg's status cannot be read\n");
    return 1;
  }
  g_autoptr (GHashTable) installed = vs_dpkg_installed (dirs, &error);
  if (!installed) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 1;
  }
  g_autoptr (GDBusConnection) bus = session_bus ();
  g_autoptr (VsStore) store = bus ? vs_store_new_dbus (bus) : NULL;
  VsChanges changes = { 0 };
  int status = 0;

  /* the packages installed from Software before there was a sandbox: once, or when asked */
  g_autofree char *stamp = g_build_filename (dirs->data_home, "vibeos-sandbox", "history-imported", NULL);
  if (import && (client || !g_file_test (stamp, G_FILE_TEST_EXISTS))) {
    g_autoptr (GError) history_error = NULL;
    g_auto (GStrv) names = vs_history_installed (dirs, client ? client : INSTALLER_CLIENT, &history_error);
    if (!names) {
      g_printerr ("vibeos-sandbox: PackageKit's history: %s\n", history_error->message);
    } else {
      for (guint i = 0; names[i]; i++) {
        g_autoptr (VsPackage) known = vs_registry_load (dirs, names[i]);
        if (known || vs_package_is_system (names[i]) || !g_hash_table_contains (installed, names[i]))
          continue;
        register_package (dirs, installed, store, names[i], "history", &changes, FALSE);
      }
      if (!client) {
        g_autofree char *parent = g_path_get_dirname (stamp);
        g_mkdir_with_parents (parent, 0700);
        g_file_set_contents (stamp, "", 0, NULL);
      }
    }
  }

  /* the ones updated (their files again), uninstalled (their files gone, and their data with them) or changed in
   * between */
  g_autoptr (GPtrArray) names = vs_registry_list (dirs);
  for (guint i = 0; i < names->len; i++) {
    const char *name = names->pdata[i];
    g_autoptr (GError) e = NULL;
    VsDpkgState state = vs_dpkg_state (states, name);
    VsDpkgPackage *now = g_hash_table_lookup (installed, name);
    if (state == VS_DPKG_CHANGING || (state == VS_DPKG_INSTALLED && !now)) {
      g_ptr_array_add (busy, g_strdup (name));
      continue;
    }
    if (state == VS_DPKG_GONE) {
      g_autoptr (VsPackage) before = vs_registry_load (dirs, name);
      if (!vs_registry_remove (dirs, name, &changes, &e)) {
        g_printerr ("vibeos-sandbox: %s: %s\n", name, e->message);
        status = 1;
        continue;
      }
      if (before && !forget_package (dirs, store, before))
        status = 1;
      g_print ("%s: gone, and its data\n", name);
      continue;
    }
    g_autoptr (VsPackage) p = vs_package_scan (dirs, now, &e);
    if (!p && e) {
      g_printerr ("vibeos-sandbox: %s: %s\n", name, e->message);
      status = 1;
      continue;
    }
    if (!p) {
      /* still installed, with no app any more: no sandbox to keep, and its data stays */
      if (!vs_registry_remove (dirs, name, &changes, &e)) {
        g_printerr ("vibeos-sandbox: %s: %s\n", name, e->message);
        status = 1;
      } else {
        g_print ("%s: nothing to sandbox any more\n", name);
      }
      continue;
    }
    if (!register_package (dirs, installed, store, name, "store", &changes, TRUE))
      status = 1;
  }
  fix_portal_autostart (dirs, store);
  apply_changes (&changes);
  return status;
}

static int
cmd_sync (int argc, char **argv)
{
  const char *client = NULL;
  gboolean wait = FALSE;
  for (int i = 0; i < argc; i++) {
    if (g_str_has_prefix (argv[i], "--history-client="))
      client = argv[i] + 17;
    else if (!strcmp (argv[i], "--wait"))
      wait = TRUE;
    else
      return usage ();
  }
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();

  /* dpkg writes its status as it goes (a package being removed is still "installed", then half gone, then gone, and
   * the triggers run after), and each write starts a sync (--wait); but systemd starts no second one while one runs,
   * so a write meanwhile would be seen only at the next change, minutes or days later. With --wait it lets dpkg
   * settle first, and goes again while dpkg wrote during a pass or left a package half done, for up to ten minutes.
   * Without it (as the session starts) a package dpkg is half way through stays as it is until the next sync. */
  const guint limit = 600 * 1000;
  guint waited = 0;
  g_autoptr (GPtrArray) busy = NULL;
  int status = 0;
  for (guint pass = 0;; pass++) {
    if (wait)
      settle (dirs, &waited, limit);
    StatusStamp before = status_stamp (dirs);
    g_clear_pointer (&busy, g_ptr_array_unref);
    busy = g_ptr_array_new_with_free_func (g_free);
    int lock = lock_registry (dirs);
    status = sync_once (dirs, client, pass == 0, busy);
    if (lock >= 0)
      close (lock);
    StatusStamp after = status_stamp (dirs);
    if (!wait || waited >= limit || (busy->len == 0 && stamp_same (&before, &after)))
      break;
    if (stamp_same (&before, &after)) {
      /* a package half done and dpkg quiet: a long maintainer script */
      g_usleep (2 * G_USEC_PER_SEC);
      waited += 2000;
    }
  }
  for (guint i = 0; i < busy->len; i++)
    g_print ("%s: dpkg is not done with it\n", (const char *) busy->pdata[i]);
  return status;
}

static int
cmd_list (void)
{
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  g_autoptr (GPtrArray) names = vs_registry_list (dirs);
  for (guint i = 0; i < names->len; i++) {
    g_autoptr (VsPackage) p = vs_registry_load (dirs, names->pdata[i]);
    if (!p)
      continue;
    for (guint j = 0; j < p->apps->len; j++) {
      VsApp *app = p->apps->pdata[j];
      g_print ("%s\t%s\t%s\t%s\t%s\n", p->name, p->version ? p->version : "", p->origin ? p->origin : "",
               app->app_id, app->desktop_id);
    }
    if (!p->apps->len)
      g_print ("%s\t%s\t%s\t-\t-\n", p->name, p->version ? p->version : "", p->origin ? p->origin : "");
  }
  return 0;
}

/* what a registered app asks for (all of them for one that is not registered) */
static guint
app_requests (const char *app_id)
{
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  g_autoptr (VsPackage) p = vs_registry_find_app (dirs, app_id);
  for (guint i = 0; p && i < p->apps->len; i++)
    if (!strcmp (((VsApp *) p->apps->pdata[i])->app_id, app_id))
      return vs_app_requests (p->apps->pdata[i]);
  return VS_REQUESTS_ALL;
}

static int
cmd_permissions (int argc, char **argv)
{
  if (argc != 1)
    return usage ();
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (!bus)
    return 1;
  g_autoptr (VsStore) store = vs_store_new_dbus (bus);
  guint requests = app_requests (argv[0]);
  for (int i = 0; i < VS_N_PERMS; i++)
    g_print ("%s\t%s\t%s\n", vs_perm_name (i),
             vs_grant_name ((requests & (1u << i)) ? vs_perm_get (store, argv[0], i) : VS_GRANT_DENY),
             (requests & (1u << i)) ? "requested" : "not-requested");
  return 0;
}

static int
grant_rank (VsGrant g)
{
  return g == VS_GRANT_ALLOW ? 2 : g == VS_GRANT_ASK ? 1 : 0;
}

static int
cmd_set (int argc, char **argv)
{
  if (argc != 3)
    return usage ();
  int perm = vs_perm_from_name (argv[1]);
  int grant = vs_grant_from_name (argv[2]);
  if (perm < 0 || grant < 0)
    return usage ();
  g_autoptr (GDBusConnection) bus = session_bus ();
  if (!bus)
    return 1;
  g_autoptr (GError) error = NULL;
  if (grant != VS_GRANT_DENY && !(app_requests (argv[0]) & (1u << perm))) {
    g_printerr ("vibeos-sandbox: %s does not ask for the %s permission\n", argv[0], argv[1]);
    return 1;
  }
  g_autoptr (VsStore) store = vs_store_new_dbus (bus);
  VsGrant before = vs_perm_get (store, argv[0], perm);
  if (!vs_perm_set (store, argv[0], perm, grant, &error)) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 1;
  }
  /* the autostart entries follow the background permission */
  if (perm == VS_PERM_BACKGROUND) {
    g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
    g_autoptr (VsPackage) p = vs_registry_find_app (dirs, argv[0]);
    g_autoptr (GHashTable) installed = p ? vs_dpkg_installed (dirs, NULL) : NULL;
    g_autoptr (VsPackage) scanned = installed ? vs_package_scan (dirs, g_hash_table_lookup (installed, p->name), NULL)
                                              : NULL;
    if (scanned) {
      VsChanges changes = { 0 };
      vs_registry_write (dirs, scanned, may_autostart (store, scanned), &changes, NULL);
      apply_changes (&changes);
    }
    fix_portal_autostart (dirs, store);
  }
  /* taking a permission away stops the app: it does not keep what it already opened */
  if (grant_rank (grant) < grant_rank (before)) {
    int n = vs_kill_app (argv[0]);
    if (n)
      g_print ("%s: stopped\n", argv[0]);
  }
  return 0;
}

static int
cmd_ps (void)
{
  g_autoptr (GPtrArray) running = vs_instances (NULL);
  for (guint i = 0; i < running->len; i++) {
    VsInstance *inst = running->pdata[i];
    g_print ("%s\t%s\t%d\t%d\n", inst->instance_id, inst->app_id, inst->pid, inst->child_pid);
  }
  return 0;
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  if (argc < 2)
    return usage ();
  const char *cmd = argv[1];
  argc -= 2;
  argv += 2;

  if (!strcmp (cmd, "run")) {
    gboolean forward = FALSE;
    int i = 0;
    if (i < argc && !strcmp (argv[i], "--file-forwarding")) {
      forward = TRUE;
      i++;
    }
    if (i + 1 >= argc || strcmp (argv[i + 1], "--"))
      return usage ();
    return vs_run (argv[i], forward, argv + i + 2);
  }
  if (!strcmp (cmd, "add"))
    return cmd_add (argc, argv);
  if (!strcmp (cmd, "remove"))
    return cmd_remove (argc, argv);
  if (!strcmp (cmd, "sync"))
    return cmd_sync (argc, argv);
  if (!strcmp (cmd, "list"))
    return cmd_list ();
  if (!strcmp (cmd, "permissions") || !strcmp (cmd, "info"))
    return cmd_permissions (argc, argv);
  if (!strcmp (cmd, "set"))
    return cmd_set (argc, argv);
  if (!strcmp (cmd, "clear-cache"))
    return cmd_clear_cache (argc, argv);
  if (!strcmp (cmd, "ps"))
    return cmd_ps ();
  if (!strcmp (cmd, "kill")) {
    if (argc != 1)
      return usage ();
    g_print ("%d\n", vs_kill_app (argv[0]));
    return 0;
  }
  return usage ();
}
