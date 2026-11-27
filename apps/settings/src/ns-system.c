/* Language and region, Date and time, Users, Power, Wallpapers and Software information. The system services
 * (AccountsService, localed, timedated) are asked in the background; what needs the administrator's password asks
 * through polkit, whose prompt GNOME Shell shows. */
#include "ns-caps.h"
#include "ns-bt.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static GDBusConnection *
bus (GBusType t)
{
  return g_bus_get_sync (t, NULL, NULL);
}

/* ---- Language and region ---- */

/* What the page shows: the installed UTF-8 locales, their names, each in its own language as Android's list has them
 * (labels: NULL-terminated, as ns_row_pick_names takes them), the same locales as regions for the Formats list,
 * named in the user's language (regions: NULL-terminated too, or empty when not made), and the user's language.
 * Finding them out is slow:
 * localectl reads every installed locale (0.7 s, and seconds for the first look after a boot), AccountsService
 * answers after that, and the Language row stood empty all the while (owner, 11-21). So the last answer is kept, for
 * the app's life and in ~/.cache/vibeos-settings/language, and the page shows it at once; each time it opens it asks
 * again in the background and redraws only if something changed. With nothing kept yet (the first look ever), the
 * Language row shows a loading indicator until the answer comes. */
typedef struct {
  GPtrArray *locales, *labels, *regions;
  char      *regions_in;             /* the ICU locale the regions are named in (the user's when they were made) */
  char      *language;               /* AccountsService's Language for the user, "" when it has none */
} Known;

static Known *known;                 /* the last answer, or NULL */
static gboolean known_read;          /* the cache file was read (once) */

static void
known_free (Known *k)
{
  if (!k)
    return;
  g_clear_pointer (&k->locales, g_ptr_array_unref);
  g_clear_pointer (&k->labels, g_ptr_array_unref);
  g_clear_pointer (&k->regions, g_ptr_array_unref);
  g_free (k->regions_in);
  g_free (k->language);
  g_free (k);
}

static Known *
known_new (void)
{
  Known *k = g_new0 (Known, 1);
  k->locales = g_ptr_array_new_with_free_func (g_free);
  k->labels = g_ptr_array_new_with_free_func (g_free);
  k->regions = g_ptr_array_new_with_free_func (g_free);
  return k;
}

/* the regions' names, when @k has them in the language of @in */
static gboolean
known_regions_in (Known *k, const char *in)
{
  return k->regions->len == k->locales->len + 1 && !g_strcmp0 (k->regions_in, in);
}

static char *
known_file (void)
{
  return g_build_filename (g_get_user_cache_dir (), "vibeos-settings", "language", NULL);
}

/* the cache: "language\t<it>", "regions\t<the ICU locale they are named in>", then "<locale>\t<its name>\t<its
 * region's name>" a line each (an older cache has neither the regions line nor the third field). A few tens of
 * kilobytes, read once. */
static Known *
known_get (void)
{
  if (known_read)
    return known;
  known_read = TRUE;
  g_autofree char *file = known_file (), *text = NULL;
  if (!g_file_get_contents (file, &text, NULL, NULL))
    return NULL;
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  if (!lines[0] || !g_str_has_prefix (lines[0], "language\t"))
    return NULL;
  Known *k = known_new ();
  k->language = g_strdup (lines[0] + strlen ("language\t"));
  guint i = 1;
  if (lines[i] && g_str_has_prefix (lines[i], "regions\t"))
    k->regions_in = g_strdup (lines[i++] + strlen ("regions\t"));
  gboolean regions = TRUE;
  for (; lines[i]; i++) {
    g_auto (GStrv) f = g_strsplit (lines[i], "\t", 3);
    if (!f[0] || !*f[0] || !f[1] || !*f[1])
      continue;
    g_ptr_array_add (k->locales, g_strdup (f[0]));
    g_ptr_array_add (k->labels, g_strdup (f[1]));
    regions = regions && f[2] && *f[2];
    if (regions)
      g_ptr_array_add (k->regions, g_strdup (f[2]));
  }
  g_ptr_array_add (k->labels, NULL);
  if (regions)
    g_ptr_array_add (k->regions, NULL);
  else
    g_ptr_array_set_size (k->regions, 0);
  known = k;
  return known;
}

static void
known_written (GObject *src, GAsyncResult *res, gpointer unused)
{
  g_autoptr (GError) error = NULL;
  if (!g_file_replace_contents_finish (G_FILE (src), res, NULL, &error))
    g_debug ("the language cache: %s", error->message);
}

/* keeps @k (taking it) as the last answer, and writes it out: in the background, or at once with @now (Settings is
 * about to start over, and a write in the background would be cut short) */
static void
known_keep (Known *k, gboolean now)
{
  if (k != known) {
    known_free (known);
    known = k;
  }
  known_read = TRUE;
  GString *out = g_string_new (NULL);
  gboolean regions = k->regions_in && known_regions_in (k, k->regions_in);
  g_string_append_printf (out, "language\t%s\n", k->language ? k->language : "");
  if (regions)
    g_string_append_printf (out, "regions\t%s\n", k->regions_in);
  for (guint i = 0; i < k->locales->len; i++)
    g_string_append_printf (out, "%s\t%s%s%s\n", (char *) k->locales->pdata[i], (char *) k->labels->pdata[i],
                            regions ? "\t" : "", regions ? (char *) k->regions->pdata[i] : "");
  g_autofree char *file = known_file ();
  g_autofree char *dir = g_path_get_dirname (file);
  g_mkdir_with_parents (dir, 0700);
  g_autoptr (GError) error = NULL;
  if (now) {
    if (!g_file_set_contents (file, out->str, out->len, &error))
      g_debug ("the language cache: %s", error->message);
    g_string_free (out, TRUE);
    return;
  }
  g_autoptr (GFile) f = g_file_new_for_path (file);
  g_autoptr (GBytes) bytes = g_string_free_to_bytes (out);
  g_file_replace_contents_bytes_async (f, bytes, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, known_written,
                                       NULL);
}

static gboolean
same_strings (GPtrArray *a, GPtrArray *b)
{
  if (!a || !b || a->len != b->len)
    return FALSE;
  for (guint i = 0; i < a->len; i++)
    if (g_strcmp0 (a->pdata[i], b->pdata[i]))
      return FALSE;
  return TRUE;
}

