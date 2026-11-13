/* gnome-phone-migrate-ids: carries one user's settings, files and home screen over from the IDs gnome-phone had up to
 * 0.1.6 (legacy-ids.h) to the neo.* ones. It runs once per user, early in the session
 * (gnome-phone-migrate-ids.service, before graphical-session-pre.target; neo-launcher also runs it when it starts
 * and finds no stamp), and then marks the user done with $XDG_STATE_HOME/gnome-phone/ids-migrated, which lists what
 * it did. Two runs at once take turns (a lock beside the stamp), and the second finds the stamp.
 *
 * It never overwrites what the user already has under a new ID, and it leaves every old setting where it was:
 *  - GSettings: the keys under each old path that the new path does not have yet are copied across. The old schemas
 *    are gone with the old packages, so GSettings cannot read them: dconf dump and dconf load do. The desktop ids in
 *    hidden-from-home are renamed on the way. A device's own defaults for the old schemas (vendor overrides, which
 *    no longer apply) become the user's values where they differ from the new schema's and the user has none.
 *  - per-app directories ($XDG_DATA_HOME, $XDG_CONFIG_HOME, $XDG_CACHE_HOME, $XDG_STATE_HOME/<app id>: the clock's
 *    alarms, the browser's profile, the calculator's history) move to the new name, and the old name stays as a
 *    symlink to it, so a program from before the upgrade that is still running keeps finding its files. When the
 *    new directory exists already, only the files it lacks are copied into it.
 *  - the browser's home screen shortcuts (desktop files in ~/.local/share/applications) take the new name, with the
 *    new icon name, icon path and window class.
 *  - default and associated apps (mimeapps.list): the new desktop id goes in front of each old one.
 *  - GNOME Shell's favourites get the new desktop id beside the old one. enabled-extensions and
 *    disabled-extensions name the extension's new UUID in place of the old one: a copy of the old extension left in
 *    a user's own extensions directory would otherwise run beside the new one.
 *  - per-app notification settings (org.gnome.desktop.notifications.application) are copied to the new id, which
 *    joins application-children.
 * The home layout is the launcher's: it reads old desktop ids in any layout.json (a ROM's in /etc/xdg too) and in
 * hidden-from-home as the new ones (neo-launcher/src/apps.c). What a 0.1.6 launcher made of the user's layout while
 * the packages were upgraded is undone here, from the copy saved before (restore_layout). */
#include "legacy-ids.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static GString  *report;   /* what was done, a line each: printed for the journal and kept in the stamp */
static gboolean  failed;   /* something to try again at the next start: no stamp this time */

