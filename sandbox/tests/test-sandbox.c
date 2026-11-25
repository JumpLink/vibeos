/* vibeos-sandbox's parts that do not need a running system: the permissions and how they are stored, the app ids,
 * the Exec lines, bubblewrap's and xdg-dbus-proxy's arguments, /.flatpak-info, the files written for a package, and
 * PackageKit's history. */
#include "vs-bwrap.h"
#include "vs-exec.h"
#include "vs-perms.h"
#include "vs-registry.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <unistd.h>

/* ---- helpers ---- */

static gboolean
has (GPtrArray *a, const char *s)
{
  for (guint i = 0; i < a->len; i++)
    if (!g_strcmp0 (a->pdata[i], s))
      return TRUE;
  return FALSE;
}

/* `x y z` appear one after the other */
static gboolean
has3 (GPtrArray *a, const char *x, const char *y, const char *z)
{
  for (guint i = 0; i + 2 < a->len; i++)
    if (!g_strcmp0 (a->pdata[i], x) && !g_strcmp0 (a->pdata[i + 1], y) && !g_strcmp0 (a->pdata[i + 2], z))
      return TRUE;
  return FALSE;
}

static gboolean
has_prefix (GPtrArray *a, const char *prefix)
{
  for (guint i = 0; i < a->len; i++)
    if (a->pdata[i] && g_str_has_prefix (a->pdata[i], prefix))
      return TRUE;
  return FALSE;
}

static void
write_file (const char *root, const char *path, const char *text)
{
  g_autofree char *full = g_build_filename (root, path, NULL);
  g_autofree char *dir = g_path_get_dirname (full);
  g_mkdir_with_parents (dir, 0755);
  g_assert_true (g_file_set_contents (full, text, -1, NULL));
}

static char *
read_file (const char *path)
{
  char *text = NULL;
  g_file_get_contents (path, &text, NULL, NULL);
  return text;
}

/* ---- permissions ---- */

static void
test_perm_defaults (void)
{
  g_autoptr (VsStore) store = vs_store_new_memory ();
  const char *app = "org.example.Game";
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_NETWORK), ==, VS_GRANT_ALLOW);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_LOCATION), ==, VS_GRANT_ASK);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_CAMERA), ==, VS_GRANT_ASK);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_MICROPHONE), ==, VS_GRANT_ASK);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_CONTACTS), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_CALENDAR), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_PHONE), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_SMS), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_FILES), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_BACKGROUND), ==, VS_GRANT_ALLOW);
  /* no store at all: the defaults */
  g_assert_cmpint (vs_perm_get (NULL, app, VS_PERM_MICROPHONE), ==, VS_GRANT_ASK);

  for (int i = 0; i < VS_N_PERMS; i++)
    g_assert_cmpint (vs_perm_from_name (vs_perm_name (i)), ==, i);
  g_assert_cmpint (vs_perm_from_name ("modem"), ==, -1);
  g_assert_cmpint (vs_grant_from_name ("allow"), ==, VS_GRANT_ALLOW);
  g_assert_cmpint (vs_grant_from_name ("deny"), ==, VS_GRANT_DENY);
  g_assert_cmpint (vs_grant_from_name ("ask"), ==, VS_GRANT_ASK);
  g_assert_cmpint (vs_grant_from_name ("yes"), ==, -1);
}

static void
test_perm_store (void)
{
  g_autoptr (VsStore) store = vs_store_new_memory ();
  g_autoptr (GError) error = NULL;
  const char *app = "org.example.Map";

  /* the location portal's own entry */
  g_assert_true (vs_perm_set (store, app, VS_PERM_LOCATION, VS_GRANT_ALLOW, &error));
  g_auto (GStrv) loc = store->get (store, "location", "location", app, NULL);
  g_assert_cmpstr (loc[0], ==, "EXACT");
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_LOCATION), ==, VS_GRANT_ALLOW);
  g_assert_true (vs_perm_set (store, app, VS_PERM_LOCATION, VS_GRANT_DENY, &error));
  g_auto (GStrv) none = store->get (store, "location", "location", app, NULL);
  g_assert_cmpstr (none[0], ==, "NONE");
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_LOCATION), ==, VS_GRANT_DENY);
  /* asking again: no entry */
  g_assert_true (vs_perm_set (store, app, VS_PERM_LOCATION, VS_GRANT_ASK, &error));
  g_assert_null (store->get (store, "location", "location", app, NULL));
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_LOCATION), ==, VS_GRANT_ASK);
  /* what the portal writes when the user allows it in its dialog: an accuracy, then a time */
  const char *city[] = { "CITY", "1763400000", NULL };
  store->set (store, "location", "location", app, city, NULL);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_LOCATION), ==, VS_GRANT_ALLOW);

  /* the camera portal's */
  g_assert_true (vs_perm_set (store, app, VS_PERM_CAMERA, VS_GRANT_ALLOW, &error));
  g_auto (GStrv) cam = store->get (store, "devices", "camera", app, NULL);
  g_assert_cmpstr (cam[0], ==, "yes");
  g_assert_true (vs_perm_set (store, app, VS_PERM_CAMERA, VS_GRANT_ASK, &error));
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_CAMERA), ==, VS_GRANT_ASK);

  /* the microphone: yes, no, and ask again (the shell asks when the app first records) */
  g_assert_true (vs_perm_set (store, app, VS_PERM_MICROPHONE, VS_GRANT_ALLOW, &error));
  g_auto (GStrv) mic = store->get (store, VS_STORE_TABLE, "microphone", app, NULL);
  g_assert_cmpstr (mic[0], ==, "yes");
  g_assert_true (vs_perm_set (store, app, VS_PERM_MICROPHONE, VS_GRANT_ASK, &error));
  g_auto (GStrv) mic_ask = store->get (store, VS_STORE_TABLE, "microphone", app, NULL);
  g_assert_cmpstr (mic_ask[0], ==, "ask");
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_MICROPHONE), ==, VS_GRANT_ASK);
  g_assert_true (vs_perm_set (store, app, VS_PERM_MICROPHONE, VS_GRANT_DENY, &error));
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_MICROPHONE), ==, VS_GRANT_DENY);
  g_assert_true (vs_perm_set (store, app, VS_PERM_MICROPHONE, VS_GRANT_ALLOW, &error));

  /* the others in our table, and no question for them */
  g_assert_false (vs_perm_set (store, app, VS_PERM_CONTACTS, VS_GRANT_ASK, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_true (vs_perm_set (store, app, VS_PERM_NETWORK, VS_GRANT_DENY, &error));
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_NETWORK), ==, VS_GRANT_DENY);
  /* an "ask" where nothing asks means no */
  const char *ask[] = { "ask", NULL };
  store->set (store, VS_STORE_TABLE, "sms", app, ask, NULL);
  g_assert_cmpint (vs_perm_get (store, app, VS_PERM_SMS), ==, VS_GRANT_DENY);
  /* the background portal's */
  g_assert_true (vs_perm_set (store, app, VS_PERM_BACKGROUND, VS_GRANT_ASK, &error));
  g_auto (GStrv) bg = store->get (store, "background", "background", app, NULL);
  g_assert_cmpstr (bg[0], ==, "ask");
  /* another app is not touched */
  g_assert_cmpint (vs_perm_get (store, "org.example.Other", VS_PERM_MICROPHONE), ==, VS_GRANT_ASK);

  VsGrants grants;
  vs_grants_load (store, app, &grants);
  g_assert_true (vs_grants_allowed (&grants, VS_PERM_MICROPHONE));
  g_assert_false (vs_grants_allowed (&grants, VS_PERM_NETWORK));
  g_assert_false (vs_grants_allowed (&grants, VS_PERM_CAMERA));

  /* uninstalled: out of every table, the portals' too; the other app keeps its own */
  const char *yes[] = { "yes", NULL };
  store->set (store, "notifications", "notification", app, yes, NULL);
  store->set (store, "documents", "doc1", app, (const char *[]) { "read", "write", NULL }, NULL);
  store->set (store, "documents", "doc1", "org.example.Other", (const char *[]) { "read", NULL }, NULL);
  store->set (store, "documents", "doc2", app, (const char *[]) { "read", NULL }, NULL);
  vs_perm_set (store, "org.example.Other", VS_PERM_MICROPHONE, VS_GRANT_ALLOW, NULL);
  store->set (store, VS_ASK_TABLE, "microphone", app, ask, NULL);
  g_assert_true (vs_perm_forget (store, app, &error));
  g_assert_no_error (error);
  const char *tables[][2] = { { "location", "location" }, { "devices", "camera" }, { VS_STORE_TABLE, "microphone" },
                              { VS_ASK_TABLE, "microphone" }, { VS_STORE_TABLE, "network" }, { VS_STORE_TABLE, "sms" },
                              { "background", "background" }, { "notifications", "notification" },
                              { "documents", "doc1" }, { "documents", "doc2" } };
  for (guint i = 0; i < G_N_ELEMENTS (tables); i++) {
    g_auto (GStrv) left = store->get (store, tables[i][0], tables[i][1], app, NULL);
    g_assert_null (left);
  }
  g_auto (GStrv) others = store->get (store, "documents", "doc1", "org.example.Other", NULL);
  g_assert_cmpstr (others[0], ==, "read");
  g_assert_cmpint (vs_perm_get (store, "org.example.Other", VS_PERM_MICROPHONE), ==, VS_GRANT_ALLOW);
  /* again: nothing left to take */
  g_assert_true (vs_perm_forget (store, app, &error));
}