typedef struct {
  GPtrArray *locales, *labels;       /* what the rows were made from (refs of a Known's) */
  char      *shown;                  /* the language they show */
  char      *path;                   /* the user in AccountsService */
  Known     *fresh;                  /* the background look, until AccountsService has answered */
  GtkWidget *group, *restart;
} Lang;

static void
lang_free (Lang *l)
{
  g_clear_pointer (&l->locales, g_ptr_array_unref);
  g_clear_pointer (&l->labels, g_ptr_array_unref);
  known_free (l->fresh);
  g_free (l->shown);
  g_free (l->path);
  g_free (l);
}

/* Settings started over after a language change sets this, for the page to offer the restart again */
#define RESTARTED "VIBEOS_SETTINGS_LANGUAGE_CHANGED"

/* Settings again, in the session's new language, on the Language page: this program started over in place, its
 * environment the new one already (vibeos_locale_set_session). An activation token, as an app launch passes one,
 * lets its new window take the front this one has. */
static void
start_over (void)
{
  g_settings_sync ();
  GdkDisplay *display = gdk_display_get_default ();
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new ("vibeos.Settings.desktop");
  if (display && info) {
    g_autoptr (GdkAppLaunchContext) launch = gdk_display_get_app_launch_context (display);
    g_autofree char *token =
      g_app_launch_context_get_startup_notify_id (G_APP_LAUNCH_CONTEXT (launch), G_APP_INFO (info), NULL);
    if (token)
      g_setenv ("XDG_ACTIVATION_TOKEN", token, TRUE);
  }
  g_setenv (RESTARTED, "1", TRUE);
  char *argv[] = { (char *) g_get_prgname (), "--page", "language", NULL };
  execv ("/proc/self/exe", argv);
  g_warning ("Settings could not start over: %s", g_strerror (errno));
}

/* xdg-user-dirs-gtk offers to rename the standard folders when the language they were named in (user-dirs.locale) is
 * not the session's. VibeOS keeps their English names in every language and turns it off in the session
 * (vibeos-session's user-dirs-update-gtk.service.d); this also keeps it quiet where the user's service manager
 * started before that was installed, as its own "Don't ask me this again" does: the locale as it compares it,
 * setlocale's with the "." on cut off. Nothing is renamed. */
static void
user_dirs_locale (const char *loc)
{
  g_autofree char *l = g_strndup (loc, strcspn (loc, "."));
  g_autofree char *file = g_build_filename (g_get_user_config_dir (), "user-dirs.locale", NULL);
  g_autoptr (GError) error = NULL;
  if (*l && !g_file_set_contents (file, l, -1, &error))
    g_warning ("user-dirs.locale: %s", error->message);
}

/* The session in @loc at once (Android's way): the programs started from now on, the launcher (it follows the
 * change), and Settings, started over in it. Apps that are open keep theirs until they start again. */
static void
switch_language (const char *loc)
{
  user_dirs_locale (loc);
  g_autoptr (GSettings) s = ns_settings ("org.gnome.system.locale");
  g_autofree char *region = s ? g_settings_get_string (s, "region") : NULL;
  g_autoptr (GError) error = NULL;
  if (!vibeos_locale_set_session (loc, region, &error))
    g_warning ("the session's new language: %s", error->message);
  start_over ();
}

/* AccountsService has the language (or said no): the session takes it now */
static void
lang_saved (GObject *src, GAsyncResult *res, gpointer loc)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!v)
    g_warning ("AccountsService SetLanguage: %s", error->message);
  switch_language (loc);
  g_free (loc);
}

static void
lang_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  const char *loc = l->locales->pdata[i];
  if (!g_strcmp0 (loc, l->shown))
    return;
  g_free (l->shown);
  l->shown = g_strdup (loc);
  if (known && same_strings (known->locales, l->locales)) {
    g_free (known->language);
    known->language = g_strdup (loc);
    known_keep (known, TRUE);
  }
  /* The language is the user's (AccountsService), as GNOME Settings sets it on Debian: a display manager gives it to
   * the session it starts, and so does vibeos.service (vibeos-session-language). Not the system's: Debian's systemd
   * keeps localed read-only (org.freedesktop.locale1.read-only.conf denies SetLocale to everyone, root too). The
   * change waits for AccountsService's answer: it asks polkit about this program, which would be gone by then. */
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys && l->path) {
    gtk_widget_set_sensitive (l->group, FALSE);
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", l->path, "org.freedesktop.Accounts.User", "SetLanguage",
                            g_variant_new ("(s)", loc), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                            NULL, lang_saved, g_strdup (loc));
    return;
  }
  switch_language (loc);
}

/* the Formats region: the session's at once too (the launcher's clock, the apps started from now on), Settings
 * itself with it; nothing to start over, since no text of its own changes */
static void
region_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  GSettings *s = g_object_get_data (G_OBJECT (row), "ns-locale");
  const char *region = i == 0 ? "" : (const char *) l->locales->pdata[i - 1];
  g_settings_set_string (s, "region", region);
  const char *lang = l->shown && *l->shown ? l->shown : g_getenv ("LANG");
  g_autoptr (GError) error = NULL;
  if (!vibeos_locale_set_session (lang && *lang ? lang : "C.UTF-8", region, &error))
    g_warning ("the session's new formats: %s", error->message);
  gtk_widget_set_visible (l->restart, TRUE);
}

static void
call_done (GObject *src, GAsyncResult *res, gpointer what)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!v)
    g_warning ("%s: %s", (const char *) what, error->message);
}

/* the device started again, for what keeps the language it started in: logind's restart, as the power menu's
 * Restart does it (the default reboot mode: this system again) */
static void
restart_device (GtkWidget *row, gpointer data)
{
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (!sys)
    return;
  g_autoptr (GVariant) v = g_dbus_connection_call_sync (sys, "org.freedesktop.login1", "/org/freedesktop/login1",
                                                        "org.freedesktop.login1.Manager", "SetRebootParameter",
                                                        g_variant_new ("(s)", ""), NULL, G_DBUS_CALL_FLAGS_NONE, 3000,
                                                        NULL, NULL);
  g_dbus_connection_call (sys, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                          "Reboot", g_variant_new ("(b)", TRUE), NULL,
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1, NULL, call_done,
                          (gpointer) "logind Reboot");
}