static void note (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

static void
note (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  g_string_append_vprintf (report, fmt, ap);
  va_end (ap);
  g_string_append_c (report, '\n');
}

/* a path for the report, with ~ for the home directory */
static const char *
home (const char *path)
{
  static GPtrArray *keep;
  const char *h = g_get_home_dir ();
  if (!g_str_has_prefix (path, h) || path[strlen (h)] != '/')
    return path;
  if (!keep)
    keep = g_ptr_array_new_with_free_func (g_free);
  char *s = g_strconcat ("~", path + strlen (h), NULL);
  g_ptr_array_add (keep, s);
  return s;
}

static gboolean
ptr_has (GPtrArray *list, const char *s)
{
  for (guint i = 0; i < list->len; i++)
    if (!g_strcmp0 (list->pdata[i], s))
      return TRUE;
  return FALSE;
}

/* ---- per-app directories ---------------------------------------------------------------------------------------- */

/* the entries of `from` that `to` does not have, copied in (directories recursively); what `to` has stays */
static gboolean
copy_missing (GFile *from, GFile *to, GError **error)
{
  g_autoptr (GFileEnumerator) e = g_file_enumerate_children (from, G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                             G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!e)
    return FALSE;
  for (;;) {
    GFileInfo *next = NULL;
    if (!g_file_enumerator_iterate (e, &next, NULL, NULL, error))
      return FALSE;
    if (!next)
      return TRUE;
    g_autoptr (GFile) src = g_file_get_child (from, g_file_info_get_name (next));
    g_autoptr (GFile) dst = g_file_get_child (to, g_file_info_get_name (next));
    GFileType type = g_file_info_get_file_type (next);
    GFileType have = g_file_query_file_type (dst, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
      if (have == G_FILE_TYPE_UNKNOWN && !g_file_make_directory (dst, NULL, error))
        return FALSE;
      if ((have == G_FILE_TYPE_UNKNOWN || have == G_FILE_TYPE_DIRECTORY) && !copy_missing (src, dst, error))
        return FALSE;
    } else if ((type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_SYMBOLIC_LINK) && have == G_FILE_TYPE_UNKNOWN) {
      if (!g_file_copy (src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, error))
        return FALSE;
    }
    /* sockets and pipes (a running program's) are not data: they are left */
  }
}

static void
move_dir (const char *base, const char *old_name, const char *new_name)
{
  g_autofree char *old = g_build_filename (base, old_name, NULL);
  g_autofree char *now = g_build_filename (base, new_name, NULL);
  GStatBuf st;

  /* nothing there, or the symlink an earlier run left */
  if (g_lstat (old, &st) || !S_ISDIR (st.st_mode))
    return;
  if (g_lstat (now, &st)) {
    if (g_rename (old, now)) {
      note ("could not move %s to %s: %s", home (old), home (now), g_strerror (errno));
      failed = TRUE;
    } else if (symlink (new_name, old)) {
      note ("moved %s to %s (no symlink in its place: %s)", home (old), home (now), g_strerror (errno));
    } else {
      note ("moved %s to %s", home (old), home (now));
    }
    return;
  }
  if (!S_ISDIR (st.st_mode)) {
    note ("%s is not a directory: %s left as it is", home (now), home (old));
    return;
  }
  g_autoptr (GFile) from = g_file_new_for_path (old);
  g_autoptr (GFile) to = g_file_new_for_path (now);
  g_autoptr (GError) error = NULL;
  if (copy_missing (from, to, &error)) {
    note ("%s existed: copied in what it lacked from %s", home (now), home (old));
  } else {
    note ("could not copy %s into %s: %s", home (old), home (now), error->message);
    failed = TRUE;
  }
}

static void
move_app_dirs (void)
{
  const char *bases[] = { g_get_user_data_dir (), g_get_user_config_dir (), g_get_user_cache_dir (),
                          g_get_user_state_dir () };
  for (guint i = 0; i < legacy_n_ids; i++) {
    if (!legacy_ids[i].data)
      continue;
    g_autofree char *old = g_strconcat (LEGACY_PREFIX, legacy_ids[i].old, NULL);
    for (guint b = 0; b < G_N_ELEMENTS (bases); b++)
      move_dir (bases[b], old, legacy_ids[i].now);
  }
}

/* ---- the home layout -------------------------------------------------------------------------------------------- */

/* A 0.1.6 launcher still running while the packages are upgraded watches every app's desktop file go and the renamed
 * one come, an app it has never seen: it empties the old one's cells, puts the new one on the first free cell (A→Z)
 * and saves that over the user's layout. The first renamed package to be unpacked saved each user's layout as it was
 * before (debian/layouts-before-0.1.7.sh), and it goes back in place here, before the new launcher reads it; the one
 * that launcher made stays beside it. */
static void
restore_layout (void)
{
  g_autofree char *saved = g_strconcat (SAVED_LAYOUTS, "/", g_get_user_name (), ".json", NULL);
  /* once: a run that left something else undone comes back at the next start, when the user may have moved icons */
  g_autofree char *mark = g_build_filename (g_get_user_state_dir (), "gnome-phone", "layout-restored", NULL);
  g_autofree char *before = NULL, *now = NULL;
  gsize len;
  if (g_file_test (mark, G_FILE_TEST_EXISTS) || !g_file_get_contents (saved, &before, &len, NULL))
    return;
  g_autofree char *file = g_build_filename (g_get_user_config_dir (), "neo-launcher", "layout.json", NULL);
  g_autofree char *aside = g_build_filename (g_get_user_config_dir (), "neo-launcher", "layout-upgrade-0.1.7.json",
                                             NULL);
  g_autoptr (GError) error = NULL;
  if (g_file_get_contents (file, &now, NULL, NULL) && !strcmp (now, before)) {
    /* upgraded with no launcher running: nothing to undo */
  } else if (now && g_rename (file, aside)) {
    note ("could not move %s aside: %s", home (file), g_strerror (errno));
    failed = TRUE;
    return;
  } else if (!g_file_set_contents (file, before, len, &error)) {
    note ("could not put the home layout back: %s", error->message);
    failed = TRUE;
    return;
  } else if (now) {
    note ("home layout: back as it was before the upgrade; the one the old launcher made during it is %s",
          home (aside));
  } else {
    note ("home layout: back as it was before the upgrade");
  }
  if (!g_file_set_contents (mark, "", 0, &error))
    g_printerr ("gnome-phone-migrate-ids: %s\n", error->message);
}

/* ---- the browser's shortcuts ------------------------------------------------------------------------------------ */

static char *
replace (const char *s, const char *from, const char *to)
{
  g_auto (GStrv) parts = g_strsplit (s, from, -1);
  return g_strjoinv (to, parts);
}

static void
move_shortcuts (void)
{
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "applications", NULL);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    if (g_str_has_prefix (name, LEGACY_SHORTCUT_PREFIX) && g_str_has_suffix (name, ".desktop"))
      g_ptr_array_add (names, g_strdup (name));
  for (guint i = 0; i < names->len; i++) {
    g_autofree char *new_name = legacy_desktop_id (names->pdata[i]);
    g_autofree char *old = g_build_filename (dir, names->pdata[i], NULL);
    g_autofree char *now = g_build_filename (dir, new_name, NULL);
    if (g_file_test (now, G_FILE_TEST_EXISTS)) {
      note ("%s exists: %s left as it is", home (now), home (old));
      continue;
    }
    g_autoptr (GKeyFile) kf = g_key_file_new ();
    g_autoptr (GError) error = NULL;
    if (!g_key_file_load_from_file (kf, old, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error)) {
      note ("could not read %s: %s", home (old), error->message);
      continue;
    }
    /* a shortcut without a favicon shows the browser's icon; one with a favicon has it in the browser's data
     * directory, which move_app_dirs renamed */
    g_autofree char *icon = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
    if (!g_strcmp0 (icon, LEGACY_PREFIX "Browser")) {
      g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, "neo.Browser");
    } else if (icon && strstr (icon, "/" LEGACY_PREFIX "NeoBrowser/")) {
      g_autofree char *path = replace (icon, "/" LEGACY_PREFIX "NeoBrowser/", "/neo.Browser/");
      g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, path);
    }
    g_autofree char *wm = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP,
                                                 G_KEY_FILE_DESKTOP_KEY_STARTUP_WM_CLASS, NULL);
    if (!g_strcmp0 (wm, LEGACY_PREFIX "NeoBrowser"))
      g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_STARTUP_WM_CLASS, "neo.Browser");
    if (!g_key_file_save_to_file (kf, now, &error)) {
      note ("could not write %s: %s", home (now), error->message);
      failed = TRUE;
      continue;
    }
    g_unlink (old);
    note ("browser shortcut %s is %s", (char *) names->pdata[i], new_name);
  }
}

