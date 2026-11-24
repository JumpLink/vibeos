/* Running an app in its sandbox (vs-run.h) */
#define _GNU_SOURCE
#include "vs-run.h"
#include "vs-bwrap.h"
#include "vs-exec.h"
#include "vs-registry.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* where each target fd goes in bubblewrap */
enum { FD_INFO = 3, FD_MACHINE_ID, FD_BWRAP_INFO, FD_BLOCK, FD_PROXY_INFO, FD_PROXY_SYNC };

static char *
instances_dir (void)
{
  return g_build_filename (g_get_user_runtime_dir (), ".flatpak", NULL);
}

static char *
proxies_dir (void)
{
  return g_build_filename (g_get_user_runtime_dir (), ".vibeos-sandbox", NULL);
}

static void
remove_tree (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d))) {
    g_autofree char *path = g_build_filename (dir, name, NULL);
    if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
      remove_tree (path);
    else
      g_unlink (path);
  }
  g_rmdir (dir);
}

/* ---- the instances ---- */

void
vs_instance_free (VsInstance *instance)
{
  if (!instance)
    return;
  g_free (instance->instance_id);
  g_free (instance->app_id);
  g_free (instance);
}

static pid_t
read_pid (const char *path)
{
  g_autofree char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return 0;
  return (pid_t) g_ascii_strtoll (text, NULL, 10);
}

static pid_t
read_child_pid (const char *path)
{
  g_autofree char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return 0;
  const char *at = strstr (text, "\"child-pid\"");
  if (!at || !(at = strchr (at, ':')))
    return 0;
  return (pid_t) g_ascii_strtoll (at + 1, NULL, 10);
}

/* the supervisor at `pid` is ours and alive */
static gboolean
is_supervisor (pid_t pid)
{
  if (pid <= 0 || kill (pid, 0) < 0)
    return FALSE;
  g_autofree char *path = g_strdup_printf ("/proc/%d/cmdline", pid);
  g_autofree char *cmdline = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &cmdline, &len, NULL))
    return FALSE;
  /* the program, then "run" */
  return strstr (cmdline, "vibeos-sandbox") != NULL;
}

GPtrArray *
vs_instances (const char *app_id)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify) vs_instance_free);
  g_autofree char *dir = instances_dir ();
  g_autofree char *proxies = proxies_dir ();
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d))) {
    g_autofree char *inst = g_build_filename (dir, name, NULL);
    g_autofree char *info_path = g_build_filename (inst, "info", NULL);
    g_autofree char *pid_path = g_build_filename (inst, "pid", NULL);
    g_autofree char *bwrap_path = g_build_filename (inst, "bwrapinfo.json", NULL);
    g_autoptr (GKeyFile) info = g_key_file_new ();
    if (!g_key_file_load_from_file (info, info_path, G_KEY_FILE_NONE, NULL))
      continue;
    /* only ours: Flatpak's own have no X-VibeOS-Sandbox */
    if (!g_key_file_has_key (info, "Instance", VS_MARK_KEY, NULL))
      continue;
    pid_t pid = read_pid (pid_path);
    if (!is_supervisor (pid)) {
      g_autofree char *proxy = g_build_filename (proxies, name, NULL);
      remove_tree (inst);
      remove_tree (proxy);
      continue;
    }
    g_autofree char *app = g_key_file_get_string (info, "Application", "name", NULL);
    if (app_id && g_strcmp0 (app, app_id))
      continue;
    VsInstance *i = g_new0 (VsInstance, 1);
    i->instance_id = g_strdup (name);
    i->app_id = g_steal_pointer (&app);
    i->pid = pid;
    i->child_pid = read_child_pid (bwrap_path);
    g_ptr_array_add (found, i);
  }
  return found;
}

int
vs_kill_app (const char *app_id)
{
  g_autoptr (GPtrArray) running = vs_instances (app_id);
  for (guint i = 0; i < running->len; i++) {
    VsInstance *inst = running->pdata[i];
    /* the sandbox's first process takes every process in it along; the supervisor then cleans up */
    if (inst->child_pid > 0)
      kill (inst->child_pid, SIGKILL);
    else
      kill (inst->pid, SIGTERM);
  }
  return running->len;
}

/* ---- what the device has ---- */