/* the Language and Formats rows, from @k */
static void
lang_rows (NsPage *page, Known *k)
{
  Lang *l = g_object_get_data (G_OBJECT (page), "ns-lang");
  g_clear_pointer (&l->locales, g_ptr_array_unref);
  g_clear_pointer (&l->labels, g_ptr_array_unref);
  l->locales = g_ptr_array_ref (k->locales);
  l->labels = g_ptr_array_ref (k->labels);
  g_free (l->shown);
  l->shown = g_strdup (k->language ? k->language : "");
  const char *current = k->language && *k->language ? k->language : g_getenv ("LANG");
  guint sel = G_MAXUINT;
  for (guint i = 0; i < l->locales->len; i++)
    if (current && !g_strcmp0 (l->locales->pdata[i], current))
      sel = i;
  ns_group_clear (l->group);
  GtkWidget *r = ns_row_pick_names (l->group, _("Language"), (const char *const *) l->labels->pdata, sel, lang_picked,
                                    l);
  /* one the list lacks by its name; "C.UTF-8" and the like (no name): no language chosen yet, programs speak their
   * own (English) */
  if (sel == G_MAXUINT) {
    g_autofree char *name = vibeos_locale_name (current, NULL);
    ns_row_set_subtitle (r, name ? name : _("System default (English)"));
  }
  GSettings *s = ns_page_settings (page, "org.gnome.system.locale");
  if (s) {
    /* the regions in the user's language ("German (Switzerland)"): what is picked is how dates and numbers look,
     * not a language to read. Named in the background with the list, or here when the list kept was named in
     * another language (the one before a change) or not at all (an older cache). */
    g_autofree char *ui = vibeos_locale_id ();
    gboolean named = known_regions_in (k, ui);
    g_autoptr (GPtrArray) rl = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (rl, g_strdup (_("Same as the language")));
    for (guint i = 0; i < l->locales->len; i++) {
      char *name = named ? g_strdup (k->regions->pdata[i]) : vibeos_locale_name (l->locales->pdata[i], ui);
      g_ptr_array_add (rl, name ? name : g_strdup (l->locales->pdata[i]));
    }
    g_ptr_array_add (rl, NULL);
    g_autofree char *region = g_settings_get_string (s, "region");
    guint rs = 0;
    for (guint i = 0; i < l->locales->len; i++)
      if (!g_strcmp0 (l->locales->pdata[i], region))
        rs = i + 1;
    r = ns_row_pick_names (l->group, _("Formats"), (const char *const *) rl->pdata, rs, region_picked, l);
    g_object_set_data (G_OBJECT (r), "ns-locale", s);
  }
}

/* the background look is complete: kept, and the rows redrawn if it differs from what they show */
static void
lang_fresh (NsPage *page, const char *language)
{
  Lang *l = g_object_get_data (G_OBJECT (page), "ns-lang");
  Known *k = g_steal_pointer (&l->fresh);
  k->language = g_strdup (language ? language : "");
  /* localectl gave nothing: the last answer's list stays; with none, the rows show the language without a list,
   * and nothing is kept */
  if (!k->locales->len) {
    if (!known) {
      lang_rows (page, k);
      known_free (k);
      return;
    }
    g_free (known->language);
    known->language = g_strdup (k->language);
    known_free (k);
    k = known;
  }
  /* the regions' names need no second look: the rows named them here when the list had none in this language */
  gboolean same = same_strings (l->locales, k->locales) && same_strings (l->labels, k->labels) &&
                  !g_strcmp0 (l->shown, k->language);
  known_keep (k, FALSE);
  if (!same)
    lang_rows (page, known);
}

static void
user_lang_got (GObject *src, GAsyncResult *res, gpointer page)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) lang = NULL;
  if (v)
    g_variant_get (v, "(v)", &lang);
  lang_fresh (page, lang ? g_variant_get_string (lang, NULL) : NULL);
  g_object_unref (page);
}

static void
user_found (GObject *src, GAsyncResult *res, gpointer page)
{
  Lang *l = g_object_get_data (page, "ns-lang");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (!v) {
    lang_fresh (page, NULL);
    g_object_unref (page);
    return;
  }
  g_free (l->path);
  g_variant_get (v, "(o)", &l->path);
  g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", l->path,
                          "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", "org.freedesktop.Accounts.User", "Language"), G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NONE, 3000, NULL, user_lang_got, page);
}

/* in a thread: the installed locales from localectl, the UTF-8 ones that name a language, with their names, and
 * their regions' names in the language of @data (the user's ICU locale, found before: the environment is read in
 * the main thread only) */
static void
locales_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  const char *argv[] = { "localectl", "list-locales", "--no-pager", NULL };
  g_autofree char *out = NULL;
  g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                NULL, NULL);
  Known *k = known_new ();
  g_auto (GStrv) lines = g_strsplit (out ? out : "", "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const char *loc = g_strstrip (lines[i]);
    if (!*loc || !strstr (loc, "UTF-8") || g_str_has_prefix (loc, "C."))
      continue;
    g_ptr_array_add (k->locales, g_strdup (loc));
    char *name = vibeos_locale_name (loc, NULL), *region = vibeos_locale_name (loc, data);
    g_ptr_array_add (k->labels, name ? name : g_strdup (loc));
    g_ptr_array_add (k->regions, region ? region : g_strdup (loc));
  }
  g_ptr_array_add (k->labels, NULL);
  g_ptr_array_add (k->regions, NULL);
  k->regions_in = g_strdup (data);
  g_task_return_pointer (task, k, (GDestroyNotify) known_free);
}

static void
locales_listed (GObject *src, GAsyncResult *res, gpointer page)
{
  Lang *l = g_object_get_data (page, "ns-lang");
  known_free (l->fresh);
  l->fresh = g_task_propagate_pointer (G_TASK (res), NULL);
  if (!l->fresh)
    l->fresh = known_new ();
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.freedesktop.Accounts",
                            "FindUserById", g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, user_found, page);
  else {
    lang_fresh (page, known ? known->language : NULL);
    g_object_unref (page);
  }
}