/* ---- names ---- */

static void
test_app_ids (void)
{
  g_assert_true (vs_is_valid_app_id ("org.gnome.Mines"));
  g_assert_true (vs_is_valid_app_id ("org.example.app-name"));
  g_assert_true (vs_is_valid_app_id ("io.github.foo_bar.Baz"));
  g_assert_false (vs_is_valid_app_id ("firefox-esr"));
  g_assert_false (vs_is_valid_app_id ("org.foo-bar.Baz"));
  g_assert_false (vs_is_valid_app_id ("org..Foo"));
  g_assert_false (vs_is_valid_app_id (".org.Foo"));
  g_assert_false (vs_is_valid_app_id ("org.Foo."));
  g_assert_false (vs_is_valid_app_id ("org.Foo Bar"));
  g_assert_false (vs_is_valid_app_id (""));

  g_assert_true (vs_is_valid_bus_name ("org.gnome.Mines"));
  g_assert_true (vs_is_valid_bus_name ("org.mpris.MediaPlayer2.foo-bar"));
  g_assert_false (vs_is_valid_bus_name ("org.2048.Game"));
  g_assert_false (vs_is_valid_bus_name ("single"));
  g_assert_false (vs_is_valid_bus_name (":1.42"));

  gboolean renamed = TRUE;
  g_autofree char *mines = vs_app_id_for ("org.gnome.Mines.desktop", &renamed);
  g_assert_cmpstr (mines, ==, "org.gnome.Mines");
  g_assert_false (renamed);
  g_autofree char *ff = vs_app_id_for ("firefox-esr.desktop", &renamed);
  g_assert_cmpstr (ff, ==, "vibeos.app.firefox-esr");
  g_assert_true (renamed);
  g_autofree char *num = vs_app_id_for ("2048.desktop", &renamed);
  g_assert_cmpstr (num, ==, "vibeos.app._2048");
  g_assert_true (vs_is_valid_app_id (num) && vs_is_valid_bus_name (num));
  g_autofree char *dash = vs_app_id_for ("org.foo-bar.Baz.desktop", &renamed);
  g_assert_cmpstr (dash, ==, "vibeos.app.org_foo-bar_Baz");
  g_assert_true (renamed);
  g_autofree char *digit = vs_app_id_for ("org.2048.Game.desktop", &renamed);
  g_assert_true (renamed);
  g_assert_true (vs_is_valid_bus_name (digit));

  g_assert_true (vs_package_is_system ("vibeos-settings"));
  g_assert_true (vs_package_is_system ("gnome-shell"));
  g_assert_false (vs_package_is_system ("gnome-mines"));
}

/* ---- Exec lines ---- */

static void
test_exec_wrap (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *a = vs_exec_wrap ("foo --new %U", "/usr/bin/vibeos-sandbox", "org.example.Foo", TRUE, &error);
  g_assert_cmpstr (a, ==, "/usr/bin/vibeos-sandbox run --file-forwarding org.example.Foo -- foo --new @@u %U @@");
  g_autofree char *b = vs_exec_wrap ("\"/opt/My App/run\" %f --title=\"a b\" %i %c %k", "/usr/bin/vibeos-sandbox",
                                     "vibeos.app.myapp", TRUE, &error);
  g_assert_cmpstr (b, ==, "/usr/bin/vibeos-sandbox run --file-forwarding vibeos.app.myapp -- \"/opt/My App/run\" @@ %f @@ "
                          "\"--title=a b\" %i %c %k");
  /* deprecated codes go, as GIO drops them */
  g_autofree char *c = vs_exec_wrap ("foo %d %m %F", "/usr/bin/vibeos-sandbox", "org.example.Foo", TRUE, &error);
  g_assert_cmpstr (c, ==, "/usr/bin/vibeos-sandbox run --file-forwarding org.example.Foo -- foo @@ %F @@");
  /* a D-Bus service: no forwarding */
  g_autofree char *d = vs_exec_wrap ("/usr/bin/foo --gapplication-service", "/usr/bin/vibeos-sandbox", "org.example.Foo",
                                     FALSE, &error);
  g_assert_cmpstr (d, ==, "/usr/bin/vibeos-sandbox run org.example.Foo -- /usr/bin/foo --gapplication-service");
  /* what needs quoting gets it, and comes back the same */
  g_autofree char *e = vs_exec_wrap ("sh -c 'echo $HOME'", "/usr/bin/vibeos-sandbox", "org.example.Foo", FALSE, &error);
  g_auto (GStrv) argv = NULL;
  g_assert_true (g_shell_parse_argv (e, NULL, &argv, NULL));
  g_assert_cmpstr (argv[4], ==, "sh");
  g_assert_cmpstr (argv[6], ==, "echo $HOME");
  g_assert_null (vs_exec_wrap ("foo \"unterminated", "/usr/bin/vibeos-sandbox", "org.example.Foo", FALSE, NULL));
}

static char *
map_doc (const char *arg, gboolean uri, gpointer data)
{
  if (!strcmp (arg, "/secret"))
    return NULL;
  return g_strdup_printf ("%s:%s", uri ? "uri" : "path", arg);
}

static void
test_exec_forward (void)
{
  const char *argv[] = { "foo", "--new", "@@u", "file:///a", "http://b", "@@", "@@", "/c", "/secret", "@@", "--x", NULL };
  g_auto (GStrv) out = vs_exec_forward (argv, map_doc, NULL);
  const char *want[] = { "foo", "--new", "uri:file:///a", "uri:http://b", "path:/c", "--x", NULL };
  g_assert_cmpstrv (out, want);
  /* nothing opened: the markers go */
  const char *none[] = { "foo", "@@u", "@@", NULL };
  g_auto (GStrv) bare = vs_exec_forward (none, map_doc, NULL);
  const char *want_bare[] = { "foo", NULL };
  g_assert_cmpstrv (bare, want_bare);
}

/* ---- the sandbox's command lines ---- */

static const char *const test_env[] = {
  "LANG=de_DE.UTF-8", "SSH_AUTH_SOCK=/run/user/1000/openssh_agent", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus",
  "WAYLAND_DISPLAY=wayland-0", "XDG_CURRENT_DESKTOP=GNOME", "GDK_SCALE=2", "PATH=/home/user/bin:/usr/bin",
  "GTK_USE_PORTAL=0", "SPONGY_DENSITY=3", "HOME=/home/user", "XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth",
  "LD_PRELOAD=/evil.so", NULL,
};

