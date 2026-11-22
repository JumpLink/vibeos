/* Which packages run sandboxed (vs-registry.h) */
#include "vs-registry.h"
#include "vs-bwrap.h"
#include "vs-exec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVED_SUFFIX ".vibeos-sandbox-saved"

VsDirs *
vs_dirs_new_default (void)
{
  VsDirs *dirs = g_new0 (VsDirs, 1);
  dirs->data_home = g_strdup (g_get_user_data_dir ());
  dirs->config_home = g_strdup (g_get_user_config_dir ());
  dirs->sysroot = g_strdup ("");
  dirs->dpkg_admin = g_strdup ("/var/lib/dpkg");
  dirs->packagekit_db = g_strdup ("/var/lib/PackageKit/transactions.db");
  dirs->launcher = g_strdup ("/usr/bin/vibeos-sandbox");
  return dirs;
}

void
vs_dirs_free (VsDirs *dirs)
{
  if (!dirs)
    return;
  g_free (dirs->data_home);
  g_free (dirs->config_home);
  g_free (dirs->sysroot);
  g_free (dirs->dpkg_admin);
  g_free (dirs->packagekit_db);
  g_free (dirs->launcher);
  g_free (dirs);
}

static char *
sys_path (const VsDirs *dirs, const char *path)
{
  return g_strconcat (dirs->sysroot, path, NULL);
}

static char *
registry_dir (const VsDirs *dirs)
{
  return g_build_filename (dirs->data_home, "vibeos-sandbox", "packages", NULL);
}

char *
vs_app_data_dir (const VsDirs *dirs, const char *package_or_app_id)
{
  return g_build_filename (dirs->data_home, "vibeos-sandbox", "data", package_or_app_id, NULL);
}

/* removes `name` in the directory `dir` and all in it, on `dev` only */
static gboolean
remove_at (int dir, const char *name, dev_t dev, GError **error)
{
  struct stat st;
  if (fstatat (dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
    if (errno == ENOENT)
      return TRUE;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", name, g_strerror (errno));
    return FALSE;
  }
  if (S_ISDIR (st.st_mode)) {
    if (st.st_dev != dev) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: another file system is mounted there", name);
      return FALSE;
    }
    int fd = openat (dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir (fd) : NULL;
    if (!d) {
      int e = errno;
      if (fd >= 0)
        close (fd);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "%s: %s", name, g_strerror (e));
      return FALSE;
    }
    gboolean ok = TRUE;
    struct dirent *entry;
    while (ok && (errno = 0, entry = readdir (d)))
      if (strcmp (entry->d_name, ".") && strcmp (entry->d_name, ".."))
        ok = remove_at (dirfd (d), entry->d_name, dev, error);
    closedir (d);
    if (!ok)
      return FALSE;
  }
  if (unlinkat (dir, name, S_ISDIR (st.st_mode) ? AT_REMOVEDIR : 0) < 0 && errno != ENOENT) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", name, g_strerror (errno));
    return FALSE;
  }
  return TRUE;
}

/* data/ itself, opened so that nothing under it is reached through a symbolic link; -1 and no error: there is none */
static int
open_data (const VsDirs *dirs, const char *name, dev_t *dev, GError **error)
{
  if (!name || !*name || strchr (name, '/') || !strcmp (name, ".") || !strcmp (name, "..")) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "\"%s\" is no app's data", name ? name : "");
    return -1;
  }
  g_autofree char *root = g_build_filename (dirs->data_home, "vibeos-sandbox", "data", NULL);
  int fd = open (root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno != ENOENT)
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", root, g_strerror (errno));
    return -1;
  }
  struct stat st;
  if (fstat (fd, &st) < 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", root, g_strerror (errno));
    close (fd);
    return -1;
  }
  *dev = st.st_dev;
  return fd;
}

gboolean
vs_app_data_remove (const VsDirs *dirs, const char *package_or_app_id, GError **error)
{
  GError *e = NULL;
  dev_t dev;
  int fd = open_data (dirs, package_or_app_id, &dev, &e);
  if (fd < 0) {
    if (!e)
      return TRUE;
    g_propagate_error (error, e);
    return FALSE;
  }
  gboolean ok = remove_at (fd, package_or_app_id, dev, error);
  close (fd);
  return ok;
}

gboolean
vs_app_cache_clear (const VsDirs *dirs, const char *package_or_app_id, GError **error)
{
  GError *e = NULL;
  dev_t dev;
  int root = open_data (dirs, package_or_app_id, &dev, &e);
  if (root < 0) {
    if (!e)
      return TRUE;
    g_propagate_error (error, e);
    return FALSE;
  }
  /* the app's home itself, not through a link and not another file system */
  int fd = openat (root, package_or_app_id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int err = errno;
  close (root);
  if (fd < 0) {
    if (err == ENOENT)
      return TRUE;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err), "%s: %s", package_or_app_id, g_strerror (err));
    return FALSE;
  }
  struct stat st;
  gboolean ok = fstat (fd, &st) == 0;
  if (!ok)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", package_or_app_id, g_strerror (errno));
  else if (st.st_dev != dev) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: another file system is mounted there", package_or_app_id);
    ok = FALSE;
  } else
    ok = remove_at (fd, ".cache", dev, error);
  close (fd);
  return ok;
}