/* ---- default apps ----------------------------------------------------------------------------------------------- */

/* "a;b;" with each old desktop id preceded by its new one (unless the list has that already); NULL: no change */
static char *
map_app_list (const char *value)
{
  g_auto (GStrv) items = g_strsplit (value, ";", -1);
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);
  gboolean changed = FALSE;

  for (char **i = items; *i; i++) {
    if (!**i)
      continue;
    g_autofree char *now = legacy_desktop_id (*i);
    if (now && !g_strv_contains ((const char *const *) items, now) && !ptr_has (out, now)) {
      g_ptr_array_add (out, g_steal_pointer (&now));
      changed = TRUE;
    }
    g_ptr_array_add (out, g_strdup (*i));
  }
  if (!changed)
    return NULL;
  g_ptr_array_add (out, NULL);
  g_autofree char *joined = g_strjoinv (";", (char **) out->pdata);
  return g_str_has_suffix (value, ";") ? g_strconcat (joined, ";", NULL) : g_steal_pointer (&joined);
}

static void
map_mimeapps (const char *file)
{
  static const char *const groups[] = { "Default Applications", "Added Associations", "Removed Associations" };
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, file, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
    return;
  guint n = 0;
  for (guint g = 0; g < G_N_ELEMENTS (groups); g++) {
    g_auto (GStrv) keys = g_key_file_get_keys (kf, groups[g], NULL, NULL);
    for (char **k = keys; k && *k; k++) {
      g_autofree char *value = g_key_file_get_value (kf, groups[g], *k, NULL);
      g_autofree char *mapped = value ? map_app_list (value) : NULL;
      if (mapped) {
        g_key_file_set_value (kf, groups[g], *k, mapped);
        n++;
      }
    }
  }
  if (!n)
    return;
  g_autoptr (GError) error = NULL;
  if (g_key_file_save_to_file (kf, file, &error)) {
    note ("%s: the new app ids for %u types", home (file), n);
  } else {
    note ("could not write %s: %s", home (file), error->message);
    failed = TRUE;
  }
}