static void
fill (VsSandbox *sb)
{
  static const char *const root[] = { "/bin", "usr/bin", "/lib", "usr/lib", "/sbin", "", NULL };
  static const char *const user_dirs[] = { "/home/user/Documents", "/home/user/Pictures", NULL };
  static const char *const media[] = { "/media/user", NULL };
  static const char *const gpu[] = { "/dev/dri", NULL };
  static const char *const cams[] = { "/dev/video0", "/dev/media0", NULL };
  static const char *const home_files[] = { "/home/user/.config/gtk-4.0/gtk.css", NULL };
  static const char *const own[] = { "org.example.Foo.Helper", "org.example.Other", "not a name", NULL };
  static const char *const command[] = { "foo", "--bar", NULL };
  memset (sb, 0, sizeof *sb);
  sb->app_id = "org.example.Foo";
  sb->package = "foo-player";
  sb->instance_id = "1234";
  sb->home = "/home/user";
  sb->app_home = "/home/user/.local/share/vibeos-sandbox/data/foo";
  sb->runtime_dir = "/run/user/1000";
  sb->user_name = "user";
  sb->wayland = "/run/user/1000/wayland-0";
  sb->pipewire = "/run/user/1000/pipewire-0";
  sb->pulse = "/run/user/1000/pulse/native";
  sb->bus_proxy = "/run/user/1000/.vibeos-sandbox/1234/bus";
  sb->documents = "/run/user/1000/doc/by-app/org.example.Foo";
  sb->resolv_conf = "/run/systemd/resolve/stub-resolv.conf";
  sb->fontconfig_cache = "/var/cache/fontconfig";
  sb->root = root;
  sb->user_dirs = user_dirs;
  sb->media_dirs = media;
  sb->gpu_devices = gpu;
  sb->camera_devices = cams;
  sb->home_files = home_files;
  sb->disable_userns = TRUE;
  sb->own_names = own;
  sb->env = test_env;
  sb->flatpak_info_fd = 3;
  sb->machine_id_fd = 4;
  sb->info_fd = 5;
  sb->block_fd = 6;
  sb->command = command;
  for (int i = 0; i < VS_N_PERMS; i++)
    sb->grants.grant[i] = vs_perm_default (i);
}

static void
test_bwrap_defaults (void)
{
  VsSandbox sb;
  fill (&sb);
  g_autoptr (GPtrArray) a = vs_bwrap_args (&sb);
  g_assert_cmpstr (a->pdata[0], ==, "bwrap");
  /* namespaces; the network is shared by default */
  g_assert_true (has (a, "--unshare-user") && has (a, "--disable-userns") && has (a, "--unshare-pid")
                 && has (a, "--unshare-ipc") && has (a, "--die-with-parent") && has (a, "--new-session"));
  g_assert_false (has (a, "--unshare-net"));
  g_assert_false (has (a, "--unshare-all"));
  g_assert_true (has3 (a, "--ro-bind", "/run/systemd/resolve/stub-resolv.conf", "/run/systemd/resolve/stub-resolv.conf"));
  /* the system read-only, the symlinks as on the host */
  g_assert_true (has3 (a, "--ro-bind", "/usr", "/usr"));
  g_assert_true (has3 (a, "--ro-bind", "/etc", "/etc"));
  g_assert_true (has3 (a, "--symlink", "usr/bin", "/bin"));
  g_assert_true (has3 (a, "--ro-bind", "/sbin", "/sbin"));
  g_assert_true (has3 (a, "--ro-bind-data", "4", "/etc/machine-id"));
  g_assert_false (has3 (a, "--ro-bind", "/", "/"));
  g_assert_false (has3 (a, "--bind", "/", "/"));
  /* its own home where the user's is, and the user's files only those read through */
  g_assert_true (has3 (a, "--bind", "/home/user/.local/share/vibeos-sandbox/data/foo", "/home/user"));
  g_assert_true (has3 (a, "--ro-bind", "/home/user/.config/gtk-4.0/gtk.css", "/home/user/.config/gtk-4.0/gtk.css"));
  g_assert_false (has (a, "/home/user/Documents"));
  g_assert_false (has (a, "/media/user"));
  /* the sockets, and the filtered bus where the bus is */
  g_assert_true (has3 (a, "--ro-bind", "/run/user/1000/wayland-0", "/run/user/1000/wayland-0"));
  g_assert_true (has3 (a, "--ro-bind", "/run/user/1000/pipewire-0", "/run/user/1000/pipewire-0"));
  g_assert_false (has (a, "/run/user/1000/pipewire-0-manager"));
  g_assert_true (has3 (a, "--ro-bind", "/run/user/1000/.vibeos-sandbox/1234/bus", "/run/user/1000/bus"));
  g_assert_true (has3 (a, "--bind", "/run/user/1000/doc/by-app/org.example.Foo", "/run/user/1000/doc"));
  g_assert_true (has3 (a, "--ro-bind-data", "3", "/.flatpak-info"));
  g_assert_true (has3 (a, "--tmpfs", "/tmp", "--dir") || has (a, "/tmp"));
  /* the GPU, no camera without the permission, never the sound cards or the input devices */
  g_assert_true (has3 (a, "--dev-bind", "/dev/dri", "/dev/dri"));
  g_assert_false (has (a, "/dev/video0"));
  g_assert_false (has_prefix (a, "/dev/snd"));
  g_assert_false (has_prefix (a, "/dev/input"));
  /* which device this is (/proc/device-tree links to /sys/firmware/devicetree/base): the model and the compatible
   * list, read-only, and nothing else of /sys/firmware */
  g_assert_true (has3 (a, "--ro-bind-try", "/sys/firmware/devicetree/base/compatible",
                       "/sys/firmware/devicetree/base/compatible"));
  g_assert_true (has3 (a, "--ro-bind-try", "/sys/firmware/devicetree/base/model",
                       "/sys/firmware/devicetree/base/model"));
  guint firmware = 0;
  for (guint i = 0; i < a->len; i++)
    if (g_str_has_prefix (a->pdata[i], "/sys/firmware"))
      firmware++;
  g_assert_cmpuint (firmware, ==, 4);
  for (guint i = 0; i + 1 < a->len; i++)
    if (!g_strcmp0 (a->pdata[i], "--ro-bind-try") && g_str_has_prefix (a->pdata[i + 1], "/sys/firmware"))
      g_assert_true (g_str_has_suffix (a->pdata[i + 1], "/compatible") || g_str_has_suffix (a->pdata[i + 1], "/model"));
  /* the instance's pipes */
  g_assert_true (has (a, "--info-fd") && has (a, "--block-fd"));
  /* the command last, after -- */
  g_assert_cmpstr (a->pdata[a->len - 3], ==, "--");
  g_assert_cmpstr (a->pdata[a->len - 2], ==, "foo");
  g_assert_cmpstr (a->pdata[a->len - 1], ==, "--bar");
  /* nothing of the system bus */
  g_assert_false (has_prefix (a, "/run/dbus"));
  g_assert_false (has_prefix (a, "/var/run/dbus"));
}

static void
test_bwrap_grants (void)
{
  VsSandbox sb;
  fill (&sb);
  sb.grants.grant[VS_PERM_NETWORK] = VS_GRANT_DENY;
  sb.grants.grant[VS_PERM_CAMERA] = VS_GRANT_ALLOW;
  sb.grants.grant[VS_PERM_FILES] = VS_GRANT_ALLOW;
  sb.disable_userns = FALSE;
  g_autoptr (GPtrArray) a = vs_bwrap_args (&sb);
  g_assert_true (has (a, "--unshare-net"));
  g_assert_false (has (a, "/run/systemd/resolve/stub-resolv.conf"));
  g_assert_false (has (a, "--disable-userns"));
  g_assert_true (has3 (a, "--dev-bind", "/dev/video0", "/dev/video0"));
  g_assert_true (has3 (a, "--dev-bind", "/dev/media0", "/dev/media0"));
  g_assert_true (has3 (a, "--bind", "/home/user/Documents", "/home/user/Documents"));
  g_assert_true (has3 (a, "--bind", "/media/user", "/media/user"));
  /* the shared folders come after the home, which they are in */
  guint home = 0, docs = 0;
  for (guint i = 0; i < a->len; i++) {
    if (!g_strcmp0 (a->pdata[i], "/home/user/.local/share/vibeos-sandbox/data/foo"))
      home = i;
    if (!g_strcmp0 (a->pdata[i], "/home/user/Documents"))
      docs = i;
  }
  g_assert_cmpuint (home, <, docs);
  /* "ask" is not "allowed": the camera ask goes through the portal, without the devices */
  sb.grants.grant[VS_PERM_CAMERA] = VS_GRANT_ASK;
  g_autoptr (GPtrArray) b = vs_bwrap_args (&sb);
  g_assert_false (has (b, "/dev/video0"));
}

static const char *
env_value (GPtrArray *env, const char *name)
{
  gsize n = strlen (name);
  for (guint i = 0; i < env->len; i++)
    if (!strncmp (env->pdata[i], name, n) && ((char *) env->pdata[i])[n] == '=')
      return (char *) env->pdata[i] + n + 1;
  return NULL;
}