/* ---- dpkg ---- */

GHashTable *
vs_dpkg_states (const VsDirs *dirs, GError **error)
{
  g_autofree char *path = g_build_filename (dirs->dpkg_admin, "status", NULL);
  g_autofree char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, error))
    return NULL;
  GHashTable *states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) paragraphs = g_regex_split_simple ("\n\\s*\n", text, 0, 0);
  for (guint i = 0; paragraphs[i]; i++) {
    g_autofree char *name = NULL;
    const char *status = NULL;
    g_auto (GStrv) lines = g_strsplit (paragraphs[i], "\n", -1);
    for (guint j = 0; lines[j]; j++) {
      if (g_str_has_prefix (lines[j], "Package: "))
        name = g_strstrip (g_strdup (lines[j] + 9));
      else if (g_str_has_prefix (lines[j], "Status: "))
        status = lines[j] + 8;
    }
    if (!name || !status)
      continue;
    /* "want flag status": the last word */
    g_auto (GStrv) words = g_strsplit (g_strstrip ((char *) status), " ", -1);
    guint n = g_strv_length (words);
    const char *word = n ? words[n - 1] : "";
    VsDpkgState state = !strcmp (word, "installed") ? VS_DPKG_INSTALLED
                        : !strcmp (word, "not-installed") || !strcmp (word, "config-files") ? VS_DPKG_GONE
                        : VS_DPKG_CHANGING;
    /* one package for several architectures: the furthest along wins (installed, changing, gone) */
    gpointer before = NULL;
    if (g_hash_table_lookup_extended (states, name, NULL, &before)) {
      VsDpkgState b = GPOINTER_TO_INT (before);
      if (b == VS_DPKG_INSTALLED || (b == VS_DPKG_CHANGING && state == VS_DPKG_GONE))
        continue;
    }
    g_hash_table_replace (states, g_steal_pointer (&name), GINT_TO_POINTER (state));
  }
  return states;
}

VsDpkgState
vs_dpkg_state (GHashTable *states, const char *name)
{
  gpointer value = NULL;
  if (!states || !g_hash_table_lookup_extended (states, name, NULL, &value))
    return VS_DPKG_GONE;
  return GPOINTER_TO_INT (value);
}

static void
dpkg_package_free (VsDpkgPackage *p)
{
  g_free (p->name);
  g_free (p->version);
  g_free (p->arch);
  g_free (p);
}

GHashTable *
vs_dpkg_installed (const VsDirs *dirs, GError **error)
{
  g_autofree char *path = g_build_filename (dirs->dpkg_admin, "status", NULL);
  g_autofree char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, error))
    return NULL;
  GHashTable *installed = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) dpkg_package_free);
  g_auto (GStrv) paragraphs = g_regex_split_simple ("\n\\s*\n", text, 0, 0);
  for (guint i = 0; paragraphs[i]; i++) {
    g_autofree char *name = NULL, *version = NULL, *arch = NULL, *status = NULL;
    g_auto (GStrv) lines = g_strsplit (paragraphs[i], "\n", -1);
    for (guint j = 0; lines[j]; j++) {
      const char *l = lines[j];
      if (g_str_has_prefix (l, "Package: "))
        name = g_strstrip (g_strdup (l + 9));
      else if (g_str_has_prefix (l, "Version: "))
        version = g_strstrip (g_strdup (l + 9));
      else if (g_str_has_prefix (l, "Architecture: "))
        arch = g_strstrip (g_strdup (l + 14));
      else if (g_str_has_prefix (l, "Status: "))
        status = g_strstrip (g_strdup (l + 8));
    }
    if (!name || !status || !g_str_has_suffix (status, " installed") || g_hash_table_contains (installed, name))
      continue;
    VsDpkgPackage *p = g_new0 (VsDpkgPackage, 1);
    p->name = g_steal_pointer (&name);
    p->version = version ? g_steal_pointer (&version) : g_strdup ("");
    p->arch = arch ? g_steal_pointer (&arch) : g_strdup ("all");
    g_hash_table_insert (installed, p->name, p);
  }
  return installed;
}

char **
vs_dpkg_files (const VsDirs *dirs, const VsDpkgPackage *package)
{
  /* a Multi-Arch: same package's list has its architecture in the name */
  g_autofree char *with_arch = g_strdup_printf ("%s/info/%s:%s.list", dirs->dpkg_admin, package->name, package->arch);
  g_autofree char *plain = g_strdup_printf ("%s/info/%s.list", dirs->dpkg_admin, package->name);
  g_autofree char *text = NULL;
  if (!g_file_get_contents (with_arch, &text, NULL, NULL) && !g_file_get_contents (plain, &text, NULL, NULL))
    return g_new0 (char *, 1);
  return g_strsplit_set (g_strstrip (text), "\n", -1);
}