static void
map_all_mimeapps (void)
{
  /* mimeapps.list and the desktop-specific ones (gnome-mimeapps.list) in ~/.config, and the old place for it */
  const char *config = g_get_user_config_dir ();
  g_autoptr (GDir) d = g_dir_open (config, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    if (g_str_has_suffix (name, "mimeapps.list")) {
      g_autofree char *file = g_build_filename (config, name, NULL);
      map_mimeapps (file);
    }
  g_autofree char *old = g_build_filename (g_get_user_data_dir (), "applications", "mimeapps.list", NULL);
  map_mimeapps (old);
}

/* ---- the apps' own settings (dconf) ----------------------------------------------------------------------------- */

static char *
run (const char *const *argv, const char *input, GError **error)
{
  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                           (input ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0);
  g_autoptr (GSubprocess) p = g_subprocess_newv (argv, flags, error);
  g_autofree char *out = NULL, *err = NULL;
  if (!p || !g_subprocess_communicate_utf8 (p, input, NULL, &out, &err, error))
    return NULL;
  if (!g_subprocess_get_successful (p)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s %s: %s", argv[0], argv[1], g_strstrip (err));
    return NULL;
  }
  return g_steal_pointer (&out);
}

/* hidden-from-home ("['a.desktop', ...]") with the new desktop ids; the value as it was when it is not one */
static char *
map_id_array (const char *value)
{
  g_autoptr (GVariant) v = g_variant_parse (G_VARIANT_TYPE_STRING_ARRAY, value, NULL, NULL, NULL);
  if (!v)
    return g_strdup (value);
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);
  g_autofree const char **ids = g_variant_get_strv (v, NULL);
  for (const char **i = ids; *i; i++) {
    g_autofree char *now = legacy_desktop_id (*i);
    const char *id = now ? now : *i;
    if (!ptr_has (out, id))
      g_ptr_array_add (out, g_strdup (id));
  }
  g_autoptr (GVariant) mapped = g_variant_ref_sink (g_variant_new_strv ((const char *const *) out->pdata, out->len));
  return g_variant_print (mapped, TRUE);
}

static void
copy_path (const char *old, const char *now)
{
  g_autoptr (GError) error = NULL;
  const char *dump_old[] = { "dconf", "dump", old, NULL };
  const char *dump_new[] = { "dconf", "dump", now, NULL };
  g_autofree char *had = run (dump_old, NULL, &error);
  g_autofree char *has = had && *had ? run (dump_new, NULL, &error) : NULL;
  if (error) {
    note ("settings %s not copied: %s", old, error->message);
    failed = TRUE;
    return;
  }
  if (!had || !*had)
    return;
  g_autoptr (GKeyFile) from = g_key_file_new ();
  g_autoptr (GKeyFile) to = g_key_file_new ();
  g_autoptr (GKeyFile) add = g_key_file_new ();
  if (!g_key_file_load_from_data (from, had, -1, G_KEY_FILE_NONE, &error) ||
      !g_key_file_load_from_data (to, has, -1, G_KEY_FILE_NONE, &error)) {
    note ("settings %s not copied: %s", old, error->message);
    failed = TRUE;
    return;
  }
  guint n = 0, kept = 0;
  g_auto (GStrv) groups = g_key_file_get_groups (from, NULL);
  for (char **g = groups; *g; g++) {
    g_auto (GStrv) keys = g_key_file_get_keys (from, *g, NULL, NULL);
    for (char **k = keys; k && *k; k++) {
      if (g_key_file_has_key (to, *g, *k, NULL)) {
        kept++;
        continue;
      }
      g_autofree char *value = g_key_file_get_value (from, *g, *k, NULL);
      if (!strcmp (now, "/neo/Phone/") && !strcmp (*g, "/") && !strcmp (*k, "hidden-from-home")) {
        g_autofree char *ids = map_id_array (value);
        g_key_file_set_value (add, *g, *k, ids);
      } else {
        g_key_file_set_value (add, *g, *k, value);
      }
      n++;
    }
  }
  if (n) {
    g_autofree char *data = g_key_file_to_data (add, NULL, NULL);
    const char *load[] = { "dconf", "load", now, NULL };
    g_autofree char *out = run (load, data, &error);
    if (error) {
      note ("settings %s not copied: %s", old, error->message);
      failed = TRUE;
      return;
    }
  }
  if (n || kept)
    note ("settings %s -> %s: %u copied, %u already set there", old, now, n, kept);
}