static void
test_environment (void)
{
  VsSandbox sb;
  fill (&sb);
  g_autoptr (GPtrArray) env = vs_sandbox_env (&sb);
  g_assert_cmpstr (env_value (env, "LANG"), ==, "de_DE.UTF-8");
  g_assert_cmpstr (env_value (env, "GDK_SCALE"), ==, "2");
  g_assert_cmpstr (env_value (env, "SPONGY_DENSITY"), ==, "3");
  g_assert_cmpstr (env_value (env, "XDG_CURRENT_DESKTOP"), ==, "GNOME");
  g_assert_null (env_value (env, "SSH_AUTH_SOCK"));
  g_assert_null (env_value (env, "XAUTHORITY"));
  g_assert_null (env_value (env, "LD_PRELOAD"));
  g_assert_null (env_value (env, "DISPLAY"));
  g_assert_cmpstr (env_value (env, "DBUS_SESSION_BUS_ADDRESS"), ==, "unix:path=/run/user/1000/bus");
  g_assert_cmpstr (env_value (env, "HOME"), ==, "/home/user");
  g_assert_cmpstr (env_value (env, "XDG_RUNTIME_DIR"), ==, "/run/user/1000");
  g_assert_cmpstr (env_value (env, "XDG_CONFIG_HOME"), ==, "/home/user/.config");
  g_assert_cmpstr (env_value (env, "GSETTINGS_BACKEND"), ==, "keyfile");
  g_assert_cmpstr (env_value (env, "GTK_USE_PORTAL"), ==, "1");
  g_assert_cmpstr (env_value (env, "GIO_USE_VFS"), ==, "local");
  g_assert_cmpstr (env_value (env, "QT_QPA_PLATFORM"), ==, "wayland;xcb");
  g_assert_cmpstr (env_value (env, "PATH"), ==, "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games");
  /* bubblewrap's --setenv for each, after --clearenv */
  g_autoptr (GPtrArray) a = vs_bwrap_args (&sb);
  g_assert_true (has (a, "--clearenv"));
  g_assert_true (has3 (a, "--setenv", "GSETTINGS_BACKEND", "keyfile"));
}

