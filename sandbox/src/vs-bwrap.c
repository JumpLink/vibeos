/* The sandbox's command lines (vs-bwrap.h) */
#include "vs-bwrap.h"

#include <string.h>

static void
add (GPtrArray *a, const char *s)
{
  g_ptr_array_add (a, g_strdup (s));
}

static void
add_fmt (GPtrArray *a, const char *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
add_fmt (GPtrArray *a, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  g_ptr_array_add (a, g_strdup_vprintf (fmt, ap));
  va_end (ap);
}

static void
add3 (GPtrArray *a, const char *x, const char *y, const char *z)
{
  add (a, x);
  add (a, y);
  add (a, z);
}

/* ---- names ---- */

static gboolean
name_char (char c, gboolean dash)
{
  return g_ascii_isalnum (c) || c == '_' || (dash && c == '-');
}

gboolean
vs_is_valid_app_id (const char *id)
{
  if (!id || !*id || strlen (id) > 255 || *id == '.')
    return FALSE;
  const char *last = strrchr (id, '.');
  if (!last || !last[1])
    return FALSE;
  for (const char *s = id; *s; s++) {
    if (*s == '.') {
      if (s[1] == '.' || !s[1])
        return FALSE;
      continue;
    }
    if (!name_char (*s, s > last))
      return FALSE;
  }
  return TRUE;
}

gboolean
vs_is_valid_bus_name (const char *name)
{
  if (!name || !*name || strlen (name) > 255 || *name == '.' || *name == ':')
    return FALSE;
  gboolean dot = FALSE, start = TRUE;
  for (const char *s = name; *s; s++) {
    if (*s == '.') {
      if (start)
        return FALSE;
      dot = start = TRUE;
      continue;
    }
    if (!name_char (*s, TRUE) || (start && g_ascii_isdigit (*s)))
      return FALSE;
    start = FALSE;
  }
  return dot && !start;
}

/* ---- the environment ---- */

/* what passes into the sandbox from the launcher's environment: the language, the session's kind, toolkit and GPU
 * driver settings. Not: anything that names a socket or a file of the user's (SSH_AUTH_SOCK, GPG_AGENT_INFO,
 * XAUTHORITY, PULSE_SERVER, SESSION_MANAGER), nor the bus address, the home or the runtime directory, which are the
 * sandbox's own */
static const char *const pass_names[] = {
  "LANG", "LANGUAGE", "TZ", "TERM", "COLORTERM", "XMODIFIERS", "DESKTOP_SESSION", "XDG_CURRENT_DESKTOP",
  "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE", "XDG_SESSION_CLASS", "XDG_MENU_PREFIX", "XDG_DATA_DIRS",
  "XDG_CONFIG_DIRS", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "GSETTINGS_SCHEMA_DIR", NULL,
};
static const char *const pass_prefixes[] = {
  "LC_", "GDK_", "GSK_", "GTK_", "QT_", "SDL_", "MESA_", "PAN_", "VK_", "LIBGL_", "__GL", "LIBCAMERA_", "MOZ_",
  "SPONGY_", "XCURSOR_", "CLUTTER_", "COGL_", "ADW_", NULL,
};
/* set by the sandbox itself, whatever the launcher had */
static const char *const own_names[] = { "GTK_USE_PORTAL", NULL };

static gboolean
passes (const char *name)
{
  if (g_strv_contains (own_names, name))
    return FALSE;
  if (g_strv_contains (pass_names, name))
    return TRUE;
  for (guint i = 0; pass_prefixes[i]; i++)
    if (g_str_has_prefix (name, pass_prefixes[i]))
      return TRUE;
  return FALSE;
}

static void
env_set (GPtrArray *env, const char *name, const char *value)
{
  gsize n = strlen (name);
  for (guint i = 0; i < env->len; i++) {
    const char *e = env->pdata[i];
    if (!strncmp (e, name, n) && e[n] == '=') {
      g_free (env->pdata[i]);
      env->pdata[i] = g_strconcat (name, "=", value, NULL);
      return;
    }
  }
  g_ptr_array_add (env, g_strconcat (name, "=", value, NULL));
}

static const char *
env_get (const char *const *env, const char *name)
{
  gsize n = strlen (name);
  for (guint i = 0; env && env[i]; i++)
    if (!strncmp (env[i], name, n) && env[i][n] == '=')
      return env[i] + n + 1;
  return NULL;
}

GPtrArray *
vs_sandbox_env (const VsSandbox *sb)
{
  GPtrArray *env = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; sb->env && sb->env[i]; i++) {
    const char *eq = strchr (sb->env[i], '=');
    if (!eq)
      continue;
    g_autofree char *name = g_strndup (sb->env[i], eq - sb->env[i]);
    if (passes (name))
      add (env, sb->env[i]);
  }
  g_autofree char *config = g_build_filename (sb->home, ".config", NULL);
  g_autofree char *data = g_build_filename (sb->home, ".local", "share", NULL);
  g_autofree char *cache = g_build_filename (sb->home, ".cache", NULL);
  g_autofree char *state = g_build_filename (sb->home, ".local", "state", NULL);
  g_autofree char *bus = g_strconcat ("unix:path=", sb->runtime_dir, "/bus", NULL);
  env_set (env, "HOME", sb->home);
  env_set (env, "USER", sb->user_name);
  env_set (env, "LOGNAME", sb->user_name);
  env_set (env, "SHELL", "/bin/sh");
  env_set (env, "PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games");
  env_set (env, "XDG_RUNTIME_DIR", sb->runtime_dir);
  env_set (env, "XDG_CONFIG_HOME", config);
  env_set (env, "XDG_DATA_HOME", data);
  env_set (env, "XDG_CACHE_HOME", cache);
  env_set (env, "XDG_STATE_HOME", state);
  if (!env_get (sb->env, "XDG_DATA_DIRS"))
    env_set (env, "XDG_DATA_DIRS", "/usr/local/share:/usr/share");
  if (!env_get (sb->env, "XDG_CONFIG_DIRS"))
    env_set (env, "XDG_CONFIG_DIRS", "/etc/xdg");
  if (sb->bus_proxy)
    env_set (env, "DBUS_SESSION_BUS_ADDRESS", bus);
  if (sb->wayland)
    env_set (env, "WAYLAND_DISPLAY", "wayland-0");
  /* no dconf (it would read and write every app's settings): GLib keeps the app's own in a file in its home */
  env_set (env, "GSETTINGS_BACKEND", "keyfile");
  /* no GVfs daemons on the filtered bus */
  env_set (env, "GIO_USE_VFS", "local");
  env_set (env, "GTK_USE_PORTAL", "1");
  /* no accessibility bus inside */
  env_set (env, "NO_AT_BRIDGE", "1");
  /* Qt picks X11 first on GNOME unless told; there is no X11 in here */
  if (!env_get (sb->env, "QT_QPA_PLATFORM"))
    env_set (env, "QT_QPA_PLATFORM", "wayland;xcb");
  /* WebKit would start its web processes in a sandbox of its own through Flatpak's portal, which is not there: they
   * run in this one */
  env_set (env, "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
  return env;
}

/* ---- bubblewrap ---- */

static void
bind_list (GPtrArray *a, const char *how, const char *const *paths)
{
  for (guint i = 0; paths && paths[i]; i++)
    add3 (a, how, paths[i], paths[i]);
}

GPtrArray *
vs_bwrap_args (const VsSandbox *sb)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  add (a, "bwrap");

  /* its own namespaces: user (no new ones inside), processes, System V IPC, cgroups; the network's own too, without
   * the permission (only loopback in it) */
  add (a, "--unshare-user");
  if (sb->disable_userns)
    add (a, "--disable-userns");
  add (a, "--unshare-pid");
  add (a, "--unshare-ipc");
  add (a, "--unshare-cgroup-try");
  if (!vs_grants_allowed (&sb->grants, VS_PERM_NETWORK))
    add (a, "--unshare-net");
  add (a, "--die-with-parent");
  /* a session of its own: it cannot type into the terminal it was started from */
  add (a, "--new-session");

  /* the system, read-only */
  add3 (a, "--ro-bind", "/usr", "/usr");
  for (guint i = 0; sb->root && sb->root[i] && sb->root[i + 1]; i += 2) {
    if (*sb->root[i + 1])
      add3 (a, "--symlink", sb->root[i + 1], sb->root[i]);
    else
      add3 (a, "--ro-bind", sb->root[i], sb->root[i]);
  }
  add3 (a, "--ro-bind", "/etc", "/etc");
  if (sb->machine_id_fd >= 0) {
    add (a, "--ro-bind-data");
    add_fmt (a, "%d", sb->machine_id_fd);
    add (a, "/etc/machine-id");
  }
  if (sb->has_opt)
    add3 (a, "--ro-bind", "/opt", "/opt");

  add (a, "--proc");
  add (a, "/proc");
  add (a, "--dev");
  add (a, "/dev");
  add (a, "--tmpfs");
  add (a, "/dev/shm");
  bind_list (a, "--dev-bind", sb->gpu_devices);
  if (vs_grants_allowed (&sb->grants, VS_PERM_CAMERA))
    bind_list (a, "--dev-bind", sb->camera_devices);
  /* what GPU drivers and device lookups read */
  static const char *const sys[] = { "/sys/block", "/sys/bus", "/sys/class", "/sys/dev", "/sys/devices", NULL };
  for (guint i = 0; sys[i]; i++)
    add3 (a, "--ro-bind-try", sys[i], sys[i]);

  add (a, "--tmpfs");
  add (a, "/tmp");
  add (a, "--dir");
  add (a, "/var");
  add (a, "--tmpfs");
  add (a, "/var/tmp");
  add3 (a, "--symlink", "../run", "/var/run");
  add3 (a, "--symlink", "../run/lock", "/var/lock");
  add3 (a, "--symlink", "/etc/machine-id", "/var/lib/dbus/machine-id");
  if (sb->fontconfig_cache)
    add3 (a, "--ro-bind", sb->fontconfig_cache, sb->fontconfig_cache);
  if (sb->games)
    add3 (a, "--ro-bind", sb->games, sb->games);

  /* /run: only the user's runtime directory, with the sockets the app may use */
  add (a, "--tmpfs");
  add (a, "/run");
  add (a, "--perms");
  add (a, "0700");
  add (a, "--dir");
  add (a, sb->runtime_dir);
  if (vs_grants_allowed (&sb->grants, VS_PERM_NETWORK) && sb->resolv_conf)
    add3 (a, "--ro-bind", sb->resolv_conf, sb->resolv_conf);
  g_autofree char *wayland = g_build_filename (sb->runtime_dir, "wayland-0", NULL);
  g_autofree char *pipewire = g_build_filename (sb->runtime_dir, "pipewire-0", NULL);
  g_autofree char *pulse = g_build_filename (sb->runtime_dir, "pulse", "native", NULL);
  g_autofree char *bus = g_build_filename (sb->runtime_dir, "bus", NULL);
  g_autofree char *doc = g_build_filename (sb->runtime_dir, "doc", NULL);
  g_autofree char *info_link = g_build_filename (sb->runtime_dir, "flatpak-info", NULL);
  if (sb->wayland)
    add3 (a, "--ro-bind", sb->wayland, wayland);
  if (sb->pipewire)
    add3 (a, "--ro-bind", sb->pipewire, pipewire);
  if (sb->pulse)
    add3 (a, "--ro-bind", sb->pulse, pulse);
  if (sb->bus_proxy)
    add3 (a, "--ro-bind", sb->bus_proxy, bus);
  if (sb->documents)
    add3 (a, "--bind", sb->documents, doc);

  /* the home: the app's own directory where the user's is */
  add3 (a, "--bind", sb->app_home, sb->home);
  bind_list (a, "--ro-bind", sb->home_files);
  if (vs_grants_allowed (&sb->grants, VS_PERM_FILES)) {
    bind_list (a, "--bind", sb->user_dirs);
    bind_list (a, "--bind", sb->media_dirs);
  }

  /* who it is, for the portals, PipeWire, mutter and GLib */
  if (sb->flatpak_info_fd >= 0) {
    add (a, "--ro-bind-data");
    add_fmt (a, "%d", sb->flatpak_info_fd);
    add (a, "/.flatpak-info");
    add3 (a, "--symlink", "../../../.flatpak-info", info_link);
  }

  if (sb->info_fd >= 0) {
    add (a, "--info-fd");
    add_fmt (a, "%d", sb->info_fd);
  }
  if (sb->block_fd >= 0) {
    add (a, "--block-fd");
    add_fmt (a, "%d", sb->block_fd);
  }

  add (a, "--clearenv");
  g_autoptr (GPtrArray) env = vs_sandbox_env (sb);
  for (guint i = 0; i < env->len; i++) {
    const char *e = env->pdata[i];
    const char *eq = strchr (e, '=');
    add (a, "--setenv");
    g_ptr_array_add (a, g_strndup (e, eq - e));
    add (a, eq + 1);
  }
  add (a, "--chdir");
  add (a, sb->home);
  add (a, "--");
  for (guint i = 0; sb->command && sb->command[i]; i++)
    add (a, sb->command[i]);
  return a;
}