/* ---- names ---- */

static const char *const system_packages[] = {
  "vibeos", "gnome-shell", "gnome-session", "gnome-settings-daemon", "mutter", "gdm3", "xdg-desktop-portal",
  "xdg-desktop-portal-gnome", "xdg-desktop-portal-gtk", "pipewire", "wireplumber", "modemmanager",
  "network-manager", "evolution-data-server", "callaudiod", "feedbackd", "packagekit", NULL,
};

gboolean
vs_package_is_system (const char *name)
{
  return g_str_has_prefix (name, "vibeos-") || g_strv_contains (system_packages, name);
}

char *
vs_app_id_for (const char *desktop_id, gboolean *renamed)
{
  g_autofree char *id = g_str_has_suffix (desktop_id, ".desktop") ? g_strndup (desktop_id, strlen (desktop_id) - 8)
                                                                  : g_strdup (desktop_id);
  gboolean ok = vs_is_valid_app_id (id) && vs_is_valid_bus_name (id);
  if (renamed)
    *renamed = !ok;
  if (ok)
    return g_steal_pointer (&id);
  /* one element under vibeos.app: letters, digits, _ and - */
  GString *s = g_string_new ("vibeos.app.");
  if (g_ascii_isdigit (id[0]))
    g_string_append_c (s, '_');
  for (const char *c = id; *c; c++)
    g_string_append_c (s, g_ascii_isalnum (*c) || *c == '-' ? *c : '_');
  return g_string_free (s, FALSE);
}

/* ---- what a package has ---- */

gboolean
vs_app_declares (const VsApp *app)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  return app->source && g_key_file_load_from_file (k, app->source, G_KEY_FILE_NONE, NULL)
         && g_key_file_has_key (k, G_KEY_FILE_DESKTOP_GROUP, VS_REQUESTS_KEY, NULL);
}

guint
vs_app_requests (const VsApp *app)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (!app->source || !g_key_file_load_from_file (k, app->source, G_KEY_FILE_NONE, NULL)
      || !g_key_file_has_key (k, G_KEY_FILE_DESKTOP_GROUP, VS_REQUESTS_KEY, NULL))
    return VS_REQUESTS_ALL;
  g_auto (GStrv) names = g_key_file_get_string_list (k, G_KEY_FILE_DESKTOP_GROUP, VS_REQUESTS_KEY, NULL, NULL);
  guint mask = 0;
  for (guint i = 0; names && names[i]; i++) {
    int perm = vs_perm_from_name (g_strstrip (names[i]));
    if (perm >= 0)
      mask |= 1u << perm;
  }
  return mask;
}

static void
app_free (VsApp *app)
{
  g_free (app->desktop_id);
  g_free (app->app_id);
  g_free (app->source);
  g_free (app);
}

static VsPackage *
package_new (const char *name)
{
  VsPackage *p = g_new0 (VsPackage, 1);
  p->name = g_strdup (name);
  p->apps = g_ptr_array_new_with_free_func ((GDestroyNotify) app_free);
  p->services = g_ptr_array_new_with_free_func (g_free);
  p->autostart = g_ptr_array_new_with_free_func (g_free);
  p->units = g_ptr_array_new_with_free_func (g_free);
  p->bus_names = g_ptr_array_new_with_free_func (g_free);
  p->written = g_ptr_array_new_with_free_func (g_free);
  p->masked = g_ptr_array_new_with_free_func (g_free);
  return p;
}

void
vs_package_free (VsPackage *p)
{
  if (!p)
    return;
  g_free (p->name);
  g_free (p->version);
  g_free (p->origin);
  g_ptr_array_unref (p->apps);
  g_ptr_array_unref (p->services);
  g_ptr_array_unref (p->autostart);
  g_ptr_array_unref (p->units);
  g_ptr_array_unref (p->bus_names);
  g_ptr_array_unref (p->written);
  g_ptr_array_unref (p->masked);
  g_free (p);
}

static gboolean
str_array_has (GPtrArray *a, const char *s)
{
  for (guint i = 0; i < a->len; i++)
    if (!g_strcmp0 (a->pdata[i], s))
      return TRUE;
  return FALSE;
}

static const char *const unit_dirs[] = { "/usr/lib/systemd/user/", "/lib/systemd/user/", NULL };
static const char *const unit_suffixes[] = { ".service", ".socket", ".timer", ".path", NULL };