void
ns_page_language (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Language and region"), &c);
  Lang *l = g_new0 (Lang, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-lang", l, (GDestroyNotify) lang_free);
  l->group = ns_group (c, NULL);
  Known *k = known_get ();
  if (k)
    lang_rows (page, k);
  else
    ns_row_suffix (ns_row (l->group, NULL, _("Language"), NULL), ns_spinner_new ());
  ns_note (c, _("Apps that were already open change language after a restart."));
  GtkWidget *g = ns_group (c, NULL);
  l->restart = ns_row_button (g, _("Restart now"), FALSE, restart_device, NULL);
  /* shown once a language or a format has been picked: Settings started over after a language change says so */
  gtk_widget_set_visible (l->restart, g_getenv (RESTARTED) != NULL);
  GtkWidget *services;
  /* the services that keep running through a change (the call service, the SMS service, the reminders, the alarms)
   * say what they say in the language they started in: told after a language change, with the restart */
  gboolean changed = g_getenv (RESTARTED) != NULL;
  /* Translators: "Phone" and "Messages" are the apps' names, as their own translations have them */
  services = ns_note (c, _("Phone, Messages, reminders and alarms switch to the new language after a restart."));
  gtk_widget_set_visible (services, changed);
  g_autoptr (GTask) t = g_task_new (NULL, NULL, locales_listed, g_object_ref (page));
  g_task_set_task_data (t, vibeos_locale_id (), g_free);
  g_task_run_in_thread (t, locales_thread);
  ns_push (page);
}

/* ---- Date and time ---- */

typedef struct {
  GtkWidget *ntp, *zone;
  GPtrArray *zones;
} Time;

static void
time_free (Time *t)
{
  if (t->zones)
    g_ptr_array_unref (t->zones);
  g_free (t);
}

static void
timedate_call (const char *method, GVariant *args)
{
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.timedate1", method, args, NULL,
                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 60000, NULL, NULL, NULL);
}

static void
ntp_set (GtkWidget *sw, GParamSpec *ps, gpointer data)
{
  if (!ns_switch_syncing (GTK_WIDGET (sw)))
    timedate_call ("SetNTP", g_variant_new ("(bb)", ns_switch_get_active (sw), TRUE));
}

static void
zone_set (GtkWidget *row, gpointer zone)
{
  NsPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  timedate_call ("SetTimezone", g_variant_new ("(sb)", (const char *) zone, TRUE));
  g_autofree char *nice = g_strdelimit (g_strdup (zone), "_", ' ');
  ns_row_set_subtitle (t->zone, nice);
  ns_pop_to (page);
}

/* two steps: the region, then its cities */
static void
zone_region (GtkWidget *row, gpointer region)
{
  NsPage *page = g_object_get_data (G_OBJECT (row), "ns-page");
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  GtkWidget *c;
  NsPage *cities = ns_page_new (region, &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autofree char *prefix = g_strconcat (region, "/", NULL);
  for (guint i = 0; i < t->zones->len; i++) {
    const char *z = t->zones->pdata[i];
    if (!g_str_has_prefix (z, prefix))
      continue;
    g_autofree char *nice = g_strdelimit (g_strdup (z + strlen (prefix)), "_/", ' ');
    GtkWidget *r = ns_row (g, NULL, nice, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, zone_set, (gpointer) z);
  }
  ns_push (cities);
}

static void
zones_page (GtkWidget *row, gpointer page)
{
  Time *t = g_object_get_data (G_OBJECT (page), "ns-time");
  if (!t->zones)
    return;
  GtkWidget *c;
  NsPage *regions = ns_page_new (_("Time zone"), &c);
  GtkWidget *g = ns_group (c, NULL);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < t->zones->len; i++) {
    const char *z = t->zones->pdata[i];
    const char *slash = strchr (z, '/');
    if (!slash)
      continue;
    char *region = g_strndup (z, slash - z);
    if (g_hash_table_contains (seen, region) || !g_strcmp0 (region, "Etc")) {
      g_free (region);
      continue;
    }
    g_hash_table_add (seen, region);
    GtkWidget *r = ns_row (g, NULL, region, NULL);
    g_object_set_data (G_OBJECT (r), "ns-page", page);
    ns_row_on_activate (r, zone_region, region);
  }
  g_object_set_data_full (G_OBJECT (regions), "ns-seen", g_hash_table_ref (seen), (GDestroyNotify) g_hash_table_unref);
  ns_push (regions);
}

static void
zones_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Time *t = g_object_get_data (page, "ns-time");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (v) {
    g_autofree const char **z = NULL;
    g_variant_get (v, "(^a&s)", &z);
    t->zones = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; z[i]; i++)
      g_ptr_array_add (t->zones, g_strdup (z[i]));
  }
  g_object_unref (page);
}

static void
timedate_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Time *t = g_object_get_data (page, "ns-time");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    gboolean ntp = FALSE;
    const char *zone = NULL;
    g_variant_lookup (props, "NTP", "b", &ntp);
    g_variant_lookup (props, "Timezone", "&s", &zone);
    ns_switch_quiet (ns_row_get_switch (t->ntp), ntp);
    g_autofree char *nice = zone ? g_strdelimit (g_strdup (zone), "_", ' ') : NULL;
    ns_row_set_subtitle (t->zone, nice);
  }
  g_object_unref (page);
}

static gboolean
h24_get (GValue *value, GVariant *v, gpointer data)
{
  g_value_set_boolean (value, !g_strcmp0 (g_variant_get_string (v, NULL), "24h"));
  return TRUE;
}

static GVariant *
h24_set (const GValue *value, const GVariantType *type, gpointer data)
{
  return g_variant_new_string (g_value_get_boolean (value) ? "24h" : "12h");
}

void
ns_page_datetime (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Date and time"), &c);
  Time *t = g_new0 (Time, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-time", t, (GDestroyNotify) time_free);
  GtkWidget *g = ns_group (c, NULL);
  t->ntp = ns_row_switch (g, _("Automatic date and time"), _("From the network"), NULL, NULL);
  g_signal_connect (ns_row_get_switch (t->ntp), "notify::active", G_CALLBACK (ntp_set), NULL);
  GSettings *dt = ns_page_settings (page, "org.gnome.desktop.datetime");
  if (ns_has_key (dt, "automatic-timezone")) {
    ns_row_switch (g, _("Automatic time zone"),
                   ns_device_text (_("From the phone's location"), _("From the tablet's location")), dt,
                   "automatic-timezone");
  }
  t->zone = ns_row (g, NULL, _("Time zone"), NULL);
  ns_row_on_activate (t->zone, zones_page, page);

  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  g = ns_group (c, _("Clock"));
  /* clock-format is read by the status bar and the lock screen (statusbar.js, keyguard.js) and by the apps.
   * No weekday, date or seconds switches (clock-show-*): the status bar shows the time alone. */
  GtkWidget *r = ns_row_switch (g, _("24-hour time"), _("13:00 rather than 1:00 PM"), NULL, NULL);
  g_settings_bind_with_mapping (iface, "clock-format", ns_row_get_switch (r), "active", G_SETTINGS_BIND_DEFAULT,
                                h24_get, h24_set, NULL, NULL);

  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys) {
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.timedate1"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, timedate_got, g_object_ref (page));
    g_dbus_connection_call (sys, "org.freedesktop.timedate1", "/org/freedesktop/timedate1",
                            "org.freedesktop.timedate1", "ListTimezones", NULL, G_VARIANT_TYPE ("(as)"),
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, zones_got, g_object_ref (page));
  }
  ns_push (page);
}