/* ---- the session bus filter ---- */

static gboolean
has_rule (GPtrArray *a, const char *name)
{
  g_autofree char *rule = g_strconcat ("--own=", name, NULL);
  for (guint i = 0; i < a->len; i++)
    if (!strcmp (a->pdata[i], rule))
      return TRUE;
  return FALSE;
}

GPtrArray *
vs_proxy_rules (const VsSandbox *sb)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  add (a, "--filter");
  /* its own names: its id and what is under it, the names its package's D-Bus services have, and a media player's */
  add_fmt (a, "--own=%s", sb->app_id);
  add_fmt (a, "--own=%s.*", sb->app_id);
  for (guint i = 0; sb->own_names && sb->own_names[i]; i++)
    if (vs_is_valid_bus_name (sb->own_names[i]) && g_strcmp0 (sb->own_names[i], sb->app_id))
      add_fmt (a, "--own=%s", sb->own_names[i]);
  /* a media player's controls (MPRIS) under its app id, the last part of it or its package's name: not
   * org.mpris.MediaPlayer2.*, which would show it every other player and what plays there */
  g_autofree char *last = g_strdup (strrchr (sb->app_id, '.') + 1);
  const char *mpris[] = { sb->app_id, last, sb->package, NULL };
  for (guint i = 0; i < G_N_ELEMENTS (mpris) - 1; i++) {
    if (!mpris[i])
      continue;
    g_autofree char *name = g_strconcat ("org.mpris.MediaPlayer2.", mpris[i], NULL);
    g_strdelimit (name + strlen ("org.mpris.MediaPlayer2."), "+", '_');
    if (!vs_is_valid_bus_name (name) || has_rule (a, name))
      continue;
    add_fmt (a, "--own=%s", name);
    add_fmt (a, "--own=%s.*", name);
  }
  /* the portals, which ask the user and check the permission store */
  add (a, "--talk=org.freedesktop.portal.*");
  add (a, "--call=org.freedesktop.portal.*=*");
  add (a, "--broadcast=org.freedesktop.portal.*=@/org/freedesktop/portal/*");
  /* notifications: the shell's own notification policy applies */
  add (a, "--talk=org.freedesktop.Notifications");
  /* push messages: UnifiedPush distributors; each app only receives what is sent to its own registration, encrypted
   * to it. The messages come in as calls on the app's own name, which the proxy does not filter (it filters only
   * what the app sends, and broadcast signals) */
  add (a, "--talk=org.unifiedpush.Distributor.*");
  /* the contacts and the calendars: evolution-data-server's address books and calendars run in its factories */
  if (vs_grants_allowed (&sb->grants, VS_PERM_CONTACTS) || vs_grants_allowed (&sb->grants, VS_PERM_CALENDAR))
    add (a, "--talk=org.gnome.evolution.dataserver.Sources5");
  if (vs_grants_allowed (&sb->grants, VS_PERM_CONTACTS))
    add (a, "--talk=org.gnome.evolution.dataserver.AddressBook10");
  if (vs_grants_allowed (&sb->grants, VS_PERM_CALENDAR))
    add (a, "--talk=org.gnome.evolution.dataserver.Calendar8");
  /* calls and the call log, SMS: VibeOS's own services (ModemManager is on the system bus, which is never there) */
  if (vs_grants_allowed (&sb->grants, VS_PERM_PHONE))
    add (a, "--talk=vibeos.Dialer.Calls");
  if (vs_grants_allowed (&sb->grants, VS_PERM_SMS))
    add (a, "--talk=vibeos.Messaging.Sms");
  return a;
}