static void
test_proxy_rules (void)
{
  VsSandbox sb;
  fill (&sb);
  g_autoptr (GPtrArray) r = vs_proxy_rules (&sb);
  g_assert_cmpstr (r->pdata[0], ==, "--filter");
  g_assert_true (has (r, "--own=org.example.Foo"));
  g_assert_true (has (r, "--own=org.example.Foo.*"));
  g_assert_true (has (r, "--own=org.example.Foo.Helper"));
  g_assert_true (has (r, "--own=org.example.Other"));
  g_assert_false (has (r, "--own=not a name"));
  g_assert_false (has (r, "--own=org.mpris.MediaPlayer2.*"));
  g_assert_true (has (r, "--own=org.mpris.MediaPlayer2.org.example.Foo"));
  g_assert_true (has (r, "--own=org.mpris.MediaPlayer2.Foo"));
  g_assert_true (has (r, "--own=org.mpris.MediaPlayer2.Foo.*"));
  g_assert_true (has (r, "--own=org.mpris.MediaPlayer2.foo-player"));
  g_assert_true (has (r, "--talk=org.freedesktop.portal.*"));
  g_assert_true (has (r, "--talk=org.freedesktop.Notifications"));
  g_assert_true (has (r, "--talk=org.unifiedpush.Distributor.*"));
  /* "Open Settings": starting, showing and opening a page of Settings, and no more of it */
  g_assert_true (has (r, "--call=vibeos.Settings=org.gtk.Application.Activate@/vibeos/Settings"));
  g_assert_true (has (r, "--call=vibeos.Settings=org.gtk.Actions.Activate@/vibeos/Settings"));
  g_assert_true (has (r, "--call=org.gnome.Settings=org.gtk.Application.Open@/org/gnome/Settings"));
  g_assert_false (has (r, "--talk=vibeos.Settings"));
  g_assert_false (has (r, "--talk=org.gnome.Settings"));
  /* nothing of the user's data by default */
  g_assert_false (has_prefix (r, "--talk=org.gnome.evolution"));
  g_assert_false (has_prefix (r, "--talk=vibeos."));
  g_assert_false (has_prefix (r, "--see="));
  for (guint i = 0; i < r->len; i++) {
    const char *rule = r->pdata[i];
    g_assert_null (strstr (rule, "ModemManager"));
    g_assert_null (strstr (rule, "ca.desrt.dconf"));
    g_assert_null (strstr (rule, "org.gnome.Shell"));
    g_assert_null (strstr (rule, "org.freedesktop.secrets"));
    g_assert_null (strstr (rule, "org.gtk.vfs"));
    g_assert_null (strstr (rule, "org.a11y"));
    g_assert_null (strstr (rule, "org.freedesktop.impl.portal"));
    g_assert_false (!strcmp (rule, "--talk=*") || !strcmp (rule, "--own=*"));
  }

  sb.grants.grant[VS_PERM_CONTACTS] = VS_GRANT_ALLOW;
  g_autoptr (GPtrArray) c = vs_proxy_rules (&sb);
  g_assert_true (has (c, "--talk=org.gnome.evolution.dataserver.AddressBook10"));
  g_assert_true (has (c, "--talk=org.gnome.evolution.dataserver.Sources5"));
  g_assert_false (has (c, "--talk=org.gnome.evolution.dataserver.Calendar8"));

  sb.grants.grant[VS_PERM_CONTACTS] = VS_GRANT_DENY;
  sb.grants.grant[VS_PERM_CALENDAR] = VS_GRANT_ALLOW;
  sb.grants.grant[VS_PERM_PHONE] = VS_GRANT_ALLOW;
  sb.grants.grant[VS_PERM_SMS] = VS_GRANT_ALLOW;
  g_autoptr (GPtrArray) d = vs_proxy_rules (&sb);
  g_assert_true (has (d, "--talk=org.gnome.evolution.dataserver.Calendar8"));
  g_assert_false (has (d, "--talk=org.gnome.evolution.dataserver.AddressBook10"));
  g_assert_true (has (d, "--talk=vibeos.Dialer.Calls"));
  g_assert_true (has (d, "--talk=vibeos.Messaging.Sms"));

  g_autoptr (GPtrArray) p = vs_proxy_bwrap_args ("/run/user/1000/.vibeos-sandbox/1234", 7, "unix:path=/run/user/1000/bus",
                                                 "/run/user/1000/.vibeos-sandbox/1234/bus", 8, sb.root);
  g_assert_true (has3 (p, "--ro-bind", "/run/user/1000/bus", "/run/user/1000/bus"));
  g_assert_true (has3 (p, "--symlink", "usr/lib", "/lib"));
  g_assert_false (has3 (p, "--ro-bind", "/", "/"));
  g_assert_false (has_prefix (p, "/home"));
  g_assert_true (has3 (p, "--ro-bind-data", "7", "/.flatpak-info"));
  g_assert_true (has3 (p, "--bind", "/run/user/1000/.vibeos-sandbox/1234", "/run/user/1000/.vibeos-sandbox/1234"));
  g_assert_true (has (p, "xdg-dbus-proxy") && has (p, "--fd=8") && has (p, "--die-with-parent"));
  g_assert_cmpstr (p->pdata[p->len - 1], ==, "/run/user/1000/.vibeos-sandbox/1234/bus");

  /* as dbus-daemon gives it to the services it starts */
  g_autoptr (GPtrArray) q = vs_proxy_bwrap_args ("/run/user/1000/.vibeos-sandbox/1234", 7,
                                                 "unix:path=/run/user/1000/bus,guid=5492a5b6bbb0b71ac34b7a8f6afc6365",
                                                 "/run/user/1000/.vibeos-sandbox/1234/bus", 8, sb.root);
  g_assert_true (has3 (q, "--ro-bind", "/run/user/1000/bus", "/run/user/1000/bus"));
  const struct { const char *address, *path; } addresses[] = {
    { "unix:path=/run/user/1000/bus", "/run/user/1000/bus" },
    { "unix:guid=abc,path=/tmp/a%20b", "/tmp/a b" },
    { "unix:path=/run/a;unix:path=/run/b", "/run/a" },
    { "unix:abstract=/tmp/dbus-x", NULL },
    { "tcp:host=localhost,port=1", NULL },
    { "unix:path=relative", NULL },
    { "unix:path=/a%00b", NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS (addresses); i++) {
    g_autofree char *path = vs_bus_socket_path (addresses[i].address);
    g_assert_cmpstr (path, ==, addresses[i].path);
  }
}

static void
test_flatpak_info (void)
{
  VsSandbox sb;
  fill (&sb);
  g_autofree char *text = vs_flatpak_info (&sb);
  g_autoptr (GKeyFile) k = g_key_file_new ();
  g_assert_true (g_key_file_load_from_data (k, text, -1, G_KEY_FILE_NONE, NULL));
  g_autofree char *name = g_key_file_get_string (k, "Application", "name", NULL);
  g_autofree char *inst = g_key_file_get_string (k, "Instance", "instance-id", NULL);
  g_autofree char *shared = g_key_file_get_string (k, "Context", "shared", NULL);
  g_autofree char *devices = g_key_file_get_string (k, "Context", "devices", NULL);
  g_assert_cmpstr (name, ==, "org.example.Foo");
  g_assert_cmpstr (inst, ==, "1234");
  g_assert_cmpstr (shared, ==, "network;");
  g_assert_null (strstr (devices, "all"));
  g_assert_false (g_key_file_has_key (k, "Context", "filesystems", NULL));

  sb.grants.grant[VS_PERM_NETWORK] = VS_GRANT_DENY;
  sb.grants.grant[VS_PERM_FILES] = VS_GRANT_ALLOW;
  g_autofree char *text2 = vs_flatpak_info (&sb);
  g_autoptr (GKeyFile) k2 = g_key_file_new ();
  g_assert_true (g_key_file_load_from_data (k2, text2, -1, G_KEY_FILE_NONE, NULL));
  g_autofree char *shared2 = g_key_file_get_string (k2, "Context", "shared", NULL);
  g_assert_cmpstr (shared2, ==, "");
  g_assert_true (g_key_file_has_key (k2, "Context", "filesystems", NULL));
}

/* ---- the files written for a package ---- */

typedef struct {
  char   *tmp;
  VsDirs *dirs;
} Tree;

static const char status_text[] =
  "Package: gnome-mines\nStatus: install ok installed\nVersion: 1:48.1-1\nArchitecture: arm64\nDescription: x\n\n"
  "Package: firefox-esr\nStatus: install ok installed\nVersion: 140.5-1\nArchitecture: arm64\n\n"
  "Package: libfoo1\nStatus: install ok installed\nMulti-Arch: same\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: gone-app\nStatus: deinstall ok config-files\nVersion: 1.0\nArchitecture: arm64\n\n"
  "Package: purged-app\nStatus: purge ok not-installed\nArchitecture: arm64\n\n"
  "Package: unpacking-app\nStatus: install reinstreq half-installed\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: removing-app\nStatus: deinstall reinstreq half-installed\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: unpacked-app\nStatus: install ok unpacked\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: triggered-app\nStatus: install ok triggers-pending\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: libbar1\nStatus: install ok installed\nMulti-Arch: same\nVersion: 2.0\nArchitecture: arm64\n\n"
  "Package: libbar1\nStatus: deinstall ok config-files\nMulti-Arch: same\nVersion: 1.0\nArchitecture: armhf\n\n"
  "Package: vibeos-files\nStatus: install ok installed\nVersion: 1:0.1.1\nArchitecture: arm64\n";

static void
tree_setup (Tree *t, gconstpointer data)
{
  t->tmp = g_dir_make_tmp ("vibeos-sandbox-XXXXXX", NULL);
  g_assert_nonnull (t->tmp);
  t->dirs = g_new0 (VsDirs, 1);
  t->dirs->data_home = g_build_filename (t->tmp, "home/.local/share", NULL);
  t->dirs->config_home = g_build_filename (t->tmp, "home/.config", NULL);
  t->dirs->sysroot = g_build_filename (t->tmp, "root", NULL);
  t->dirs->dpkg_admin = g_build_filename (t->tmp, "root/var/lib/dpkg", NULL);
  t->dirs->packagekit_db = g_build_filename (t->tmp, "root/var/lib/PackageKit/transactions.db", NULL);
  t->dirs->launcher = g_strdup ("/usr/bin/vibeos-sandbox");

  g_autofree char *root = g_strdup (t->dirs->sysroot);
  write_file (root, "var/lib/dpkg/status", status_text);
  write_file (root, "var/lib/dpkg/info/gnome-mines.list",
              "/.\n/usr\n/usr/games/gnome-mines\n/usr/share/applications\n/usr/share/applications/org.gnome.Mines.desktop\n"
              "/usr/share/dbus-1/services/org.gnome.Mines.service\n/usr/share/doc/gnome-mines/copyright\n"
              "/etc/xdg/autostart/org.gnome.Mines-daemon.desktop\n/usr/lib/systemd/user/mines-helper.service\n"
              "/usr/lib/systemd/user/sub/ignored.service\n");
  write_file (root, "usr/share/applications/org.gnome.Mines.desktop",
              "[Desktop Entry]\nType=Application\nName=Mines\nName[de]=Minen\nExec=gnome-mines %U\n"
              "DBusActivatable=true\nIcon=org.gnome.Mines\nActions=new;\n\n"
              "[Desktop Action new]\nName=New game\nExec=gnome-mines --new\n");
  write_file (root, "usr/share/dbus-1/services/org.gnome.Mines.service",
              "[D-BUS Service]\nName=org.gnome.Mines\nExec=/usr/games/gnome-mines --gapplication-service\n"
              "SystemdService=mines-helper.service\n");
  write_file (root, "etc/xdg/autostart/org.gnome.Mines-daemon.desktop",
              "[Desktop Entry]\nType=Application\nName=Mines daemon\nExec=gnome-mines --daemon\n");
  write_file (root, "usr/lib/systemd/user/mines-helper.service", "[Service]\nExecStart=/bin/true\n");
  write_file (root, "var/lib/dpkg/info/firefox-esr.list",
              "/usr/share/applications/firefox-esr.desktop\n/usr/lib/firefox-esr/firefox-esr\n");
  write_file (root, "usr/share/applications/firefox-esr.desktop",
              "[Desktop Entry]\nType=Application\nName=Firefox ESR\nExec=/usr/lib/firefox-esr/firefox-esr %u\n"
              "MimeType=text/html;\n");
  write_file (root, "var/lib/dpkg/info/libfoo1:arm64.list", "/usr/lib/aarch64-linux-gnu/libfoo.so.1\n");
}

static void
tree_teardown (Tree *t, gconstpointer data)
{
  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", t->tmp);
  g_assert_cmpint (system (cmd), ==, 0);
  vs_dirs_free (t->dirs);
  g_free (t->tmp);
}

static VsPackage *
scan (Tree *t, const char *name)
{
  g_autoptr (GHashTable) installed = vs_dpkg_installed (t->dirs, NULL);
  g_assert_nonnull (installed);
  return vs_package_scan (t->dirs, g_hash_table_lookup (installed, name), NULL);
}

static char *
home_path (Tree *t, const char *rel)
{
  return g_build_filename (t->tmp, "home", rel, NULL);
}

static void
test_dpkg (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) installed = vs_dpkg_installed (t->dirs, &error);
  g_assert_no_error (error);
  g_assert_true (g_hash_table_contains (installed, "gnome-mines"));
  g_assert_true (g_hash_table_contains (installed, "libfoo1"));
  g_assert_false (g_hash_table_contains (installed, "gone-app"));
  VsDpkgPackage *mines = g_hash_table_lookup (installed, "gnome-mines");
  g_assert_cmpstr (mines->version, ==, "1:48.1-1");
  g_assert_cmpstr (mines->arch, ==, "arm64");
  g_auto (GStrv) files = vs_dpkg_files (t->dirs, g_hash_table_lookup (installed, "libfoo1"));
  g_assert_cmpstr (files[0], ==, "/usr/lib/aarch64-linux-gnu/libfoo.so.1");

  g_autoptr (VsPackage) lib = scan (t, "libfoo1");
  g_assert_null (lib);
  g_autoptr (GError) missing = NULL;
  g_assert_null (vs_package_scan (t->dirs, NULL, &missing));
  g_assert_nonnull (missing);

  g_autoptr (VsPackage) p = scan (t, "gnome-mines");
  g_assert_nonnull (p);
  g_assert_cmpuint (p->apps->len, ==, 1);
  VsApp *app = p->apps->pdata[0];
  g_assert_cmpstr (app->app_id, ==, "org.gnome.Mines");
  g_assert_false (app->renamed);
  g_assert_cmpuint (p->services->len, ==, 1);
  g_assert_cmpstr (p->bus_names->pdata[0], ==, "org.gnome.Mines");
  g_assert_cmpuint (p->autostart->len, ==, 1);
  g_assert_cmpuint (p->units->len, ==, 1);
  g_assert_cmpstr (p->units->pdata[0], ==, "mines-helper.service");
}