/* ---- Users ---- */

static void
real_name_set (const char *text, gpointer row)
{
  const char *path = g_object_get_data (G_OBJECT (row), "ns-path");
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (text && sys && path) {
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", path, "org.freedesktop.Accounts.User", "SetRealName",
                            g_variant_new ("(s)", text), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                            NULL, NULL, NULL);
    ns_row_set_subtitle (row, text);
  }
  g_object_unref (row);
}

static void
real_name_tapped (GtkWidget *row, gpointer data)
{
  ns_ask (row, _("Your name"), NULL, g_object_get_data (G_OBJECT (row), "ns-name"), FALSE, _("Save"), real_name_set,
          g_object_ref (row));
}

typedef struct {
  GtkWidget *name, *login, *type, *others;
} Users;

/* the text's inset in the rows under the one with the picture, so that the card's text lines up (libvibeos's
 * TEXT_AFTER_START, the inset after a row's start) */
#define AFTER_PICTURE 68

/* the user's picture at the row's start: AccountsService's IconFile (the picture chosen in Settings, or ~/.face for a
 * user who has chosen none), or the generic one when there is none or it cannot be read (another user's home often
 * is not) */
static void
user_picture (GtkWidget *row, GVariant *props)
{
  const char *path = NULL;
  g_autoptr (GdkTexture) t = NULL;
  GStatBuf st;
  g_variant_lookup (props, "IconFile", "&s", &path);
  /* a picture, not something huge (AccountsService takes a chosen one up to 1 MB) */
  if (path && *path && g_stat (path, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0 && st.st_size <= 4 << 20)
    t = gdk_texture_new_from_filename (path, NULL);
  if (t) {
    const GdkRGBA none = { 0, 0, 0, 0 };
    vibeos_list_row_set_avatar (VIBEOS_LIST_ROW (row), GDK_PAINTABLE (t), NULL, &none);
  } else {
    vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "avatar-default-symbolic");
  }
}

static void
other_got (GObject *src, GAsyncResult *res, gpointer group)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    const char *real = "", *login = "";
    g_variant_lookup (props, "RealName", "&s", &real);
    g_variant_lookup (props, "UserName", "&s", &login);
    user_picture (ns_row (group, NULL, *real ? real : login, login), props);
    ns_group_set_visible (group, TRUE);
  }
  g_object_unref (group);
}

static void
me_got (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) props = NULL;
  if (v) {
    g_variant_get (v, "(@a{sv})", &props);
    const char *real = "", *login = "";
    gint32 type = 0;
    g_variant_lookup (props, "RealName", "&s", &real);
    g_variant_lookup (props, "UserName", "&s", &login);
    g_variant_lookup (props, "AccountType", "i", &type);
    ns_row_set_subtitle (u->name, *real ? real : _("Not set"));
    g_object_set_data_full (G_OBJECT (u->name), "ns-name", g_strdup (real), g_free);
    ns_row_set_subtitle (u->login, login);
    ns_row_set_subtitle (u->type, type == 1 ? _("Administrator") : _("Standard"));
    user_picture (u->name, props);
  }
  g_object_unref (page);
}

static void
users_listed (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  const char *me = g_object_get_data (G_OBJECT (u->name), "ns-path");
  if (v) {
    g_autofree const char **paths = NULL;
    g_variant_get (v, "(^a&o)", &paths);
    for (guint i = 0; paths[i]; i++)
      if (g_strcmp0 (paths[i], me))
        g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", paths[i],
                                "org.freedesktop.DBus.Properties", "GetAll",
                                g_variant_new ("(s)", "org.freedesktop.Accounts.User"), G_VARIANT_TYPE ("(a{sv})"),
                                G_DBUS_CALL_FLAGS_NONE, 3000, NULL, other_got, g_object_ref (u->others));
  }
  g_object_unref (page);
}

static void
me_found (GObject *src, GAsyncResult *res, gpointer page)
{
  Users *u = g_object_get_data (page, "ns-users");
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (v) {
    const char *path;
    g_variant_get (v, "(&o)", &path);
    g_object_set_data_full (G_OBJECT (u->name), "ns-path", g_strdup (path), g_free);
    g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", path,
                            "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", "org.freedesktop.Accounts.User"), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, me_got, g_object_ref (page));
    g_dbus_connection_call (G_DBUS_CONNECTION (src), "org.freedesktop.Accounts", "/org/freedesktop/Accounts",
                            "org.freedesktop.Accounts", "ListCachedUsers", NULL, G_VARIANT_TYPE ("(ao)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, users_listed, g_object_ref (page));
  }
  g_object_unref (page);
}

void
ns_page_users (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Users"), &c);
  Users *u = g_new0 (Users, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-users", u, g_free);
  GtkWidget *g = ns_group (c, _("Your account"));
  /* the picture on the name's row; the generic one until AccountsService answers */
  u->name = ns_row (g, "avatar-default-symbolic", _("Name"), NULL);
  ns_row_on_activate (u->name, real_name_tapped, NULL);
  u->login = ns_row_info (g, _("User name"), g_get_user_name ());
  u->type = ns_row_info (g, _("Account type"), NULL);
  vibeos_list_row_set_text_inset (VIBEOS_LIST_ROW (u->login), AFTER_PICTURE);
  vibeos_list_row_set_text_inset (VIBEOS_LIST_ROW (u->type), AFTER_PICTURE);
  g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, NULL, _("Screen lock and password"), _("A pattern or PIN is also your password"));
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  u->others = ns_group (c, _("Other users"));
  ns_group_set_visible (u->others, FALSE);
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (sys)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.freedesktop.Accounts",
                            "FindUserById", g_variant_new ("(x)", (gint64) getuid ()), G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, me_found, g_object_ref (page));
  ns_push (page);
}