static void
add_if_exists (GPtrArray *a, const char *path)
{
  if (path && g_file_test (path, G_FILE_TEST_EXISTS))
    g_ptr_array_add (a, g_strdup (path));
}

static GPtrArray *
strv_new (void)
{
  return g_ptr_array_new_with_free_func (g_free);
}

static const char *const *
strv_end (GPtrArray *a)
{
  g_ptr_array_add (a, NULL);
  return (const char *const *) a->pdata;
}

static void
glob_dev (GPtrArray *a, const char *const *prefixes)
{
  g_autoptr (GDir) d = g_dir_open ("/dev", 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    for (guint i = 0; prefixes[i]; i++)
      if (g_str_has_prefix (name, prefixes[i])) {
        g_ptr_array_add (a, g_build_filename ("/dev", name, NULL));
        break;
      }
}

static int
memfd_with (const char *name, const char *data)
{
  int fd = memfd_create (name, MFD_CLOEXEC);
  if (fd < 0)
    return -1;
  gsize len = strlen (data);
  if (write (fd, data, len) != (ssize_t) len || lseek (fd, 0, SEEK_SET) < 0) {
    close (fd);
    return -1;
  }
  return fd;
}

/* the app's own machine id: the same each time, and not the device's */
static char *
machine_id_for (const char *package)
{
  g_autofree char *host = NULL;
  g_file_get_contents ("/etc/machine-id", &host, NULL, NULL);
  g_autofree char *seed = g_strconcat (host ? g_strstrip (host) : "", "\nvibeos-sandbox\n", package, NULL);
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, seed, -1);
  return g_strdup_printf ("%.32s\n", sum);
}

static gboolean
bwrap_has (const char *option)
{
  static char *help = NULL;
  if (!help && !g_spawn_command_line_sync ("bwrap --help", &help, NULL, NULL, NULL))
    help = g_strdup ("");
  return strstr (help, option) != NULL;
}

static char *
documents_mount (GDBusConnection *bus)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, "org.freedesktop.portal.Documents",
                                                            "/org/freedesktop/portal/documents",
                                                            "org.freedesktop.portal.Documents", "GetMountPoint", NULL,
                                                            G_VARIANT_TYPE ("(ay)"), G_DBUS_CALL_FLAGS_NONE, 10000,
                                                            NULL, NULL);
  if (!reply)
    return NULL;
  g_autoptr (GVariant) path = g_variant_get_child_value (reply, 0);
  return g_strdup (g_variant_get_bytestring (path));
}

/* ---- files the user opened with the app ---- */

typedef struct {
  GDBusConnection *bus;
  const char      *app_id;
  const char      *mount;
} Forward;

/* the document portal's flags: the same document for the same file; a directory as a whole */
enum { DOC_REUSE_EXISTING = 1 << 0, DOC_EXPORT_DIRECTORY = 1 << 3 };