VsPackage *
vs_package_scan (const VsDirs *dirs, const VsDpkgPackage *installed, GError **error)
{
  if (!installed) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not installed");
    return NULL;
  }
  g_autoptr (VsPackage) p = package_new (installed->name);
  p->version = g_strdup (installed->version);
  g_auto (GStrv) files = vs_dpkg_files (dirs, installed);

  for (guint i = 0; files[i]; i++) {
    const char *f = files[i];
    g_autofree char *path = sys_path (dirs, f);
    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
      continue;
    if (g_str_has_prefix (f, "/usr/share/applications/") && g_str_has_suffix (f, ".desktop")) {
      g_autoptr (GKeyFile) k = g_key_file_new ();
      if (!g_key_file_load_from_file (k, path, G_KEY_FILE_NONE, NULL))
        continue;
      g_autofree char *type = g_key_file_get_string (k, G_KEY_FILE_DESKTOP_GROUP, "Type", NULL);
      if (g_strcmp0 (type, "Application"))
        continue;
      VsApp *app = g_new0 (VsApp, 1);
      /* applications/kde/foo.desktop is kde-foo.desktop */
      app->desktop_id = g_strdelimit (g_strdup (f + strlen ("/usr/share/applications/")), "/", '-');
      app->app_id = vs_app_id_for (app->desktop_id, &app->renamed);
      app->source = g_steal_pointer (&path);
      g_ptr_array_add (p->apps, app);
    } else if (g_str_has_prefix (f, "/usr/share/dbus-1/services/") && g_str_has_suffix (f, ".service")) {
      g_autoptr (GKeyFile) k = g_key_file_new ();
      if (!g_key_file_load_from_file (k, path, G_KEY_FILE_NONE, NULL))
        continue;
      g_autofree char *name = g_key_file_get_string (k, "D-BUS Service", "Name", NULL);
      if (!name || !vs_is_valid_bus_name (name))
        continue;
      g_ptr_array_add (p->services, g_strdup (f));
      if (!str_array_has (p->bus_names, name))
        g_ptr_array_add (p->bus_names, g_steal_pointer (&name));
    } else if (g_str_has_prefix (f, "/etc/xdg/autostart/") && g_str_has_suffix (f, ".desktop")) {
      g_ptr_array_add (p->autostart, g_strdup (f));
    } else {
      for (guint d = 0; unit_dirs[d]; d++) {
        if (!g_str_has_prefix (f, unit_dirs[d]) || strchr (f + strlen (unit_dirs[d]), '/'))
          continue;
        for (guint s = 0; unit_suffixes[s]; s++)
          if (g_str_has_suffix (f, unit_suffixes[s]) && !str_array_has (p->units, f + strlen (unit_dirs[d])))
            g_ptr_array_add (p->units, g_strdup (f + strlen (unit_dirs[d])));
      }
    }
  }
  if (!p->apps->len && !p->services->len)
    return NULL;
  return g_steal_pointer (&p);
}

/* ---- the records ---- */

static char *
record_path (const VsDirs *dirs, const char *name)
{
  g_autofree char *dir = registry_dir (dirs);
  return g_build_filename (dir, name, NULL);
}

static void
strv_into (GPtrArray *a, char **v)
{
  for (guint i = 0; v && v[i]; i++)
    g_ptr_array_add (a, g_strdup (v[i]));
}

static void
set_list (GKeyFile *k, const char *group, const char *key, GPtrArray *a)
{
  g_key_file_set_string_list (k, group, key, (const char *const *) a->pdata, a->len);
}

VsPackage *
vs_registry_load (const VsDirs *dirs, const char *name)
{
  g_autofree char *path = record_path (dirs, name);
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (!g_key_file_load_from_file (k, path, G_KEY_FILE_NONE, NULL))
    return NULL;
  VsPackage *p = package_new (name);
  p->version = g_key_file_get_string (k, "Package", "Version", NULL);
  p->origin = g_key_file_get_string (k, "Package", "Origin", NULL);
  g_auto (GStrv) apps = g_key_file_get_string_list (k, "Package", "Apps", NULL, NULL);
  for (guint i = 0; apps && apps[i]; i++) {
    g_autofree char *group = g_strconcat ("App ", apps[i], NULL);
    VsApp *app = g_new0 (VsApp, 1);
    app->app_id = g_strdup (apps[i]);
    app->desktop_id = g_key_file_get_string (k, group, "DesktopId", NULL);
    app->renamed = g_key_file_get_boolean (k, group, "Renamed", NULL);
    app->source = g_key_file_get_string (k, group, "Source", NULL);
    if (!app->desktop_id)
      app->desktop_id = g_strconcat (apps[i], ".desktop", NULL);
    g_ptr_array_add (p->apps, app);
  }
  g_auto (GStrv) services = g_key_file_get_string_list (k, "Package", "Services", NULL, NULL);
  g_auto (GStrv) autostart = g_key_file_get_string_list (k, "Package", "Autostart", NULL, NULL);
  g_auto (GStrv) units = g_key_file_get_string_list (k, "Package", "Units", NULL, NULL);
  g_auto (GStrv) names = g_key_file_get_string_list (k, "Package", "BusNames", NULL, NULL);
  g_auto (GStrv) written = g_key_file_get_string_list (k, "Package", "Files", NULL, NULL);
  g_auto (GStrv) masked = g_key_file_get_string_list (k, "Package", "Masked", NULL, NULL);
  strv_into (p->services, services);
  strv_into (p->autostart, autostart);
  strv_into (p->units, units);
  strv_into (p->bus_names, names);
  strv_into (p->written, written);
  strv_into (p->masked, masked);
  return p;
}