char *
vs_bus_socket_path (const char *address)
{
  g_autofree char *first = g_strndup (address, strcspn (address, ";"));
  if (!g_str_has_prefix (first, "unix:"))
    return NULL;
  g_auto (GStrv) keys = g_strsplit (first + strlen ("unix:"), ",", -1);
  for (guint i = 0; keys[i]; i++) {
    if (!g_str_has_prefix (keys[i], "path="))
      continue;
    /* values may escape bytes as %XX */
    const char *v = keys[i] + strlen ("path=");
    GString *path = g_string_new (NULL);
    for (; *v; v++) {
      if (*v == '%' && g_ascii_isxdigit (v[1]) && g_ascii_isxdigit (v[2])) {
        g_string_append_c (path, (char) (g_ascii_xdigit_value (v[1]) << 4 | g_ascii_xdigit_value (v[2])));
        v += 2;
      } else {
        g_string_append_c (path, *v);
      }
    }
    if (path->len && path->str[0] == '/' && strlen (path->str) == path->len)
      return g_string_free (path, FALSE);
    g_string_free (path, TRUE);
    return NULL;
  }
  return NULL;
}

GPtrArray *
vs_proxy_bwrap_args (const char *proxy_dir, int flatpak_info_fd, const char *bus_address, const char *socket,
                     int sync_fd, const char *const *root)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  add (a, "bwrap");
  /* the system's programs, read-only, the real bus's socket and the directory its own goes in; nothing else */
  add3 (a, "--ro-bind", "/usr", "/usr");
  for (guint i = 0; root && root[i] && root[i + 1]; i += 2) {
    if (*root[i + 1])
      add3 (a, "--symlink", root[i + 1], root[i]);
    else
      add3 (a, "--ro-bind", root[i], root[i]);
  }
  add3 (a, "--ro-bind", "/etc", "/etc");
  /* an abstract socket needs nothing: the proxy shares the network namespace */
  g_autofree char *bus = vs_bus_socket_path (bus_address);
  if (bus)
    add3 (a, "--ro-bind", bus, bus);
  add3 (a, "--bind", proxy_dir, proxy_dir);
  add (a, "--ro-bind-data");
  add_fmt (a, "%d", flatpak_info_fd);
  add (a, "/.flatpak-info");
  add (a, "--die-with-parent");
  add (a, "--");
  add (a, "xdg-dbus-proxy");
  add_fmt (a, "--fd=%d", sync_fd);
  add (a, bus_address);
  add (a, socket);
  return a;
}