static char *
export_path (Forward *fw, const char *path)
{
  if (!fw->mount)
    return NULL;
  /* already a document */
  if (g_str_has_prefix (path, fw->mount) && path[strlen (fw->mount)] == '/')
    return g_strdup (path);
  int fd = open (path, O_PATH | O_CLOEXEC);
  if (fd < 0)
    return NULL;
  struct stat st;
  gboolean dir = fstat (fd, &st) == 0 && S_ISDIR (st.st_mode);
  g_autoptr (GUnixFDList) fds = g_unix_fd_list_new_from_array (&fd, 1);
  const char *perms[] = { "read", "write", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_with_unix_fd_list_sync (
    fw->bus, "org.freedesktop.portal.Documents", "/org/freedesktop/portal/documents",
    "org.freedesktop.portal.Documents", "AddFull",
    g_variant_new ("(@ahus^as)", g_variant_new_fixed_array (G_VARIANT_TYPE_HANDLE, (gint32[]) { 0 }, 1, sizeof (gint32)),
                   (guint32) (DOC_REUSE_EXISTING | (dir ? DOC_EXPORT_DIRECTORY : 0)), fw->app_id, perms),
    G_VARIANT_TYPE ("(asa{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000, fds, NULL, NULL, &error);
  if (!reply) {
    g_warning ("%s: %s", path, error->message);
    return NULL;
  }
  g_autofree const char **ids = NULL;
  g_autoptr (GVariant) extra = NULL;
  g_variant_get (reply, "(^a&s@a{sv})", &ids, &extra);
  if (!ids || !ids[0])
    return NULL;
  /* a file in the doc's directory; a directory is the doc's directory's one entry */
  g_autofree char *base = g_path_get_basename (path);
  return g_build_filename (fw->mount, ids[0], base, NULL);
}

static char *
forward_one (const char *arg, gboolean uri, gpointer data)
{
  Forward *fw = data;
  /* GIO hands a local file to a %u or %U as its path */
  if (!uri || g_path_is_absolute (arg))
    return export_path (fw, arg);
  g_autoptr (GFile) file = g_file_new_for_uri (arg);
  if (!g_file_is_native (file) || !g_str_has_prefix (arg, "file:"))
    return g_strdup (arg);
  g_autofree char *path = g_file_get_path (file);
  g_autofree char *doc = path ? export_path (fw, path) : NULL;
  return doc ? g_filename_to_uri (doc, NULL, NULL) : NULL;
}

/* ---- the supervisor ---- */

typedef struct {
  GMainLoop   *loop;
  GSubprocess *app;
  int          status;
} Supervisor;

static gboolean
forward_signal (gpointer data)
{
  Supervisor *s = data;
  if (s->app)
    g_subprocess_send_signal (s->app, SIGTERM);
  return G_SOURCE_CONTINUE;
}

static void
app_exited (GObject *source, GAsyncResult *result, gpointer data)
{
  Supervisor *s = data;
  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);
  if (g_subprocess_get_if_exited (G_SUBPROCESS (source)))
    s->status = g_subprocess_get_exit_status (G_SUBPROCESS (source));
  else
    s->status = 128 + g_subprocess_get_term_sig (G_SUBPROCESS (source));
  g_main_loop_quit (s->loop);
}

static char *
new_instance (char **path)
{
  g_autofree char *dir = instances_dir ();
  g_mkdir_with_parents (dir, 0700);
  for (int tries = 0; tries < 100; tries++) {
    g_autofree char *id = g_strdup_printf ("%u", g_random_int_range (1, G_MAXINT32));
    g_autofree char *p = g_build_filename (dir, id, NULL);
    if (g_mkdir (p, 0700) == 0) {
      *path = g_steal_pointer (&p);
      return g_steal_pointer (&id);
    }
  }
  return NULL;
}

/* waits for xdg-dbus-proxy's byte that it listens */
static gboolean
wait_ready (int fd, int timeout_ms)
{
  struct pollfd p = { .fd = fd, .events = POLLIN };
  char c;
  return poll (&p, 1, timeout_ms) == 1 && read (fd, &c, 1) == 1;
}

static char *
read_all (int fd)
{
  GString *s = g_string_new (NULL);
  char buf[512];
  ssize_t n;
  while ((n = read (fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
    if (n > 0)
      g_string_append_len (s, buf, n);
  return g_string_free (s, FALSE);
}

int
vs_run (const char *app_id, gboolean forward_files, char **argv)
{
  g_autoptr (GError) error = NULL;
  if (!vs_is_valid_app_id (app_id) || !vs_is_valid_bus_name (app_id)) {
    g_printerr ("vibeos-sandbox: %s is not an app id\n", app_id);
    return 125;
  }
  if (!argv || !argv[0]) {
    g_printerr ("vibeos-sandbox: no command\n");
    return 125;
  }
  g_autoptr (VsDirs) dirs = vs_dirs_new_default ();
  g_autoptr (VsPackage) package = vs_registry_find_app (dirs, app_id);
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 125;
  }
  g_autofree char *bus_address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus_address) {
    g_printerr ("vibeos-sandbox: %s\n", error->message);
    return 125;
  }

  /* the instances left behind by supervisors that are gone */
  g_ptr_array_unref (vs_instances (NULL));

  VsSandbox sb = { 0 };
  g_autoptr (VsStore) store = vs_store_new_dbus (bus);
  vs_grants_load (store, app_id, &sb.grants);
  /* what the app does not ask for it never has, whatever the store says */
  for (guint i = 0; package && i < package->apps->len; i++) {
    VsApp *app = package->apps->pdata[i];
    if (strcmp (app->app_id, app_id))
      continue;
    guint requests = vs_app_requests (app);
    for (int perm = 0; perm < VS_N_PERMS; perm++)
      if (!(requests & (1u << perm)))
        sb.grants.grant[perm] = VS_GRANT_DENY;
  }

  const char *runtime = g_get_user_runtime_dir ();
  const char *home = g_get_home_dir ();
  g_autofree char *app_home = vs_app_data_dir (dirs, package ? package->name : app_id);
  if (g_mkdir_with_parents (app_home, 0700) < 0) {
    g_printerr ("vibeos-sandbox: %s: %s\n", app_home, g_strerror (errno));
    return 125;
  }
  g_autofree char *inst_dir = NULL;
  g_autofree char *instance = new_instance (&inst_dir);
  if (!instance) {
    g_printerr ("vibeos-sandbox: no instance directory\n");
    return 125;
  }
  g_autofree char *proxies = proxies_dir ();
  g_autofree char *proxy_dir = g_build_filename (proxies, instance, NULL);
  g_autofree char *proxy_socket = g_build_filename (proxy_dir, "bus", NULL);
  g_mkdir_with_parents (proxy_dir, 0700);

  /* the device */
  const char *display = g_getenv ("WAYLAND_DISPLAY");
  g_autofree char *wayland = g_build_filename (runtime, display ? display : "wayland-0", NULL);
  g_autofree char *pipewire = g_build_filename (runtime, "pipewire-0", NULL);
  g_autofree char *pulse = g_build_filename (runtime, "pulse", "native", NULL);
  g_autofree char *mount = documents_mount (bus);
  g_autofree char *by_app = mount ? g_build_filename (mount, "by-app", app_id, NULL) : NULL;
  g_autofree char *resolv_real = realpath ("/etc/resolv.conf", NULL);

  g_autoptr (GPtrArray) root = strv_new ();
  static const char *const top[] = { "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32", NULL };
  for (guint i = 0; top[i]; i++) {
    if (g_file_test (top[i], G_FILE_TEST_IS_SYMLINK)) {
      g_ptr_array_add (root, g_strdup (top[i]));
      g_ptr_array_add (root, g_file_read_link (top[i], NULL));
    } else if (g_file_test (top[i], G_FILE_TEST_IS_DIR)) {
      g_ptr_array_add (root, g_strdup (top[i]));
      g_ptr_array_add (root, g_strdup (""));
    }
  }
  g_autoptr (GPtrArray) user_dirs = strv_new ();
  for (int d = 0; d < G_USER_N_DIRECTORIES; d++) {
    const char *path = g_get_user_special_dir (d);
    if (path && g_strcmp0 (path, home) && g_str_has_prefix (path, home) && g_file_test (path, G_FILE_TEST_IS_DIR))
      g_ptr_array_add (user_dirs, g_strdup (path));
  }
  g_autoptr (GPtrArray) media = strv_new ();
  g_autofree char *media1 = g_build_filename ("/media", g_get_user_name (), NULL);
  g_autofree char *media2 = g_build_filename ("/run/media", g_get_user_name (), NULL);
  add_if_exists (media, media1);
  add_if_exists (media, media2);
  g_autoptr (GPtrArray) gpu = strv_new ();
  add_if_exists (gpu, "/dev/dri");
  add_if_exists (gpu, "/dev/mali0");
  g_autoptr (GPtrArray) cameras = strv_new ();
  glob_dev (cameras, (const char *const[]) { "video", "media", "v4l-subdev", NULL });
  g_autoptr (GPtrArray) home_files = strv_new ();
  static const char *const read_through[] = {
    ".config/gtk-3.0/gtk.css", ".config/gtk-3.0/settings.ini", ".config/gtk-4.0/gtk.css",
    ".config/gtk-4.0/settings.ini", ".local/share/fonts", ".fonts", ".local/share/icons", ".icons", NULL,
  };
  for (guint i = 0; read_through[i]; i++) {
    g_autofree char *path = g_build_filename (home, read_through[i], NULL);
    add_if_exists (home_files, path);
  }
  g_autoptr (GPtrArray) own = strv_new ();
  for (guint i = 0; package && i < package->apps->len; i++)
    g_ptr_array_add (own, g_strdup (((VsApp *) package->apps->pdata[i])->app_id));
  for (guint i = 0; package && i < package->bus_names->len; i++)
    g_ptr_array_add (own, g_strdup (package->bus_names->pdata[i]));

  g_auto (GStrv) env = g_get_environ ();
  g_autofree char *machine_id = machine_id_for (package ? package->name : app_id);

  sb.app_id = app_id;
  sb.package = package ? package->name : NULL;
  sb.instance_id = instance;
  sb.home = home;
  sb.app_home = app_home;
  sb.runtime_dir = runtime;
  sb.user_name = g_get_user_name ();
  sb.wayland = g_file_test (wayland, G_FILE_TEST_EXISTS) ? wayland : NULL;
  sb.pipewire = g_file_test (pipewire, G_FILE_TEST_EXISTS) ? pipewire : NULL;
  sb.pulse = g_file_test (pulse, G_FILE_TEST_EXISTS) ? pulse : NULL;
  sb.bus_proxy = proxy_socket;
  sb.documents = by_app && g_file_test (by_app, G_FILE_TEST_IS_DIR) ? by_app : NULL;
  sb.resolv_conf = resolv_real && !g_str_has_prefix (resolv_real, "/etc/") && !g_str_has_prefix (resolv_real, "/usr/")
                     ? resolv_real : NULL;
  sb.fontconfig_cache = g_file_test ("/var/cache/fontconfig", G_FILE_TEST_IS_DIR) ? "/var/cache/fontconfig" : NULL;
  sb.games = g_file_test ("/var/games", G_FILE_TEST_IS_DIR) ? "/var/games" : NULL;
  sb.root = strv_end (root);
  sb.user_dirs = strv_end (user_dirs);
  sb.media_dirs = strv_end (media);
  sb.gpu_devices = strv_end (gpu);
  sb.camera_devices = strv_end (cameras);
  sb.home_files = strv_end (home_files);
  sb.has_opt = g_file_test ("/opt", G_FILE_TEST_IS_DIR);
  sb.disable_userns = bwrap_has ("--disable-userns");
  sb.own_names = strv_end (own);
  sb.env = (const char *const *) env;
  sb.flatpak_info_fd = FD_INFO;
  sb.machine_id_fd = FD_MACHINE_ID;
  sb.info_fd = FD_BWRAP_INFO;
  sb.block_fd = FD_BLOCK;

  /* the instance, as Flatpak's are: what it is, and who supervises it */
  g_autofree char *flatpak_info = vs_flatpak_info (&sb);
  g_autofree char *instance_info = NULL;
  {
    /* the instance's copy says it is ours */
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_key_file_load_from_data (k, flatpak_info, -1, G_KEY_FILE_NONE, NULL);
    g_key_file_set_boolean (k, "Instance", VS_MARK_KEY, TRUE);
    instance_info = g_key_file_to_data (k, NULL, NULL);
  }
  g_autofree char *info_path = g_build_filename (inst_dir, "info", NULL);
  g_autofree char *pid_path = g_build_filename (inst_dir, "pid", NULL);
  g_autofree char *bwrapinfo_path = g_build_filename (inst_dir, "bwrapinfo.json", NULL);
  g_autofree char *pid_text = g_strdup_printf ("%d", getpid ());
  g_file_set_contents (info_path, instance_info, -1, NULL);
  g_file_set_contents (pid_path, pid_text, -1, NULL);

  int status = 125;
  g_auto (GStrv) command = NULL;
  GSubprocess *proxy = NULL;
  int proxy_sync[2] = { -1, -1 }, info_pipe[2] = { -1, -1 }, block_pipe[2] = { -1, -1 };

  /* the session bus filter */
  {
    int proxy_info = memfd_with ("flatpak-info", flatpak_info);
    if (proxy_info < 0 || !g_unix_open_pipe (proxy_sync, FD_CLOEXEC, &error)) {
      g_printerr ("vibeos-sandbox: %s\n", error ? error->message : g_strerror (errno));
      goto out;
    }
    g_autoptr (GPtrArray) args = vs_proxy_bwrap_args (proxy_dir, FD_PROXY_INFO, bus_address, proxy_socket,
                                                      FD_PROXY_SYNC, sb.root);
    g_autoptr (GPtrArray) rules = vs_proxy_rules (&sb);
    for (guint i = 0; i < rules->len; i++)
      g_ptr_array_add (args, g_strdup (rules->pdata[i]));
    g_ptr_array_add (args, NULL);
    g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_fd (l, proxy_info, FD_PROXY_INFO);
    g_subprocess_launcher_take_fd (l, proxy_sync[1], FD_PROXY_SYNC);
    proxy_sync[1] = -1;
    proxy = g_subprocess_launcher_spawnv (l, (const char *const *) args->pdata, &error);
    if (!proxy) {
      g_printerr ("vibeos-sandbox: xdg-dbus-proxy: %s\n", error->message);
      goto out;
    }
    if (!wait_ready (proxy_sync[0], 10000)) {
      g_printerr ("vibeos-sandbox: xdg-dbus-proxy did not start\n");
      goto out;
    }
  }

  /* files the user opened with it */
  if (forward_files) {
    Forward fw = { bus, app_id, mount };
    command = vs_exec_forward ((const char *const *) argv, forward_one, &fw);
  } else {
    command = g_strdupv (argv);
  }
  sb.command = (const char *const *) command;

  /* the app */
  {
    int app_info = memfd_with ("flatpak-info", flatpak_info);
    int mid = memfd_with ("machine-id", machine_id);
    if (app_info < 0 || mid < 0 || !g_unix_open_pipe (info_pipe, FD_CLOEXEC, &error)
        || !g_unix_open_pipe (block_pipe, FD_CLOEXEC, &error)) {
      g_printerr ("vibeos-sandbox: %s\n", error ? error->message : g_strerror (errno));
      goto out;
    }
    g_autoptr (GPtrArray) args = vs_bwrap_args (&sb);
    g_ptr_array_add (args, NULL);
    g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_fd (l, app_info, FD_INFO);
    g_subprocess_launcher_take_fd (l, mid, FD_MACHINE_ID);
    g_subprocess_launcher_take_fd (l, info_pipe[1], FD_BWRAP_INFO);
    g_subprocess_launcher_take_fd (l, block_pipe[0], FD_BLOCK);
    info_pipe[1] = block_pipe[0] = -1;
    Supervisor s = { g_main_loop_new (NULL, FALSE), NULL, 125 };
    s.app = g_subprocess_launcher_spawnv (l, (const char *const *) args->pdata, &error);
    g_clear_object (&l);
    if (!s.app) {
      g_printerr ("vibeos-sandbox: bwrap: %s\n", error->message);
      g_main_loop_unref (s.loop);
      goto out;
    }
    /* bubblewrap says which process is the sandbox, then waits for the go */
    g_autofree char *bwrapinfo = read_all (info_pipe[0]);
    g_file_set_contents (bwrapinfo_path, bwrapinfo, -1, NULL);
    if (write (block_pipe[1], "x", 1) != 1)
      g_printerr ("vibeos-sandbox: %s\n", g_strerror (errno));
    g_clear_fd (&block_pipe[1], NULL);

    guint sigs[3] = {
      g_unix_signal_add (SIGTERM, forward_signal, &s),
      g_unix_signal_add (SIGINT, forward_signal, &s),
      g_unix_signal_add (SIGHUP, forward_signal, &s),
    };
    g_subprocess_wait_async (s.app, NULL, app_exited, &s);
    g_main_loop_run (s.loop);
    for (guint i = 0; i < G_N_ELEMENTS (sigs); i++)
      g_source_remove (sigs[i]);
    status = s.status;
    g_clear_object (&s.app);
    g_main_loop_unref (s.loop);
  }

out:
  g_clear_fd (&proxy_sync[0], NULL);
  g_clear_fd (&proxy_sync[1], NULL);
  g_clear_fd (&info_pipe[0], NULL);
  g_clear_fd (&info_pipe[1], NULL);
  g_clear_fd (&block_pipe[0], NULL);
  g_clear_fd (&block_pipe[1], NULL);
  if (proxy) {
    /* it goes when its pipe closes; this is in case it does not */
    g_subprocess_send_signal (proxy, SIGTERM);
    g_subprocess_wait (proxy, NULL, NULL);
    g_object_unref (proxy);
  }
  remove_tree (inst_dir);
  remove_tree (proxy_dir);
  return status;
}