static gint
compare_names (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char **) a, *(char **) b);
}

GPtrArray *
vs_registry_list (const VsDirs *dirs)
{
  g_autofree char *dir = registry_dir (dirs);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    if (name[0] != '.' && !g_str_has_suffix (name, ".tmp"))
      g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort (names, compare_names);
  return names;
}

VsPackage *
vs_registry_find_app (const VsDirs *dirs, const char *app_id)
{
  g_autoptr (GPtrArray) names = vs_registry_list (dirs);
  for (guint i = 0; i < names->len; i++) {
    g_autoptr (VsPackage) p = vs_registry_load (dirs, names->pdata[i]);
    for (guint j = 0; p && j < p->apps->len; j++)
      if (!strcmp (((VsApp *) p->apps->pdata[j])->app_id, app_id))
        return g_steal_pointer (&p);
  }
  return NULL;
}

static gboolean
save_record (const VsDirs *dirs, VsPackage *p, GError **error)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  g_autoptr (GPtrArray) apps = g_ptr_array_new ();
  g_key_file_set_string (k, "Package", "Version", p->version ? p->version : "");
  g_key_file_set_string (k, "Package", "Origin", p->origin ? p->origin : "store");
  for (guint i = 0; i < p->apps->len; i++)
    g_ptr_array_add (apps, ((VsApp *) p->apps->pdata[i])->app_id);
  set_list (k, "Package", "Apps", apps);
  set_list (k, "Package", "Services", p->services);
  set_list (k, "Package", "Autostart", p->autostart);
  set_list (k, "Package", "Units", p->units);
  set_list (k, "Package", "BusNames", p->bus_names);
  set_list (k, "Package", "Files", p->written);
  set_list (k, "Package", "Masked", p->masked);
  for (guint i = 0; i < p->apps->len; i++) {
    VsApp *app = p->apps->pdata[i];
    g_autofree char *group = g_strconcat ("App ", app->app_id, NULL);
    g_key_file_set_string (k, group, "DesktopId", app->desktop_id);
    g_key_file_set_boolean (k, group, "Renamed", app->renamed);
    if (app->source)
      g_key_file_set_string (k, group, "Source", app->source);
  }
  g_autofree char *dir = registry_dir (dirs);
  g_autofree char *path = record_path (dirs, p->name);
  if (g_mkdir_with_parents (dir, 0700) < 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", dir, g_strerror (errno));
    return FALSE;
  }
  return g_key_file_save_to_file (k, path, error);
}

/* ---- the files on the user's side ---- */

static gboolean
is_ours (const char *path)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    return FALSE;
  if (!g_key_file_load_from_file (k, path, G_KEY_FILE_NONE, NULL))
    return FALSE;
  g_auto (GStrv) groups = g_key_file_get_groups (k, NULL);
  for (guint i = 0; groups && groups[i]; i++)
    if (g_key_file_has_key (k, groups[i], VS_MARK_KEY, NULL))
      return TRUE;
  return FALSE;
}