static void
copy_settings (void)
{
  g_autofree char *dconf = g_find_program_in_path ("dconf");
  if (!dconf) {
    note ("no dconf command (dconf-cli): settings not copied");
    failed = TRUE;
    return;
  }
  for (guint i = 0; i < legacy_n_paths; i++) {
    g_autofree char *old = g_strconcat (LEGACY_PATH, legacy_paths[i].old, NULL);
    copy_path (old, legacy_paths[i].now);
  }
}

/* ---- GNOME's settings that name our apps or the extension ------------------------------------------------------- */

static GSettingsSchema *
schema (const char *id)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
  return src ? g_settings_schema_source_lookup (src, id, TRUE) : NULL;
}

static GSettings *
settings (const char *id)
{
  g_autoptr (GSettingsSchema) s = schema (id);
  return s ? g_settings_new_full (s, NULL, NULL) : NULL;
}

static void
shell_lists (void)
{
  g_autoptr (GSettings) shell = settings ("org.gnome.shell");
  if (!shell)
    return;

  /* the dash's favourites (the docked desktop): the new id in front of the old one, which the shell skips */
  g_auto (GStrv) favs = g_settings_get_strv (shell, "favorite-apps");
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);
  gboolean changed = FALSE;
  for (char **f = favs; *f; f++) {
    g_autofree char *now = legacy_desktop_id (*f);
    if (now && !g_strv_contains ((const char *const *) favs, now) && !ptr_has (out, now)) {
      g_ptr_array_add (out, g_steal_pointer (&now));
      changed = TRUE;
    }
    g_ptr_array_add (out, g_strdup (*f));
  }
  if (changed) {
    g_ptr_array_add (out, NULL);
    g_settings_set_strv (shell, "favorite-apps", (const char *const *) out->pdata);
    note ("org.gnome.shell favorite-apps: the new app ids added");
  }

  /* the extension under its new UUID, and never the old one beside it */
  const char *lists[] = { "enabled-extensions", "disabled-extensions" };
  for (guint l = 0; l < G_N_ELEMENTS (lists); l++) {
    g_auto (GStrv) uuids = g_settings_get_strv (shell, lists[l]);
    if (!g_strv_contains ((const char *const *) uuids, LEGACY_EXTENSION))
      continue;
    gboolean has = g_strv_contains ((const char *const *) uuids, EXTENSION_UUID);
    g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
    for (char **u = uuids; *u; u++)
      if (strcmp (*u, LEGACY_EXTENSION))
        g_strv_builder_add (b, *u);
      else if (!has)
        g_strv_builder_add (b, EXTENSION_UUID);
    g_auto (GStrv) mapped = g_strv_builder_end (b);
    g_settings_set_strv (shell, lists[l], (const char *const *) mapped);
    note ("org.gnome.shell %s: %s in place of the old UUID", lists[l], EXTENSION_UUID);
  }
}

/* GNOME's name for an app's notification settings: the app id in lower case, "-" for anything but [a-z0-9-] */
static char *
munge (const char *app_id)
{
  return g_strcanon (g_ascii_strdown (app_id, -1), "0123456789abcdefghijklmnopqrstuvwxyz-", '-');
}