/* ---- Power ---- */

/* Automatic suspend is gnome-settings-daemon's (sleep-inactive-battery/-ac: -type and -timeout): after so long
 * without a touch it suspends the phone, as on any GNOME machine. Some phones have a daemon of their own that
 * suspends whenever the screen goes off (VibeOS's s10e-sleepd and surya-sleepd, for the Galaxy S10e and the
 * Poco X3); there "Never" would not keep the phone awake and a time longer than the screen timeout would never
 * come, so the rows go while one of them runs. They are known by their process names (a comm, at most 15
 * characters): a phone with neither has GNOME's own behaviour, and the rows. */
static const char *const screen_off_sleepers[] = { "s10e-sleepd", "surya-sleepd", NULL };

gboolean
ns_screen_off_sleeper (void)
{
  g_autoptr (GDir) d = g_dir_open ("/proc", 0, NULL);
  for (const char *n; d && (n = g_dir_read_name (d));) {
    if (!g_ascii_isdigit (*n))
      continue;
    g_autofree char *path = g_build_filename ("/proc", n, "comm", NULL);
    g_autofree char *comm = NULL;
    if (g_file_get_contents (path, &comm, NULL, NULL) && g_strv_contains (screen_off_sleepers, g_strstrip (comm)))
      return TRUE;
  }
  return FALSE;
}

static const int sleeps[] = { 0, 300, 600, 900, 1800, 3600 };
static const char *const sleep_labels[] = { N_("Never"), N_("After 5 minutes"), N_("After 10 minutes"),
                                            N_("After 15 minutes"),
                                            N_("After 30 minutes"), N_("After 1 hour"), NULL };

static void
sleep_picked (GtkWidget *row, guint i, gpointer power)
{
  const char *what = g_object_get_data (G_OBJECT (row), "ns-what");
  g_autofree char *type = g_strconcat ("sleep-inactive-", what, "-type", NULL);
  g_autofree char *timeout = g_strconcat ("sleep-inactive-", what, "-timeout", NULL);
  g_settings_set_string (power, type, i ? "suspend" : "nothing");
  if (i)
    g_settings_set_int (power, timeout, sleeps[i]);
}

static void
sleep_row (GtkWidget *g, GSettings *power, const char *title, const char *what)
{
  g_autofree char *type = g_strconcat ("sleep-inactive-", what, "-type", NULL);
  g_autofree char *timeout = g_strconcat ("sleep-inactive-", what, "-timeout", NULL);
  g_autofree char *t = g_settings_get_string (power, type);
  int s = g_settings_get_int (power, timeout);
  guint sel = 0;
  for (guint i = 1; g_strcmp0 (t, "nothing") && i < G_N_ELEMENTS (sleeps); i++)
    if (sleeps[i] <= s)
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, sleep_labels, sel, sleep_picked, power);
  g_object_set_data (G_OBJECT (r), "ns-what", (gpointer) what);
}

/* Not on this page, and why: the power button's action (power-button-action). VibeOS keeps the power key to
 * itself (power.js), as Phosh did, so gnome-settings-daemon's action never runs. */
void
ns_page_power (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Power and screen blank"), &c);
  GSettings *power = ns_page_settings (page, "org.gnome.settings-daemon.plugins.power");
  if (!power)
    return;
  GtkWidget *g;
  if (!ns_screen_off_sleeper ()) {
    g = ns_group (c, _("Automatic suspend"));
    sleep_row (g, power, _("On battery"), "battery");
    sleep_row (g, power, _("Plugged in"), "ac");
    ns_note (c, ns_device_text (_("Suspend saves the most power, but calls, messages and alarms can be missed "
                                  "while the phone sleeps."),
                                _("Suspend saves the most power, but calls, messages and alarms can be missed "
                                  "while the tablet sleeps.")));
  }
  g = ns_group (c, NULL);
  /* gnome-settings-daemon dims the backlight through the shell a little before the screen timeout */
  if (ns_has_key (power, "idle-dim"))
    ns_row_switch (g, _("Dim the screen"), _("Before it turns off"), power, "idle-dim");
  /* only with power-profiles-daemon, which switches the profile */
  if (ns_has_key (power, "power-saver-profile-on-low-battery") &&
      (g_file_test ("/usr/share/dbus-1/system-services/org.freedesktop.UPower.PowerProfiles.service",
                    G_FILE_TEST_EXISTS) ||
       g_file_test ("/usr/share/dbus-1/system-services/net.hadess.PowerProfiles.service", G_FILE_TEST_EXISTS)))
    ns_row_switch (g, _("Power saving on low battery"), NULL, power, "power-saver-profile-on-low-battery");
  if (ns_screen_off_sleeper ()) {
    ns_note (c, ns_device_text (_("This phone sleeps as soon as its screen is off."),
                                _("This tablet sleeps as soon as its screen is off.")));
  }
  ns_push (page);
}

/* ---- Wallpapers ---- */

#define WALL_W 150
#define WALL_H 300

typedef struct {
  GtkWidget *pic;
  char      *path;
  GdkTexture *tex;
} Thumb;

static char *
wall_cache (const char *path)
{
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_MD5, path, -1);
  g_autofree char *name = g_strconcat ("wall-", sum, ".png", NULL);
  return g_build_filename (g_get_user_cache_dir (), "vibeos-settings", name, NULL);
}

static gboolean
thumb_show (gpointer data)
{
  Thumb *t = data;
  if (t->tex)
    gtk_picture_set_paintable (GTK_PICTURE (t->pic), GDK_PAINTABLE (t->tex));
  g_clear_object (&t->tex);
  g_object_unref (t->pic);
  g_free (t->path);
  g_free (t);
  return G_SOURCE_REMOVE;
}