/* a file of the user's own where ours goes: kept beside it */
static void
save_users_file (const char *path)
{
  if (!g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    return;
  if (is_ours (path))
    return;
  g_autofree char *saved = g_strconcat (path, SAVED_SUFFIX, NULL);
  if (g_file_test (saved, G_FILE_TEST_EXISTS))
    return;
  if (g_rename (path, saved) < 0)
    g_warning ("%s: %s", path, g_strerror (errno));
}

static void
restore_users_file (const char *path)
{
  g_autofree char *saved = g_strconcat (path, SAVED_SUFFIX, NULL);
  if ((g_file_test (saved, G_FILE_TEST_EXISTS) || g_file_test (saved, G_FILE_TEST_IS_SYMLINK))
      && !g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    g_rename (saved, path);
}

/* writes `data` to `path` unless it is there already; TRUE when something changed */
static gboolean
write_file (const char *path, const char *data, gboolean *changed, GError **error)
{
  g_autofree char *old = NULL;
  if (g_file_get_contents (path, &old, NULL, NULL) && !g_strcmp0 (old, data) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    return TRUE;
  save_users_file (path);
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);
  if (!g_file_set_contents_full (path, data, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error))
    return FALSE;
  *changed = TRUE;
  return TRUE;
}

static void
mark (GKeyFile *k, const char *group, const char *app_id)
{
  g_key_file_set_string (k, group, VS_MARK_KEY, app_id);
}

/* the app's desktop file, its Exec lines through the sandbox */
/* `shown`: the copy the launcher shows; else the one under the app's own id when it shows under another, which
 * still opens the files it is the default app for (and what asks for it by its own id), unseen */
static char *
wrap_desktop (const VsDirs *dirs, const VsApp *app, gboolean shown, GError **error)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (!g_key_file_load_from_file (k, app->source, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, error))
    return NULL;
  g_auto (GStrv) groups = g_key_file_get_groups (k, NULL);
  gboolean has_exec = FALSE;
  for (guint i = 0; groups[i]; i++) {
    if (strcmp (groups[i], G_KEY_FILE_DESKTOP_GROUP) && !g_str_has_prefix (groups[i], "Desktop Action "))
      continue;
    g_autofree char *exec = g_key_file_get_string (k, groups[i], "Exec", NULL);
    if (!exec)
      continue;
    g_autofree char *wrapped = vs_exec_wrap (exec, dirs->launcher, app->app_id, TRUE, error);
    if (!wrapped)
      return NULL;
    g_key_file_set_string (k, groups[i], "Exec", wrapped);
    if (!strcmp (groups[i], G_KEY_FILE_DESKTOP_GROUP))
      has_exec = TRUE;
  }
  /* GIO starts it with the Exec line, which forwards the files it opens; with no Exec line the D-Bus service
   * starts it, which is sandboxed too */
  if (has_exec)
    g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "DBusActivatable", NULL);
  g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL);
  g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "Hidden", NULL);
  if (!shown)
    g_key_file_set_boolean (k, G_KEY_FILE_DESKTOP_GROUP, "NoDisplay", TRUE);
  else if (app->renamed)
    /* the files it opens go to the copy under its own id, which the defaults name: not two apps for them */
    g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "MimeType", NULL);
  mark (k, G_KEY_FILE_DESKTOP_GROUP, app->app_id);
  g_key_file_set_string (k, G_KEY_FILE_DESKTOP_GROUP, VS_SOURCE_KEY, app->source);
  return g_key_file_to_data (k, NULL, NULL);
}

static const char *
app_for_file (VsPackage *p, const char *basename)
{
  for (guint i = 0; i < p->apps->len; i++) {
    VsApp *app = p->apps->pdata[i];
    if (!g_strcmp0 (app->desktop_id, basename))
      return app->app_id;
  }
  return p->apps->len ? ((VsApp *) p->apps->pdata[0])->app_id : NULL;
}

static char *
service_app_id (VsPackage *p, const char *bus_name)
{
  /* a service of one of its apps is that app; any other runs as the package's first app, or as itself */
  for (guint i = 0; i < p->apps->len; i++) {
    VsApp *app = p->apps->pdata[i];
    if (!strcmp (app->app_id, bus_name) || g_str_has_prefix (bus_name, app->app_id))
      return g_strdup (app->app_id);
  }
  if (p->apps->len)
    return g_strdup (((VsApp *) p->apps->pdata[0])->app_id);
  return vs_app_id_for (bus_name, NULL);
}

static char *
wrap_service (const VsDirs *dirs, VsPackage *p, const char *path, GError **error)
{
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (!g_key_file_load_from_file (k, path, G_KEY_FILE_NONE, error))
    return NULL;
  g_autofree char *name = g_key_file_get_string (k, "D-BUS Service", "Name", error);
  g_autofree char *exec = name ? g_key_file_get_string (k, "D-BUS Service", "Exec", error) : NULL;
  if (!exec)
    return NULL;
  g_autofree char *app_id = service_app_id (p, name);
  g_autofree char *wrapped = vs_exec_wrap (exec, dirs->launcher, app_id, FALSE, error);
  if (!wrapped)
    return NULL;
  /* the bus starts the sandbox itself: not systemd's unit, which is masked */
  g_autoptr (GKeyFile) out = g_key_file_new ();
  g_key_file_set_string (out, "D-BUS Service", "Name", name);
  g_key_file_set_string (out, "D-BUS Service", "Exec", wrapped);
  mark (out, "D-BUS Service", app_id);
  return g_key_file_to_data (out, NULL, NULL);
}