static void
notifications (void)
{
  g_autoptr (GSettings) top = settings ("org.gnome.desktop.notifications");
  g_autoptr (GSettingsSchema) app = schema ("org.gnome.desktop.notifications.application");
  if (!top || !app)
    return;
  g_auto (GStrv) kids = g_settings_get_strv (top, "application-children");
  g_auto (GStrv) keys = g_settings_schema_list_keys (app);
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_strv_builder_addv (b, (const char **) kids);
  g_autoptr (GPtrArray) added = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; i < legacy_n_ids; i++) {
    g_autofree char *old_id = g_strconcat (LEGACY_PREFIX, legacy_ids[i].old, NULL);
    g_autofree char *old = munge (old_id);
    g_autofree char *now = munge (legacy_ids[i].now);
    if (!g_strv_contains ((const char *const *) kids, old))
      continue;
    g_autofree char *old_path = g_strconcat ("/org/gnome/desktop/notifications/application/", old, "/", NULL);
    g_autofree char *new_path = g_strconcat ("/org/gnome/desktop/notifications/application/", now, "/", NULL);
    g_autoptr (GSettings) from = g_settings_new_full (app, NULL, old_path);
    g_autoptr (GSettings) to = g_settings_new_full (app, NULL, new_path);
    guint n = 0;
    for (char **k = keys; *k; k++) {
      g_autoptr (GVariant) v = g_settings_get_user_value (from, *k);
      g_autoptr (GVariant) have = g_settings_get_user_value (to, *k);
      if (!v || have)
        continue;
      if (!strcmp (*k, "application-id")) {
        g_autofree char *desktop = g_strconcat (legacy_ids[i].now, ".desktop", NULL);
        g_settings_set_string (to, *k, desktop);
      } else {
        g_settings_set_value (to, *k, v);
      }
      n++;
    }
    if (!g_strv_contains ((const char *const *) kids, now) && !ptr_has (added, now)) {
      g_strv_builder_add (b, now);
      g_ptr_array_add (added, g_strdup (now));
    }
    note ("notification settings %s -> %s: %u copied", old, now, n);
  }
  if (added->len) {
    g_auto (GStrv) all = g_strv_builder_end (b);
    g_settings_set_strv (top, "application-children", (const char *const *) all);
  }
}

/* ---- a device's defaults for the old schemas -------------------------------------------------------------------- */

/* "/neo/Phone/" -> "neo.Phone": each schema's path is its id */
static char *
path_schema_id (const char *path)
{
  g_autofree char *trimmed = g_strndup (path + 1, strlen (path) - 2);
  return g_strdelimit (g_steal_pointer (&trimmed), "/", '.');
}

/* Every "schema\nkey" -> value the vendor override files (*.gschema.override) set, with the value that wins:
 * glib-compile-schemas reads a directory's override files in name order and a later file overrides an earlier one,
 * and the first data directory is the one GSettings looks in first. Groups for one desktop ("[id:GNOME]") are left
 * out. */
static GHashTable *
vendor_overrides (void)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  const char *const *data = g_get_system_data_dirs ();
  for (gint d = (gint) g_strv_length ((char **) data) - 1; d >= 0; d--) {
    g_autofree char *dir_name = g_build_filename (data[d], "glib-2.0", "schemas", NULL);
    g_autoptr (GDir) dir = g_dir_open (dir_name, 0, NULL);
    if (!dir)
      continue;
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    for (const char *n; (n = g_dir_read_name (dir));)
      if (g_str_has_suffix (n, ".gschema.override"))
        g_ptr_array_add (names, g_strdup (n));
    g_ptr_array_sort_values (names, (GCompareFunc) strcmp);
    for (guint f = 0; f < names->len; f++) {
      g_autofree char *file = g_build_filename (dir_name, names->pdata[f], NULL);
      g_autoptr (GKeyFile) kf = g_key_file_new ();
      if (!g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, NULL))
        continue;
      g_auto (GStrv) groups = g_key_file_get_groups (kf, NULL);
      for (char **g = groups; *g; g++) {
        if (strchr (*g, ':'))
          continue;
        g_auto (GStrv) keys = g_key_file_get_keys (kf, *g, NULL, NULL);
        for (char **k = keys; k && *k; k++)
          g_hash_table_insert (set, g_strconcat (*g, "\n", *k, NULL), g_key_file_get_value (kf, *g, *k, NULL));
      }
    }
  }
  return set;
}