/* ---- /.flatpak-info ---- */

char *
vs_flatpak_info (const VsSandbox *sb)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  g_key_file_set_string (k, "Application", "name", sb->app_id);
  g_key_file_set_string (k, "Instance", "instance-id", sb->instance_id);
  g_key_file_set_string (k, "Instance", "instance-path", sb->app_home);
  g_key_file_set_boolean (k, "Instance", "session-bus-proxy", TRUE);
  g_key_file_set_boolean (k, "Instance", "system-bus-proxy", FALSE);
  /* what it shares with the system: GLib and the portals read "network" here */
  g_key_file_set_string (k, "Context", "shared", vs_grants_allowed (&sb->grants, VS_PERM_NETWORK) ? "network;" : "");
  GString *sockets = g_string_new (NULL);
  if (sb->wayland)
    g_string_append (sockets, "wayland;");
  if (sb->pulse)
    g_string_append (sockets, "pulseaudio;");
  if (sb->bus_proxy)
    g_string_append (sockets, "session-bus;");
  g_key_file_set_string (k, "Context", "sockets", sockets->str);
  g_string_free (sockets, TRUE);
  /* never "all": PipeWire would make it a manager */
  g_key_file_set_string (k, "Context", "devices", sb->gpu_devices && sb->gpu_devices[0] ? "dri;" : "");
  /* the document portal gives the app a file it can see already as it is */
  if (vs_grants_allowed (&sb->grants, VS_PERM_FILES))
    g_key_file_set_string (k, "Context", "filesystems",
                           "xdg-desktop;xdg-documents;xdg-download;xdg-music;xdg-pictures;xdg-videos;"
                           "xdg-templates;xdg-public-share;");
  return g_key_file_to_data (k, NULL, NULL);
}