static char *
wrap_autostart (const VsDirs *dirs, VsPackage *p, const char *path, gboolean run, GError **error)
{
  g_autofree char *base = g_path_get_basename (path);
  const char *app_id = app_for_file (p, base);
  g_autofree char *fallback = app_id ? NULL : vs_app_id_for (base, NULL);
  if (!app_id)
    app_id = fallback;
  g_autoptr (GKeyFile) k = g_key_file_new ();
  if (!run || !g_key_file_load_from_file (k, path, G_KEY_FILE_KEEP_TRANSLATIONS, NULL)) {
    /* it does not start with the session */
    g_autoptr (GKeyFile) hidden = g_key_file_new ();
    g_key_file_set_string (hidden, G_KEY_FILE_DESKTOP_GROUP, "Type", "Application");
    g_key_file_set_string (hidden, G_KEY_FILE_DESKTOP_GROUP, "Name", app_id);
    g_key_file_set_boolean (hidden, G_KEY_FILE_DESKTOP_GROUP, "Hidden", TRUE);
    mark (hidden, G_KEY_FILE_DESKTOP_GROUP, app_id);
    return g_key_file_to_data (hidden, NULL, NULL);
  }
  g_autofree char *exec = g_key_file_get_string (k, G_KEY_FILE_DESKTOP_GROUP, "Exec", error);
  if (!exec)
    return NULL;
  g_autofree char *wrapped = vs_exec_wrap (exec, dirs->launcher, app_id, FALSE, error);
  if (!wrapped)
    return NULL;
  g_key_file_set_string (k, G_KEY_FILE_DESKTOP_GROUP, "Exec", wrapped);
  g_key_file_remove_key (k, G_KEY_FILE_DESKTOP_GROUP, "DBusActivatable", NULL);
  mark (k, G_KEY_FILE_DESKTOP_GROUP, app_id);
  return g_key_file_to_data (k, NULL, NULL);
}

static gboolean
mask_unit (const char *path, gboolean *changed, GError **error)
{
  g_autofree char *target = g_file_read_link (path, NULL);
  if (!g_strcmp0 (target, "/dev/null"))
    return TRUE;
  save_users_file (path);
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);
  if (symlink ("/dev/null", path) < 0) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", path, g_strerror (errno));
    return FALSE;
  }
  *changed = TRUE;
  return TRUE;
}

/* takes away a file written before, if it is still ours, and puts back the user's own */
static gboolean
unwrite (const char *path)
{
  gboolean gone = FALSE;
  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK)) {
    g_autofree char *target = g_file_read_link (path, NULL);
    if (!g_strcmp0 (target, "/dev/null"))
      gone = g_unlink (path) == 0;
  } else if (is_ours (path)) {
    gone = g_unlink (path) == 0;
  }
  restore_users_file (path);
  return gone;
}

static gboolean
is_service_path (const VsDirs *dirs, const char *path)
{
  g_autofree char *dir = g_build_filename (dirs->data_home, "dbus-1", "services", NULL);
  return g_str_has_prefix (path, dir);
}

gboolean
vs_registry_write (const VsDirs *dirs, VsPackage *p, gboolean autostart, VsChanges *changes, GError **error)
{
  VsChanges dummy = { 0 };
  if (!changes)
    changes = &dummy;
  g_autoptr (VsPackage) before = vs_registry_load (dirs, p->name);
  g_autoptr (GPtrArray) written = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) masked = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *apps_dir = g_build_filename (dirs->data_home, "applications", NULL);
  g_autofree char *services_dir = g_build_filename (dirs->data_home, "dbus-1", "services", NULL);
  g_autofree char *autostart_dir = g_build_filename (dirs->config_home, "autostart", NULL);
  g_autofree char *units_dir = g_build_filename (dirs->config_home, "systemd", "user", NULL);

  if (!p->origin && before)
    p->origin = g_strdup (before->origin);

  for (guint i = 0; i < p->apps->len; i++) {
    VsApp *app = p->apps->pdata[i];
    g_autofree char *data = wrap_desktop (dirs, app, TRUE, error);
    if (!data)
      return FALSE;
    g_autofree char *name = g_strconcat (app->app_id, ".desktop", NULL);
    g_autofree char *path = g_build_filename (apps_dir, name, NULL);
    if (!write_file (path, data, &changes->apps, error))
      return FALSE;
    g_ptr_array_add (written, g_steal_pointer (&path));
    if (app->renamed) {
      g_autofree char *unseen = wrap_desktop (dirs, app, FALSE, error);
      g_autofree char *own = g_build_filename (apps_dir, app->desktop_id, NULL);
      if (!unseen || !write_file (own, unseen, &changes->apps, error))
        return FALSE;
      g_ptr_array_add (written, g_steal_pointer (&own));
    }
  }
  for (guint i = 0; i < p->services->len; i++) {
    const char *f = p->services->pdata[i];
    g_autofree char *src = sys_path (dirs, f);
    g_autofree char *data = wrap_service (dirs, p, src, error);
    if (!data)
      return FALSE;
    g_autofree char *base = g_path_get_basename (f);
    g_autofree char *path = g_build_filename (services_dir, base, NULL);
    if (!write_file (path, data, &changes->services, error))
      return FALSE;
    g_ptr_array_add (written, g_steal_pointer (&path));
  }
  for (guint i = 0; i < p->autostart->len; i++) {
    const char *f = p->autostart->pdata[i];
    g_autofree char *src = sys_path (dirs, f);
    g_autofree char *data = wrap_autostart (dirs, p, src, autostart, error);
    if (!data)
      return FALSE;
    g_autofree char *base = g_path_get_basename (f);
    g_autofree char *path = g_build_filename (autostart_dir, base, NULL);
    gboolean ignored = FALSE;
    if (!write_file (path, data, &ignored, error))
      return FALSE;
    g_ptr_array_add (written, g_steal_pointer (&path));
  }
  for (guint i = 0; i < p->units->len; i++) {
    g_autofree char *path = g_build_filename (units_dir, p->units->pdata[i], NULL);
    if (!mask_unit (path, &changes->units, error))
      return FALSE;
    g_ptr_array_add (masked, g_steal_pointer (&path));
  }

  /* what the version before had and this one has not */
  for (guint i = 0; before && i < before->written->len; i++) {
    const char *old = before->written->pdata[i];
    if (str_array_has (written, old) || !unwrite (old))
      continue;
    if (is_service_path (dirs, old))
      changes->services = TRUE;
    else
      changes->apps = TRUE;
  }
  for (guint i = 0; before && i < before->masked->len; i++)
    if (!str_array_has (masked, before->masked->pdata[i]) && unwrite (before->masked->pdata[i]))
      changes->units = TRUE;

  g_ptr_array_set_size (p->written, 0);
  g_ptr_array_extend_and_steal (p->written, g_steal_pointer (&written));
  g_ptr_array_set_size (p->masked, 0);
  g_ptr_array_extend_and_steal (p->masked, g_steal_pointer (&masked));
  return save_record (dirs, p, error);
}