/* one after the other in one thread: big pictures decoded side by side would want a lot of memory */
static void
thumbs_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  GPtrArray *thumbs = data;
  for (guint i = 0; i < thumbs->len && !g_cancellable_is_cancelled (cancel); i++) {
    Thumb *t = thumbs->pdata[i];
    thumbs->pdata[i] = NULL;
    g_autofree char *cache = wall_cache (t->path);
    g_autoptr (GdkPixbuf) pb = gdk_pixbuf_new_from_file (cache, NULL);
    if (!pb) {
      pb = gdk_pixbuf_new_from_file_at_scale (t->path, WALL_W * 2, WALL_H, TRUE, NULL);
      if (pb) {
        g_autofree char *dir = g_path_get_dirname (cache);
        g_mkdir_with_parents (dir, 0700);
        gdk_pixbuf_save (pb, cache, "png", NULL, NULL);
      }
    }
    if (pb)
      t->tex = ns_texture_for_pixbuf (pb);
    g_idle_add (thumb_show, t);
  }
  for (guint i = 0; i < thumbs->len; i++)
    if (thumbs->pdata[i])
      g_idle_add (thumb_show, thumbs->pdata[i]);
  g_task_return_boolean (task, TRUE);
}

/* GNOME's background is the wallpaper of both screens: the launcher draws it, and the lock screen (keyguard.js)
 * shows the same picture, as Shell 51's own lock screen does. The screensaver's picture-uri is read by nothing. */
static void
wall_apply (const char *path, const char *dark)
{
  g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
  g_autofree char *duri = dark ? g_filename_to_uri (dark, NULL, NULL) : g_strdup (uri);
  g_autoptr (GSettings) bg = ns_settings ("org.gnome.desktop.background");
  if (!bg)
    return;
  g_settings_set_string (bg, "picture-uri", uri);
  g_settings_set_string (bg, "picture-uri-dark", duri);
  /* a picture chosen here replaces the parallax one, which would hide it */
  g_autoptr (GSettings) vibeos = ns_settings ("vibeos.Shell");
  if (vibeos && ns_has_key (vibeos, "wallpaper-parallax"))
    g_settings_set_string (vibeos, "wallpaper-parallax", "");
}

typedef struct {
  char *path, *dark;
  GtkWidget *w;
} Apply;

static void
apply_response (gboolean yes, gpointer data)
{
  Apply *a = data;
  if (yes)
    wall_apply (a->path, a->dark);
  g_free (a->path);
  g_free (a->dark);
  g_object_unref (a->w);
  g_free (a);
}

/* "Set as wallpaper", asked before a tapped picture replaces the one there is (the parallax grid asks it too).
 * One picture is both screens' (wall_apply), so there is no "Home screen" / "Lock screen" choice to make. */
void
ns_wallpaper_ask (GtkWidget *w, const char *path, const char *dark)
{
  Apply *a = g_new0 (Apply, 1);
  a->path = g_strdup (path);
  a->dark = g_strdup (dark);
  a->w = g_object_ref (w);
  ns_confirm (w, _("Set as wallpaper"), _("On the home screen and the lock screen"), _("Set"), FALSE, apply_response,
              a);
}

static void
wall_tapped (GtkFlowBox *flow, GtkFlowBoxChild *child, gpointer data)
{
  GtkWidget *pic = gtk_flow_box_child_get_child (child);
  ns_wallpaper_ask (GTK_WIDGET (flow), g_object_get_data (G_OBJECT (pic), "ns-path"),
                    g_object_get_data (G_OBJECT (pic), "ns-dark"));
}

static void
file_chosen (GObject *src, GAsyncResult *res, gpointer w)
{
  g_autoptr (GFile) f = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  g_autofree char *path = f ? g_file_get_path (f) : NULL;
  if (path)
    ns_wallpaper_ask (w, path, NULL);
  g_object_unref (w);
}

static void
from_files (GtkWidget *row, gpointer data)
{
  GtkFileDialog *fd = gtk_file_dialog_new ();
  GtkFileFilter *ff = gtk_file_filter_new ();
  gtk_file_filter_set_name (ff, _("Pictures"));
  gtk_file_filter_add_mime_type (ff, "image/*");
  gtk_file_dialog_set_default_filter (fd, ff);
  g_autoptr (GFile) pics = g_file_new_for_path (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES)
                                                ? g_get_user_special_dir (G_USER_DIRECTORY_PICTURES) : g_get_home_dir ());
  gtk_file_dialog_set_initial_folder (fd, pics);
  gtk_file_dialog_open (fd, GTK_WINDOW (gtk_widget_get_root (row)), NULL, file_chosen, g_object_ref (row));
  g_object_unref (ff);
  g_object_unref (fd);
}

static gboolean
is_picture (const char *n)
{
  return g_str_has_suffix (n, ".jpg") || g_str_has_suffix (n, ".jpeg") || g_str_has_suffix (n, ".png")
         || g_str_has_suffix (n, ".jxl") || g_str_has_suffix (n, ".webp") || g_str_has_suffix (n, ".svg");
}

static void
cancel_free (GCancellable *c)
{
  g_cancellable_cancel (c);
  g_object_unref (c);
}

void
ns_page_wallpapers (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Wallpapers"), &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, _("Choose from your pictures"), FALSE, from_files, NULL);

  g = ns_group (c, NULL);
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 4);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flow), TRUE);
  g_signal_connect (flow, "child-activated", G_CALLBACK (wall_tapped), NULL);
  ns_row_widget (g, NULL, flow);

  GPtrArray *thumbs = g_ptr_array_new ();
  g_autofree char *user = g_build_filename (g_get_user_data_dir (), "backgrounds", NULL);
  const char *dirs[] = { user, "/usr/share/backgrounds/gnome", "/usr/share/backgrounds", NULL };
  for (guint d = 0; dirs[d]; d++) {
    g_autoptr (GDir) dir = g_dir_open (dirs[d], 0, NULL);
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    for (const char *n; dir && (n = g_dir_read_name (dir));)
      if (is_picture (n) && !strstr (n, "-d."))     /* GNOME's dark variants go with their light ones */
        g_ptr_array_add (names, g_strdup (n));
    g_ptr_array_sort (names, (GCompareFunc) g_ascii_strcasecmp);
    for (guint i = 0; i < names->len; i++) {
      const char *n = names->pdata[i];
      g_autofree char *path = g_build_filename (dirs[d], n, NULL);
      GtkWidget *pic = gtk_picture_new ();
      gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
      gtk_widget_set_size_request (pic, WALL_W / 2 + 40, WALL_H / 2 + 40);
      gtk_widget_set_overflow (pic, GTK_OVERFLOW_HIDDEN);
      gtk_widget_add_css_class (pic, "ns-wall-thumb");
      gtk_widget_add_css_class (pic, "ns-parallax-thumb");
      g_object_set_data_full (G_OBJECT (pic), "ns-path", g_strdup (path), g_free);
      /* "x-l.jxl" has a "x-d.jxl" for the dark style */
      const char *l = strstr (n, "-l.");
      if (l) {
        g_autofree char *dn = g_strdup (n);
        dn[l - n + 1] = 'd';
        g_autofree char *dark = g_build_filename (dirs[d], dn, NULL);
        if (g_file_test (dark, G_FILE_TEST_EXISTS))
          g_object_set_data_full (G_OBJECT (pic), "ns-dark", g_steal_pointer (&dark), g_free);
      }
      gtk_flow_box_append (GTK_FLOW_BOX (flow), pic);
      Thumb *t = g_new0 (Thumb, 1);
      t->pic = g_object_ref (pic);
      t->path = g_strdup (path);
      g_ptr_array_add (thumbs, t);
    }
  }
  GCancellable *cancel = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (page), "ns-cancel", cancel, (GDestroyNotify) cancel_free);
  g_autoptr (GTask) task = g_task_new (NULL, cancel, NULL, NULL);
  g_task_set_task_data (task, thumbs, (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, thumbs_thread);
  ns_push (page);
}