/* A device that set its own defaults for the old schemas (a ROM's override: hidden-from-home, no AOD on an LCD) lost
 * them with the upgrade, since an override for a schema that is no longer installed is skipped. Until the device
 * names the new schema, each such default that differs from the new schema's becomes the user's own value, when the
 * user has none: the phone looks and behaves as it did before the upgrade. */
static void
device_defaults (void)
{
  g_autoptr (GHashTable) set = vendor_overrides ();
  for (guint i = 0; i < legacy_n_paths; i++) {
    g_autofree char *now_id = path_schema_id (legacy_paths[i].now);
    g_autofree char *dir = g_strndup (legacy_paths[i].old, strlen (legacy_paths[i].old) - 1);
    g_autofree char *old_id = g_strconcat (LEGACY_PREFIX, dir, NULL);
    g_autoptr (GSettingsSchema) s = schema (now_id);
    if (!s)
      continue;
    g_autoptr (GSettings) gs = g_settings_new_full (s, NULL, NULL);
    g_auto (GStrv) keys = g_settings_schema_list_keys (s);
    for (char **k = keys; *k; k++) {
      g_autofree char *old_key = g_strconcat (old_id, "\n", *k, NULL);
      g_autofree char *now_key = g_strconcat (now_id, "\n", *k, NULL);
      const char *text = g_hash_table_lookup (set, old_key);
      if (!text || g_hash_table_contains (set, now_key))
        continue;
      g_autoptr (GVariant) mine = g_settings_get_user_value (gs, *k);
      if (mine)
        continue;
      g_autofree char *mapped = !strcmp (*k, "hidden-from-home") ? map_id_array (text) : g_strdup (text);
      g_autoptr (GSettingsSchemaKey) key = g_settings_schema_get_key (s, *k);
      g_autoptr (GVariant) v = g_variant_parse (g_settings_schema_key_get_value_type (key), mapped, NULL, NULL, NULL);
      g_autoptr (GVariant) def = g_settings_get_default_value (gs, *k);
      if (!v || !g_settings_schema_key_range_check (key, v) || (def && g_variant_equal (v, def)))
        continue;
      g_settings_set_value (gs, *k, v);
      note ("the device's default %s %s=%s kept as the user's", now_id, *k, mapped);
    }
  }
}

/* ---- once per user ---------------------------------------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  g_autofree char *stamp = legacy_stamp_path ();
  g_autofree char *dir = g_path_get_dirname (stamp);
  g_autofree char *lock_file = g_strconcat (stamp, ".lock", NULL);

  if (g_mkdir_with_parents (dir, 0700)) {
    g_printerr ("gnome-phone-migrate-ids: %s: %s\n", dir, g_strerror (errno));
    return 1;
  }
  int lock = g_open (lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock < 0 || flock (lock, LOCK_EX)) {
    g_printerr ("gnome-phone-migrate-ids: %s: %s\n", lock_file, g_strerror (errno));
    return 1;
  }
  if (g_file_test (stamp, G_FILE_TEST_EXISTS))
    return 0;

  /* plain files only: no GVfs daemon started for this, this early in the session */
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  report = g_string_new (NULL);
  move_app_dirs ();
  restore_layout ();
  move_shortcuts ();
  map_all_mimeapps ();
  copy_settings ();
  device_defaults ();
  shell_lists ();
  notifications ();
  g_settings_sync ();

  g_print ("%s", report->len ? report->str : "nothing to carry over\n");
  if (failed) {
    g_printerr ("gnome-phone-migrate-ids: not everything was carried over; it tries again at the next start\n");
    return 1;
  }
  g_autoptr (GDateTime) when = g_date_time_new_now_local ();
  g_autofree char *date = g_date_time_format_iso8601 (when);
  g_autofree char *text = g_strdup_printf ("# gnome-phone 0.1.7 renamed its IDs (neo.*); this user's settings and "
                                           "files were carried over on %s\n%s", date,
                                           report->len ? report->str : "nothing to carry over\n");
  g_autoptr (GError) error = NULL;
  if (!g_file_set_contents (stamp, text, -1, &error)) {
    g_printerr ("gnome-phone-migrate-ids: %s\n", error->message);
    return 1;
  }
  close (lock);
  return 0;
}