gboolean
vs_registry_remove (const VsDirs *dirs, const char *name, VsChanges *changes, GError **error)
{
  VsChanges dummy = { 0 };
  if (!changes)
    changes = &dummy;
  g_autoptr (VsPackage) p = vs_registry_load (dirs, name);
  if (!p) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is not sandboxed", name);
    return FALSE;
  }
  for (guint i = 0; i < p->written->len; i++) {
    const char *f = p->written->pdata[i];
    if (!unwrite (f))
      continue;
    if (is_service_path (dirs, f))
      changes->services = TRUE;
    else
      changes->apps = TRUE;
  }
  for (guint i = 0; i < p->masked->len; i++)
    if (unwrite (p->masked->pdata[i]))
      changes->units = TRUE;
  g_autofree char *path = record_path (dirs, name);
  if (g_unlink (path) < 0 && errno != ENOENT) {
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", path, g_strerror (errno));
    return FALSE;
  }
  return TRUE;
}

/* ---- PackageKit's history ---- */

char **
vs_history_installed (const VsDirs *dirs, const char *client, GError **error)
{
  sqlite3 *db = NULL;
  g_autofree char *uri = g_strconcat ("file:", dirs->packagekit_db, "?mode=ro", NULL);
  if (sqlite3_open_v2 (uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", dirs->packagekit_db,
                 db ? sqlite3_errmsg (db) : "cannot open");
    sqlite3_close (db);
    return NULL;
  }
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT data FROM transactions WHERE succeeded = 1 AND role IN ('install-packages', "
                    "'install-files') AND instr(cmdline, ?1) > 0 ORDER BY timespec";
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", dirs->packagekit_db, sqlite3_errmsg (db));
    sqlite3_close (db);
    return NULL;
  }
  sqlite3_bind_text (st, 1, client, -1, SQLITE_TRANSIENT);
  GPtrArray *names = g_ptr_array_new ();
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *data = (const char *) sqlite3_column_text (st, 0);
    /* a line per step of each package: what happened (downloading, preparing, decompressing, installing…), its id
     * (name;version;arch;repo), its summary. A package put on the system is unpacked (preparing, decompressing)
     * and then installing; one whose triggers ran is only "installing", one updated is "updating" */
    g_autoptr (GHashTable) unpacked = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr (GHashTable) replaced = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr (GPtrArray) installing = g_ptr_array_new_with_free_func (g_free);
    g_auto (GStrv) lines = g_strsplit (data ? data : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
      g_auto (GStrv) fields = g_strsplit (lines[i], "\t", 3);
      if (!fields[0] || !fields[1])
        continue;
      g_autofree char *name = g_strndup (fields[1], strcspn (fields[1], ";"));
      if (!*name)
        continue;
      const char *what = fields[0];
      if (!strcmp (what, "preparing") || !strcmp (what, "decompressing"))
        g_hash_table_add (unpacked, g_strdup (name));
      else if (!strcmp (what, "updating") || !strcmp (what, "downgrading") || !strcmp (what, "reinstalling"))
        g_hash_table_add (replaced, g_strdup (name));
      else if (!strcmp (what, "installing") && !str_array_has (installing, name))
        g_ptr_array_add (installing, g_strdup (name));
    }
    for (guint i = 0; i < installing->len; i++) {
      const char *name = installing->pdata[i];
      if (g_hash_table_contains (unpacked, name) && !g_hash_table_contains (replaced, name)
          && !str_array_has (names, name))
        g_ptr_array_add (names, g_strdup (name));
    }
  }
  sqlite3_finalize (st);
  sqlite3_close (db);
  g_ptr_array_add (names, NULL);
  return (char **) g_ptr_array_free (names, FALSE);
}