static void
test_dpkg_states (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) states = vs_dpkg_states (t->dirs, &error);
  g_assert_no_error (error);
  g_assert_cmpint (vs_dpkg_state (states, "gnome-mines"), ==, VS_DPKG_INSTALLED);
  /* only its configuration files left, purged, or never there: gone */
  g_assert_cmpint (vs_dpkg_state (states, "gone-app"), ==, VS_DPKG_GONE);
  g_assert_cmpint (vs_dpkg_state (states, "purged-app"), ==, VS_DPKG_GONE);
  g_assert_cmpint (vs_dpkg_state (states, "never-there"), ==, VS_DPKG_GONE);
  /* dpkg in the middle of it (an update unpacks, a removal takes the files away first): neither */
  g_assert_cmpint (vs_dpkg_state (states, "unpacking-app"), ==, VS_DPKG_CHANGING);
  g_assert_cmpint (vs_dpkg_state (states, "removing-app"), ==, VS_DPKG_CHANGING);
  g_assert_cmpint (vs_dpkg_state (states, "unpacked-app"), ==, VS_DPKG_CHANGING);
  g_assert_cmpint (vs_dpkg_state (states, "triggered-app"), ==, VS_DPKG_CHANGING);
  /* one architecture installed is installed */
  g_assert_cmpint (vs_dpkg_state (states, "libbar1"), ==, VS_DPKG_INSTALLED);
  g_assert_cmpint (vs_dpkg_state (NULL, "gnome-mines"), ==, VS_DPKG_GONE);
}

static void
test_requests (Tree *t, gconstpointer data)
{
  g_autofree char *root = g_strdup (t->dirs->sysroot);
  VsApp app = { 0 };
  /* no key: it may be given any of them */
  app.source = g_build_filename (root, "usr/share/applications/org.gnome.Mines.desktop", NULL);
  g_assert_cmpuint (vs_app_requests (&app), ==, VS_REQUESTS_ALL);
  g_assert_false (vs_app_declares (&app));
  g_free (app.source);
  write_file (root, "usr/share/applications/game.desktop",
              "[Desktop Entry]\nType=Application\nName=Game\nExec=game\n"
              VS_REQUESTS_KEY "=network; camera ;teleport;\n");
  app.source = g_build_filename (root, "usr/share/applications/game.desktop", NULL);
  g_assert_cmpuint (vs_app_requests (&app), ==, (1u << VS_PERM_NETWORK) | (1u << VS_PERM_CAMERA));
  g_assert_true (vs_app_declares (&app));
  g_free (app.source);
  /* the key, and nothing in it: it asks for nothing */
  write_file (root, "usr/share/applications/quiet.desktop",
              "[Desktop Entry]\nType=Application\nName=Quiet\nExec=quiet\n" VS_REQUESTS_KEY "=\n");
  app.source = g_build_filename (root, "usr/share/applications/quiet.desktop", NULL);
  g_assert_cmpuint (vs_app_requests (&app), ==, 0);
  g_free (app.source);
  /* a file that is not there: no limits it could name */
  app.source = g_build_filename (root, "nothing.desktop", NULL);
  g_assert_cmpuint (vs_app_requests (&app), ==, VS_REQUESTS_ALL);
  g_free (app.source);
}