/* ---- Software information ---- */

/* the shell's own version, as GNOME Settings' About asks for it: org.gnome.Shell's ShellVersion */
static void
shell_version_got (GObject *src, GAsyncResult *res, gpointer row)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  g_autoptr (GVariant) version = NULL;
  if (v)
    g_variant_get (v, "(v)", &version);
  if (version && g_variant_is_of_type (version, G_VARIANT_TYPE_STRING))
    ns_row_set_subtitle (row, g_variant_get_string (version, NULL));
  g_object_unref (row);
}

void
ns_page_software (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Software information"), &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_bt_name_row (g);

  g = ns_group (c, NULL);
  if (*vibeos_release ())
    ns_row_info (g, "VibeOS", vibeos_release ());
  g_autofree char *deb = NULL;
  if (g_file_get_contents ("/etc/debian_version", &deb, NULL, NULL))
    g_strstrip (deb);
  ns_row_info (g, "Debian", deb);
  GtkWidget *shell = ns_row_info (g, "GNOME Shell", NULL);
  g_autoptr (GDBusConnection) session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session)
    g_dbus_connection_call (session, "org.gnome.Shell", "/org/gnome/Shell", "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", "org.gnome.Shell", "ShellVersion"), G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, shell_version_got, g_object_ref (shell));
  g_autofree char *gtk = g_strdup_printf ("%u.%u.%u", gtk_get_major_version (), gtk_get_minor_version (),
                                          gtk_get_micro_version ());
  ns_row_info (g, "GTK", gtk);
  struct utsname u;
  uname (&u);
  ns_row_info (g, _("Kernel"), u.release);
  ns_row_info (g, _("Kernel build"), u.version);
  ns_row_info (g, _("Settings"), NS_VERSION);
  ns_push (page);
}

/* ---- Mouse, touchpad and hardware keyboard: mutter follows GNOME's peripherals settings ---- */

static void
speed_row (GtkWidget *g, GSettings *s)
{
  GtkWidget *scale;
  ns_row_scale (g, _("Pointer speed"), "input-mouse-symbolic", -1, 1, 0.1, &scale);
  g_settings_bind (s, "speed", gtk_range_get_adjustment (GTK_RANGE (scale)), "value", G_SETTINGS_BIND_DEFAULT);
}

static const guint delays[] = { 250, 500, 800 };
static const char *const delay_labels[] = { NC_("key repeat delay", "Short"), NC_("key repeat delay", "Medium"),
                                            NC_("key repeat delay", "Long"), NULL };
static const guint rates[] = { 15, 30, 60 };
static const char *const rate_labels[] = { NC_("key repeat speed", "Fast"), NC_("key repeat speed", "Medium"),
                                           NC_("key repeat speed", "Slow"), NULL };

static void
uint_picked (GtkWidget *row, guint i, gpointer s)
{
  const guint *values = g_object_get_data (G_OBJECT (row), "ns-uints");
  g_settings_set_uint (s, g_object_get_data (G_OBJECT (row), "ns-key"), values[i]);
}

static GtkWidget *
uint_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const guint *values, guint n,
          const char *context, const char *const *labels)
{
  guint v = g_settings_get_uint (s, key), sel = 0;
  for (guint i = 0; i < n; i++)
    if (ABS ((int) values[i] - (int) v) < ABS ((int) values[sel] - (int) v))
      sel = i;
  GtkWidget *r = ns_row_pick_in (g, title, context, labels, sel, uint_picked, s);
  g_object_set_data (G_OBJECT (r), "ns-uints", (gpointer) values);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
  return r;
}

void
ns_page_mouse (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new (_("Mouse and touchpad"), &c);
  GSettings *mouse = ns_page_settings (page, "org.gnome.desktop.peripherals.mouse");
  GSettings *pad = ns_page_settings (page, "org.gnome.desktop.peripherals.touchpad");
  GSettings *kbd = ns_page_settings (page, "org.gnome.desktop.peripherals.keyboard");
  if (mouse) {
    GtkWidget *g = ns_group (c, _("Mouse"));
    speed_row (g, mouse);
    ns_row_switch (g, _("Natural scrolling"), _("The content moves with the wheel, like a touchscreen"), mouse,
                   "natural-scroll");
    ns_row_switch (g, _("Left-handed"), _("The right button is the main one"), mouse, "left-handed");
  }
  if (pad) {
    GtkWidget *g = ns_group (c, _("Touchpad"));
    speed_row (g, pad);
    ns_row_switch (g, _("Tap to click"), _("A tap counts as a click"), pad, "tap-to-click");
    ns_row_switch (g, _("Natural scrolling"), _("The content moves with your fingers"), pad, "natural-scroll");
  }
  if (kbd) {
    GtkWidget *g = ns_group (c, _("Hardware keyboard"));
    ns_row_switch (g, _("Repeat keys"), _("A held key types again and again"), kbd, "repeat");
    GtkWidget *r = uint_row (g, _("Delay before repeating"), kbd, "delay", delays, G_N_ELEMENTS (delays),
                             "key repeat delay", delay_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
    r = uint_row (g, _("Repeat speed"), kbd, "repeat-interval", rates, G_N_ELEMENTS (rates), "key repeat speed",
                  rate_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
  }
  ns_note (c, _("For a Bluetooth or USB mouse, touchpad or keyboard."));
  ns_push (page);
}