static void
test_data_remove (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *home = vs_app_data_dir (t->dirs, "gnome-mines");
  g_autofree char *other = vs_app_data_dir (t->dirs, "firefox-esr");
  g_autofree char *outside = g_build_filename (t->tmp, "outside", NULL);
  write_file (home, ".local/share/gnome-mines/scores", "1");
  write_file (home, ".config/gnome-mines/settings.ini", "x");
  write_file (other, "keep", "1");
  write_file (outside, "precious", "1");
  /* links out of it are taken away, never followed */
  g_autofree char *to_file = g_build_filename (outside, "precious", NULL);
  g_autofree char *link1 = g_build_filename (home, "file-link", NULL);
  g_autofree char *link2 = g_build_filename (home, ".local/dir-link", NULL);
  g_assert_cmpint (symlink (to_file, link1), ==, 0);
  g_assert_cmpint (symlink (outside, link2), ==, 0);

  g_assert_true (vs_app_data_remove (t->dirs, "gnome-mines", &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (home, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (to_file, G_FILE_TEST_IS_REGULAR));
  g_autofree char *kept = g_build_filename (other, "keep", NULL);
  g_assert_true (g_file_test (kept, G_FILE_TEST_IS_REGULAR));
  /* nothing there: done */
  g_assert_true (vs_app_data_remove (t->dirs, "gnome-mines", &error));

  /* only a name in data/ */
  const char *bad[] = { "", ".", "..", "../packages", "a/b", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_autoptr (GError) e = NULL;
    g_assert_false (vs_app_data_remove (t->dirs, bad[i], &e));
    g_assert_error (e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  g_assert_false (vs_app_data_remove (t->dirs, NULL, NULL));
  g_assert_true (g_file_test (kept, G_FILE_TEST_IS_REGULAR));

  /* data/ itself a link: not followed */
  g_autofree char *data_dir = g_path_get_dirname (other);
  g_autofree char *moved = g_build_filename (t->tmp, "moved-data", NULL);
  g_assert_cmpint (g_rename (data_dir, moved), ==, 0);
  g_assert_cmpint (symlink (moved, data_dir), ==, 0);
  g_autoptr (GError) linked = NULL;
  g_assert_false (vs_app_data_remove (t->dirs, "firefox-esr", &linked));
  g_assert_nonnull (linked);
  g_autofree char *moved_keep = g_build_filename (moved, "firefox-esr", "keep", NULL);
  g_assert_true (g_file_test (moved_keep, G_FILE_TEST_IS_REGULAR));
}

static void
test_cache_clear (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *home = vs_app_data_dir (t->dirs, "gnome-mines");
  g_autofree char *outside = g_build_filename (t->tmp, "outside", NULL);
  write_file (home, ".cache/gnome-mines/thumbs/1.png", "1");
  write_file (home, ".config/gnome-mines/settings.ini", "x");
  write_file (outside, "precious", "1");
  g_autofree char *to_file = g_build_filename (outside, "precious", NULL);
  g_autofree char *link = g_build_filename (home, ".cache/out", NULL);
  g_assert_cmpint (symlink (outside, link), ==, 0);

  /* the cache goes, the rest of the home stays, and nothing a link points at */
  g_assert_true (vs_app_cache_clear (t->dirs, "gnome-mines", &error));
  g_assert_no_error (error);
  g_autofree char *cache = g_build_filename (home, ".cache", NULL);
  g_autofree char *settings = g_build_filename (home, ".config/gnome-mines/settings.ini", NULL);
  g_assert_false (g_file_test (cache, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (settings, G_FILE_TEST_IS_REGULAR));
  g_assert_true (g_file_test (to_file, G_FILE_TEST_IS_REGULAR));
  /* no cache, or no home: nothing to do */
  g_assert_true (vs_app_cache_clear (t->dirs, "gnome-mines", &error));
  g_assert_true (vs_app_cache_clear (t->dirs, "firefox-esr", &error));
  g_assert_no_error (error);

  /* .cache a link out of the home: the link goes, not what it points at */
  g_assert_cmpint (symlink (outside, cache), ==, 0);
  g_assert_true (vs_app_cache_clear (t->dirs, "gnome-mines", &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (cache, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (to_file, G_FILE_TEST_IS_REGULAR));

  /* the home itself a link: refused */
  g_autofree char *moved = g_build_filename (t->tmp, "moved-home", NULL);
  write_file (outside, ".cache/keep", "1");
  g_assert_cmpint (g_rename (home, moved), ==, 0);
  g_assert_cmpint (symlink (outside, home), ==, 0);
  g_autoptr (GError) linked = NULL;
  g_assert_false (vs_app_cache_clear (t->dirs, "gnome-mines", &linked));
  g_assert_nonnull (linked);
  g_autofree char *kept = g_build_filename (outside, ".cache/keep", NULL);
  g_assert_true (g_file_test (kept, G_FILE_TEST_IS_REGULAR));

  const char *bad[] = { "", ".", "..", "a/b", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_autoptr (GError) e = NULL;
    g_assert_false (vs_app_cache_clear (t->dirs, bad[i], &e));
    g_assert_error (e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
}

static void
test_registry_write (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  /* a file of the user's own where the app's goes */
  g_autofree char *apps = home_path (t, ".local/share/applications");
  g_mkdir_with_parents (apps, 0755);
  g_autofree char *desktop = g_build_filename (apps, "org.gnome.Mines.desktop", NULL);
  g_assert_true (g_file_set_contents (desktop, "[Desktop Entry]\nType=Application\nName=My mines\nExec=mine\n", -1, NULL));

  g_autoptr (VsPackage) p = scan (t, "gnome-mines");
  p->origin = g_strdup ("store");
  VsChanges changes = { 0 };
  g_assert_true (vs_registry_write (t->dirs, p, FALSE, &changes, &error));
  g_assert_no_error (error);
  g_assert_true (changes.apps && changes.services && changes.units);

  /* the desktop file: through the sandbox, files forwarded, not D-Bus activated, translations kept */
  g_autoptr (GKeyFile) k = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (k, desktop, G_KEY_FILE_KEEP_TRANSLATIONS, NULL));
  g_autofree char *exec = g_key_file_get_string (k, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (exec, ==, "/usr/bin/vibeos-sandbox run --file-forwarding org.gnome.Mines -- gnome-mines @@u %U @@");
  g_autofree char *action = g_key_file_get_string (k, "Desktop Action new", "Exec", NULL);
  g_assert_cmpstr (action, ==, "/usr/bin/vibeos-sandbox run --file-forwarding org.gnome.Mines -- gnome-mines --new");
  g_assert_false (g_key_file_has_key (k, "Desktop Entry", "DBusActivatable", NULL));
  g_autofree char *mark = g_key_file_get_string (k, "Desktop Entry", VS_MARK_KEY, NULL);
  g_assert_cmpstr (mark, ==, "org.gnome.Mines");
  g_autofree char *source = g_key_file_get_string (k, "Desktop Entry", VS_SOURCE_KEY, NULL);
  g_assert_true (g_str_has_suffix (source, "/usr/share/applications/org.gnome.Mines.desktop"));
  g_autofree char *de = g_key_file_get_locale_string (k, "Desktop Entry", "Name", "de", NULL);
  g_assert_cmpstr (de, ==, "Minen");
  /* the user's own kept */
  g_autofree char *saved = g_strconcat (desktop, ".vibeos-sandbox-saved", NULL);
  g_autofree char *saved_text = read_file (saved);
  g_assert_nonnull (strstr (saved_text, "My mines"));

  /* the D-Bus service: through the sandbox, not systemd's */
  g_autofree char *service = home_path (t, ".local/share/dbus-1/services/org.gnome.Mines.service");
  g_autoptr (GKeyFile) s = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (s, service, G_KEY_FILE_NONE, NULL));
  g_autofree char *sexec = g_key_file_get_string (s, "D-BUS Service", "Exec", NULL);
  g_assert_cmpstr (sexec, ==, "/usr/bin/vibeos-sandbox run org.gnome.Mines -- /usr/games/gnome-mines --gapplication-service");
  g_assert_false (g_key_file_has_key (s, "D-BUS Service", "SystemdService", NULL));
  g_autofree char *sname = g_key_file_get_string (s, "D-BUS Service", "Name", NULL);
  g_assert_cmpstr (sname, ==, "org.gnome.Mines");

  /* the autostart entry: hidden */
  g_autofree char *autostart = home_path (t, ".config/autostart/org.gnome.Mines-daemon.desktop");
  g_autoptr (GKeyFile) as = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (as, autostart, G_KEY_FILE_NONE, NULL));
  g_assert_true (g_key_file_get_boolean (as, "Desktop Entry", "Hidden", NULL));

  /* the user unit: masked */
  g_autofree char *unit = home_path (t, ".config/systemd/user/mines-helper.service");
  g_autofree char *target = g_file_read_link (unit, NULL);
  g_assert_cmpstr (target, ==, "/dev/null");

  /* written again: nothing changes */
  VsChanges again = { 0 };
  g_assert_true (vs_registry_write (t->dirs, p, FALSE, &again, &error));
  g_assert_false (again.apps || again.services || again.units);

  /* allowed in the background: the autostart entry runs, sandboxed */
  g_assert_true (vs_registry_write (t->dirs, p, TRUE, &again, &error));
  g_autoptr (GKeyFile) as2 = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (as2, autostart, G_KEY_FILE_NONE, NULL));
  g_assert_false (g_key_file_get_boolean (as2, "Desktop Entry", "Hidden", NULL));
  g_autofree char *aexec = g_key_file_get_string (as2, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (aexec, ==, "/usr/bin/vibeos-sandbox run org.gnome.Mines -- gnome-mines --daemon");

  /* the record */
  g_autoptr (VsPackage) loaded = vs_registry_load (t->dirs, "gnome-mines");
  g_assert_nonnull (loaded);
  g_assert_cmpstr (loaded->origin, ==, "store");
  g_assert_cmpstr (loaded->version, ==, "1:48.1-1");
  g_assert_cmpuint (loaded->apps->len, ==, 1);
  g_assert_cmpstr (((VsApp *) loaded->apps->pdata[0])->app_id, ==, "org.gnome.Mines");
  g_assert_cmpuint (loaded->written->len, ==, 3);
  g_assert_cmpuint (loaded->masked->len, ==, 1);
  g_autoptr (VsPackage) found = vs_registry_find_app (t->dirs, "org.gnome.Mines");
  g_assert_nonnull (found);
  g_assert_cmpstr (found->name, ==, "gnome-mines");
  g_assert_null (vs_registry_find_app (t->dirs, "org.gnome.Chess"));
  g_autoptr (GPtrArray) names = vs_registry_list (t->dirs);
  g_assert_cmpuint (names->len, ==, 1);

  /* removed: everything goes, the user's own file comes back, the app's data stays */
  g_autofree char *data_dir = vs_app_data_dir (t->dirs, "gnome-mines");
  g_mkdir_with_parents (data_dir, 0700);
  VsChanges removed = { 0 };
  g_assert_true (vs_registry_remove (t->dirs, "gnome-mines", &removed, &error));
  g_assert_true (removed.apps && removed.services && removed.units);
  g_autofree char *back = read_file (desktop);
  g_assert_nonnull (strstr (back, "My mines"));
  g_assert_false (g_file_test (saved, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (service, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (autostart, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (unit, G_FILE_TEST_IS_SYMLINK));
  g_assert_true (g_file_test (data_dir, G_FILE_TEST_IS_DIR));
  g_assert_null (vs_registry_load (t->dirs, "gnome-mines"));
  g_assert_false (vs_registry_remove (t->dirs, "gnome-mines", NULL, NULL));
}

static void
test_registry_renamed (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (VsPackage) p = scan (t, "firefox-esr");
  g_assert_nonnull (p);
  VsApp *app = p->apps->pdata[0];
  g_assert_cmpstr (app->app_id, ==, "vibeos.app.firefox-esr");
  g_assert_true (app->renamed);
  g_assert_true (vs_registry_write (t->dirs, p, FALSE, NULL, &error));
  g_assert_no_error (error);

  /* shows under the new id; its own still opens it, sandboxed, unseen */
  g_autofree char *shown = home_path (t, ".local/share/applications/vibeos.app.firefox-esr.desktop");
  g_autofree char *hidden = home_path (t, ".local/share/applications/firefox-esr.desktop");
  g_autoptr (GKeyFile) k = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (k, shown, G_KEY_FILE_NONE, NULL));
  g_autofree char *exec = g_key_file_get_string (k, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (exec, ==, "/usr/bin/vibeos-sandbox run --file-forwarding vibeos.app.firefox-esr -- "
                             "/usr/lib/firefox-esr/firefox-esr @@u %u @@");
  g_assert_false (g_key_file_get_boolean (k, "Desktop Entry", "NoDisplay", NULL));
  g_assert_false (g_key_file_has_key (k, "Desktop Entry", "MimeType", NULL));
  g_autoptr (GKeyFile) h = g_key_file_new ();
  g_assert_true (g_key_file_load_from_file (h, hidden, G_KEY_FILE_NONE, NULL));
  g_assert_false (g_key_file_get_boolean (h, "Desktop Entry", "Hidden", NULL));
  g_assert_true (g_key_file_get_boolean (h, "Desktop Entry", "NoDisplay", NULL));
  g_autofree char *hexec = g_key_file_get_string (h, "Desktop Entry", "Exec", NULL);
  g_assert_cmpstr (hexec, ==, exec);
  g_assert_true (g_key_file_has_key (h, "Desktop Entry", "MimeType", NULL));

  g_assert_true (vs_registry_remove (t->dirs, "firefox-esr", NULL, &error));
  g_assert_false (g_file_test (shown, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (hidden, G_FILE_TEST_EXISTS));
}

static void
test_registry_update (Tree *t, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (VsPackage) p = scan (t, "gnome-mines");
  g_assert_true (vs_registry_write (t->dirs, p, FALSE, NULL, &error));
  /* the new version has no D-Bus service and no user unit any more */
  g_autofree char *root = g_strdup (t->dirs->sysroot);
  write_file (root, "var/lib/dpkg/info/gnome-mines.list", "/usr/share/applications/org.gnome.Mines.desktop\n");
  g_autoptr (VsPackage) q = scan (t, "gnome-mines");
  VsChanges changes = { 0 };
  g_assert_true (vs_registry_write (t->dirs, q, FALSE, &changes, &error));
  g_assert_true (changes.services && changes.units);
  g_autofree char *service = home_path (t, ".local/share/dbus-1/services/org.gnome.Mines.service");
  g_autofree char *unit = home_path (t, ".config/systemd/user/mines-helper.service");
  g_assert_false (g_file_test (service, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (unit, G_FILE_TEST_IS_SYMLINK));
  g_autoptr (VsPackage) loaded = vs_registry_load (t->dirs, "gnome-mines");
  g_assert_cmpuint (loaded->written->len, ==, 1);
  /* the origin stays */
  g_assert_cmpstr (loaded->origin, ==, "store");
}

static void
exec_sql (sqlite3 *db, const char *sql)
{
  char *err = NULL;
  if (sqlite3_exec (db, sql, NULL, NULL, &err) != SQLITE_OK)
    g_error ("%s: %s", sql, err);
}

static void
test_history (Tree *t, gconstpointer data)
{
  g_autofree char *dir = g_path_get_dirname (t->dirs->packagekit_db);
  g_mkdir_with_parents (dir, 0755);
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (t->dirs->packagekit_db, &db), ==, SQLITE_OK);
  exec_sql (db, "CREATE TABLE transactions (transaction_id TEXT PRIMARY KEY,timespec TEXT,duration INTEGER,"
                "succeeded INTEGER DEFAULT 0,role TEXT,data TEXT,description TEXT,uid INTEGER DEFAULT 0,cmdline TEXT)");
  /* as PackageKit's apt backend records it: every step of each package; libglib only had its triggers run */
  exec_sql (db, "INSERT INTO transactions VALUES ('/1', '2026-11-01', 1, 1, 'install-packages', "
                "'downloading\tlibfoo1;2.0;arm64;debian\tFoo\n"
                "preparing\tgnome-mines;1:48.1-1;arm64;debian\tMines\ndecompressing\tgnome-mines;1:48.1-1;arm64;debian\tMines\n"
                "installing\tgnome-mines;1:48.1-1;arm64;debian\tMines\n"
                "preparing\tlibfoo1;2.0;arm64;debian\tFoo\ndecompressing\tlibfoo1;2.0;arm64;debian\tFoo\n"
                "installing\tlibfoo1;2.0;arm64;debian\tFoo\npreparing\tgnome-mines;1:48.1-1;arm64;debian\tMines\n"
                "installing\tgnome-mines;1:48.1-1;arm64;debian\tMines\n"
                "installing\tlibglib2.0-0t64;2.90;arm64;debian\tGLib', '', 1000, "
                "'/usr/bin/vibeos-installer --gapplication-service')");
  exec_sql (db, "INSERT INTO transactions VALUES ('/2', '2026-11-02', 1, 0, 'install-packages', "
                "'preparing\tfailed-app;1;arm64;debian\tx\ninstalling\tfailed-app;1;arm64;debian\tx', '', 1000, "
                "'/usr/bin/vibeos-installer')");
  exec_sql (db, "INSERT INTO transactions VALUES ('/3', '2026-11-03', 1, 1, 'install-packages', "
                "'decompressing\tfrom-gnome-software;1;arm64;debian\tx\ninstalling\tfrom-gnome-software;1;arm64;debian\tx', "
                "'', 1000, '/usr/bin/gnome-software')");
  exec_sql (db, "INSERT INTO transactions VALUES ('/4', '2026-11-04', 1, 1, 'update-packages', "
                "'updating\tother;2;arm64;debian\tx\npreparing\tnewdep;1;arm64;debian\tx\ninstalling\tnewdep;1;arm64;debian\tx', "
                "'', 1000, '/usr/bin/vibeos-installer')");
  exec_sql (db, "INSERT INTO transactions VALUES ('/5', '2026-11-05', 1, 1, 'install-files', "
                "'preparing\tfrom-deb;1;arm64;local\tx\ninstalling\tfrom-deb;1;arm64;local\tx\n"
                "preparing\tupdated-dep;3;arm64;debian\tx\nupdating\tupdated-dep;3;arm64;debian\tx\n"
                "installing\tupdated-dep;3;arm64;debian\tx', '', 1000, '/usr/bin/vibeos-installer')");
  exec_sql (db, "INSERT INTO transactions VALUES ('/6', '2026-11-06', 1, 1, 'install-packages', NULL, '', 0, "
                "'/usr/bin/vibeos-installer')");
  sqlite3_close (db);

  g_autoptr (GError) error = NULL;
  g_auto (GStrv) names = vs_history_installed (t->dirs, "vibeos-installer", &error);
  g_assert_no_error (error);
  const char *want[] = { "gnome-mines", "libfoo1", "from-deb", NULL };
  g_assert_cmpstrv (names, want);
  g_auto (GStrv) pk = vs_history_installed (t->dirs, "gnome-software", &error);
  const char *want_pk[] = { "from-gnome-software", NULL };
  g_assert_cmpstrv (pk, want_pk);

  g_free (t->dirs->packagekit_db);
  t->dirs->packagekit_db = g_build_filename (t->tmp, "nothing.db", NULL);
  g_auto (GStrv) none = vs_history_installed (t->dirs, "vibeos-installer", &error);
  g_assert_null (none);
  g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/perms/defaults", test_perm_defaults);
  g_test_add_func ("/perms/store", test_perm_store);
  g_test_add_func ("/names/app-ids", test_app_ids);
  g_test_add_func ("/exec/wrap", test_exec_wrap);
  g_test_add_func ("/exec/forward", test_exec_forward);
  g_test_add_func ("/bwrap/defaults", test_bwrap_defaults);
  g_test_add_func ("/bwrap/grants", test_bwrap_grants);
  g_test_add_func ("/bwrap/env", test_environment);
  g_test_add_func ("/proxy/rules", test_proxy_rules);
  g_test_add_func ("/flatpak-info", test_flatpak_info);
  g_test_add ("/registry/dpkg", Tree, NULL, tree_setup, test_dpkg, tree_teardown);
  g_test_add ("/registry/dpkg-states", Tree, NULL, tree_setup, test_dpkg_states, tree_teardown);
  g_test_add ("/registry/requests", Tree, NULL, tree_setup, test_requests, tree_teardown);
  g_test_add ("/registry/data-remove", Tree, NULL, tree_setup, test_data_remove, tree_teardown);
  g_test_add ("/registry/cache-clear", Tree, NULL, tree_setup, test_cache_clear, tree_teardown);
  g_test_add ("/registry/write", Tree, NULL, tree_setup, test_registry_write, tree_teardown);
  g_test_add ("/registry/renamed", Tree, NULL, tree_setup, test_registry_renamed, tree_teardown);
  g_test_add ("/registry/update", Tree, NULL, tree_setup, test_registry_update, tree_teardown);
  g_test_add ("/history", Tree, NULL, tree_setup, test_history, tree_teardown);
  return g_test_run ();
}
