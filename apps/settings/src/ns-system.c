/* Language and region, Date and time, Users, Power, Wallpapers and Software information. The system services
 * (AccountsService, localed, timedated) are asked in the background; what needs the administrator's password asks
 * through polkit, whose prompt GNOME Shell shows. */
#include "ns-caps.h"
#include "ns-bt.h"
#include "ns-pages.h"
#include "ns-ui.h"

#include <json-glib/json-glib.h>
#include <sys/utsname.h>
#include <unistd.h>

static GDBusConnection *
bus (GBusType t)
{
  return g_bus_get_sync (t, NULL, NULL);
}

/* ---- Language and region ---- */

typedef struct {
  GHashTable *langs, *countries;     /* "de" → "German", "DE" → "Germany" */
} Names;

static GHashTable *
iso_table (const char *file, const char *list, const char *key)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (JsonParser) p = json_parser_new ();
  if (!json_parser_load_from_file (p, file, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return t;
  JsonObject *o = json_node_get_object (json_parser_get_root (p));
  JsonArray *a = json_object_has_member (o, list) ? json_object_get_array_member (o, list) : NULL;
  for (guint i = 0; a && i < json_array_get_length (a); i++) {
    JsonObject *e = json_array_get_object_element (a, i);
    const char *k = json_object_get_string_member_with_default (e, key, NULL);
    const char *k3 = json_object_get_string_member_with_default (e, "alpha_3", NULL);
    const char *n = json_object_get_string_member_with_default (e, "common_name", NULL);
    if (!n)
      n = json_object_get_string_member_with_default (e, "name", NULL);
    if (n && k)
      g_hash_table_insert (t, g_strdup (k), g_strdup (n));
    if (n && k3 && !g_hash_table_contains (t, k3))
      g_hash_table_insert (t, g_strdup (k3), g_strdup (n));
  }
  return t;
}

/* "de_DE.UTF-8" → "German (Germany)" */
static char *
locale_name (Names *n, const char *locale)
{
  g_autofree char *l = g_strdup (locale);
  char *cut = strpbrk (l, ".@");
  if (cut)
    *cut = 0;
  g_auto (GStrv) parts = g_strsplit (l, "_", 2);
  const char *lang = g_hash_table_lookup (n->langs, parts[0]);
  const char *country = parts[1] ? g_hash_table_lookup (n->countries, parts[1]) : NULL;
  if (!lang)
    return g_strdup (locale);
  return country ? g_strdup_printf ("%s (%s)", lang, country) : g_strdup (lang);
}

/* What the page shows: the installed UTF-8 locales, their names (labels: NULL-terminated, as ns_row_pick takes them)
 * and the user's language. Finding them out is slow: localectl reads every installed locale (0.7 s, and seconds for
 * the first look after a boot), AccountsService answers after that, and the Language row stood empty all the while
 * (owner, 11-21). So the last answer is kept, for the app's life and in ~/.cache/vibeos-settings/language, and the
 * page shows it at once; each time it opens it asks again in the background and redraws only if something changed.
 * With nothing kept yet (the first look ever), the Language row shows a loading indicator until the answer comes. */
typedef struct {
  GPtrArray *locales, *labels;
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
  g_free (k->language);
  g_free (k);
}

static Known *
known_new (void)
{
  Known *k = g_new0 (Known, 1);
  k->locales = g_ptr_array_new_with_free_func (g_free);
  k->labels = g_ptr_array_new_with_free_func (g_free);
  return k;
}

static char *
known_file (void)
{
  return g_build_filename (g_get_user_cache_dir (), "vibeos-settings", "language", NULL);
}

/* the cache: "language\t<it>", then "<locale>\t<its name>" a line each. A few kilobytes, read once. */
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
  for (guint i = 1; lines[i]; i++) {
    char *tab = strchr (lines[i], '\t');
    if (!tab || tab == lines[i] || !tab[1])
      continue;
    g_ptr_array_add (k->locales, g_strndup (lines[i], tab - lines[i]));
    g_ptr_array_add (k->labels, g_strdup (tab + 1));
  }
  g_ptr_array_add (k->labels, NULL);
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

/* keeps @k (taking it) as the last answer, and writes it out in the background */
static void
known_keep (Known *k)
{
  if (k != known) {
    known_free (known);
    known = k;
  }
  known_read = TRUE;
  GString *out = g_string_new (NULL);
  g_string_append_printf (out, "language\t%s\n", k->language ? k->language : "");
  for (guint i = 0; i < k->locales->len; i++)
    g_string_append_printf (out, "%s\t%s\n", (char *) k->locales->pdata[i], (char *) k->labels->pdata[i]);
  g_autofree char *file = known_file ();
  g_autofree char *dir = g_path_get_dirname (file);
  g_mkdir_with_parents (dir, 0700);
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
  GtkWidget *group, *logout;
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

static void
lang_set (GObject *src, GAsyncResult *res, gpointer what)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!v)
    g_warning ("%s: %s", (const char *) what, error->message);
}

static void
lang_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  const char *loc = l->locales->pdata[i];
  g_autoptr (GDBusConnection) sys = bus (G_BUS_TYPE_SYSTEM);
  if (!sys)
    return;
  if (l->path)
    g_dbus_connection_call (sys, "org.freedesktop.Accounts", l->path, "org.freedesktop.Accounts.User", "SetLanguage",
                            g_variant_new ("(s)", loc), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                            NULL, lang_set, (gpointer) "AccountsService SetLanguage");
  /* The language is the user's (AccountsService), as GNOME Settings sets it on Debian: the session takes it at the
   * next log in. Not the system's: Debian's systemd keeps localed read-only (org.freedesktop.locale1.read-only.conf
   * denies SetLocale to everyone, root too), so the SetLocale this page used to send was refused every time. */
  gtk_widget_set_visible (l->logout, TRUE);
  g_free (l->shown);
  l->shown = g_strdup (loc);
  if (known && same_strings (known->locales, l->locales)) {
    g_free (known->language);
    known->language = g_strdup (loc);
    known_keep (known);
  }
}

static void
region_picked (GtkWidget *row, guint i, gpointer data)
{
  Lang *l = data;
  GSettings *s = g_object_get_data (G_OBJECT (row), "ns-locale");
  g_settings_set_string (s, "region", i == 0 ? "" : (const char *) l->locales->pdata[i - 1]);
  gtk_widget_set_visible (l->logout, TRUE);
}

static void
logout (GtkWidget *row, gpointer data)
{
  g_autoptr (GDBusConnection) ses = bus (G_BUS_TYPE_SESSION);
  if (ses)
    g_dbus_connection_call (ses, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager",
                            "Logout", g_variant_new ("(u)", 0), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
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
  GtkWidget *r = ns_row_pick (l->group, "Language", (const char *const *) l->labels->pdata, sel, lang_picked, l);
  /* "C.UTF-8" and the like: no language chosen yet, programs speak their own (English) */
  if (sel == G_MAXUINT)
    ns_row_set_subtitle (r, !current || g_str_has_prefix (current, "C.") || !g_strcmp0 (current, "C")
                            || !g_strcmp0 (current, "POSIX") ? "System default (English)" : current);
  GSettings *s = ns_page_settings (page, "org.gnome.system.locale");
  if (s) {
    g_autoptr (GPtrArray) rl = g_ptr_array_new ();
    g_ptr_array_add (rl, (gpointer) "Same as the language");
    for (guint i = 0; i + 1 < l->labels->len; i++)
      g_ptr_array_add (rl, l->labels->pdata[i]);
    g_ptr_array_add (rl, NULL);
    g_autofree char *region = g_settings_get_string (s, "region");
    guint rs = 0;
    for (guint i = 0; i < l->locales->len; i++)
      if (!g_strcmp0 (l->locales->pdata[i], region))
        rs = i + 1;
    r = ns_row_pick (l->group, "Formats", (const char *const *) rl->pdata, rs, region_picked, l);
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
  gboolean same = same_strings (l->locales, k->locales) && !g_strcmp0 (l->shown, k->language);
  known_keep (k);
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

/* in a thread: the installed locales from localectl, the UTF-8 ones that name a language, with their names */
static void
locales_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  const char *argv[] = { "localectl", "list-locales", "--no-pager", NULL };
  g_autofree char *out = NULL;
  g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                NULL, NULL);
  Names n = { iso_table ("/usr/share/iso-codes/json/iso_639-2.json", "639-2", "alpha_2"),
              iso_table ("/usr/share/iso-codes/json/iso_3166-1.json", "3166-1", "alpha_2") };
  Known *k = known_new ();
  g_auto (GStrv) lines = g_strsplit (out ? out : "", "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const char *loc = g_strstrip (lines[i]);
    if (!*loc || !strstr (loc, "UTF-8") || g_str_has_prefix (loc, "C."))
      continue;
    g_ptr_array_add (k->locales, g_strdup (loc));
    g_ptr_array_add (k->labels, locale_name (&n, loc));
  }
  g_ptr_array_add (k->labels, NULL);
  g_hash_table_unref (n.langs);
  g_hash_table_unref (n.countries);
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
  NsPage *page = ns_page_new ("Language & region", &c);
  Lang *l = g_new0 (Lang, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-lang", l, (GDestroyNotify) lang_free);
  l->group = ns_group (c, NULL);
  Known *k = known_get ();
  if (k)
    lang_rows (page, k);
  else
    ns_row_suffix (ns_row (l->group, NULL, "Language", NULL), ns_spinner_new ());
  ns_note (c, "A new language or format shows in apps after you log out and in again.");
  GtkWidget *g = ns_group (c, NULL);
  l->logout = ns_row_button (g, "Log out now", FALSE, logout, NULL);
  gtk_widget_set_visible (l->logout, FALSE);
  g_autoptr (GTask) t = g_task_new (NULL, NULL, locales_listed, g_object_ref (page));
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
  NsPage *regions = ns_page_new ("Time zone", &c);
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
  NsPage *page = ns_page_new ("Date & time", &c);
  Time *t = g_new0 (Time, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-time", t, (GDestroyNotify) time_free);
  GtkWidget *g = ns_group (c, NULL);
  t->ntp = ns_row_switch (g, "Automatic date and time", "From the network", NULL, NULL);
  g_signal_connect (ns_row_get_switch (t->ntp), "notify::active", G_CALLBACK (ntp_set), NULL);
  GSettings *dt = ns_page_settings (page, "org.gnome.desktop.datetime");
  if (ns_has_key (dt, "automatic-timezone")) {
    g_autofree char *from = g_strdup_printf ("From the %s's location", ns_device_noun ());
    ns_row_switch (g, "Automatic time zone", from, dt, "automatic-timezone");
  }
  t->zone = ns_row (g, NULL, "Time zone", NULL);
  ns_row_on_activate (t->zone, zones_page, page);

  GSettings *iface = ns_page_settings (page, "org.gnome.desktop.interface");
  g = ns_group (c, "Clock");
  /* clock-format is read by the status bar and the lock screen (statusbar.js, keyguard.js) and by the apps.
   * No weekday, date or seconds switches (clock-show-*): the status bar shows the time alone. */
  GtkWidget *r = ns_row_switch (g, "24-hour time", "13:00 rather than 1:00 PM", NULL, NULL);
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
  ns_ask (row, "Your name", NULL, g_object_get_data (G_OBJECT (row), "ns-name"), FALSE, "Save", real_name_set,
          g_object_ref (row));
}

typedef struct {
  GtkWidget *name, *login, *type, *others;
} Users;

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
    ns_row (group, "avatar-default-symbolic", *real ? real : login, login);
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
    ns_row_set_subtitle (u->name, *real ? real : "Not set");
    g_object_set_data_full (G_OBJECT (u->name), "ns-name", g_strdup (real), g_free);
    ns_row_set_subtitle (u->login, login);
    ns_row_set_subtitle (u->type, type == 1 ? "Administrator" : "Standard");
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
  NsPage *page = ns_page_new ("Users", &c);
  Users *u = g_new0 (Users, 1);
  g_object_set_data_full (G_OBJECT (page), "ns-users", u, g_free);
  GtkWidget *g = ns_group (c, "Your account");
  u->name = ns_row (g, NULL, "Name", NULL);
  ns_row_on_activate (u->name, real_name_tapped, NULL);
  u->login = ns_row_info (g, "User name", g_get_user_name ());
  u->type = ns_row_info (g, "Account type", NULL);
  g = ns_group (c, NULL);
  GtkWidget *r = ns_row (g, NULL, "Screen lock and password", "A pattern or PIN is also your password");
  ns_row_on_activate (r, ns_page_lock_screen, NULL);
  u->others = ns_group (c, "Other users");
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
static const char *const sleep_labels[] = { "Never", "After 5 minutes", "After 10 minutes", "After 15 minutes",
                                            "After 30 minutes", "After 1 hour", NULL };

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
  NsPage *page = ns_page_new ("Power and screen blank", &c);
  GSettings *power = ns_page_settings (page, "org.gnome.settings-daemon.plugins.power");
  if (!power)
    return;
  GtkWidget *g;
  if (!ns_screen_off_sleeper ()) {
    g = ns_group (c, "Automatic suspend");
    sleep_row (g, power, "On battery", "battery");
    sleep_row (g, power, "Plugged in", "ac");
    g_autofree char *missed = g_strdup_printf ("Suspend saves the most power, but calls, messages and alarms can be "
                                              "missed while the %s sleeps.", ns_device_noun ());
    ns_note (c, missed);
  }
  g = ns_group (c, NULL);
  /* gnome-settings-daemon dims the backlight through the shell a little before the screen timeout */
  if (ns_has_key (power, "idle-dim"))
    ns_row_switch (g, "Dim the screen", "Before it turns off", power, "idle-dim");
  /* only with power-profiles-daemon, which switches the profile */
  if (ns_has_key (power, "power-saver-profile-on-low-battery") &&
      (g_file_test ("/usr/share/dbus-1/system-services/org.freedesktop.UPower.PowerProfiles.service",
                    G_FILE_TEST_EXISTS) ||
       g_file_test ("/usr/share/dbus-1/system-services/net.hadess.PowerProfiles.service", G_FILE_TEST_EXISTS)))
    ns_row_switch (g, "Power saving on low battery", NULL, power, "power-saver-profile-on-low-battery");
  if (ns_screen_off_sleeper ()) {
    g_autofree char *sleeps = g_strdup_printf ("This %s sleeps as soon as its screen is off.", ns_device_noun ());
    ns_note (c, sleeps);
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
  ns_confirm (w, "Set as wallpaper", "On the home screen and the lock screen", "Set", FALSE, apply_response, a);
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
  gtk_file_filter_set_name (ff, "Pictures");
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
  NsPage *page = ns_page_new ("Wallpapers", &c);
  GtkWidget *g = ns_group (c, NULL);
  ns_row_button (g, "Choose from your pictures", FALSE, from_files, NULL);

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
  NsPage *page = ns_page_new ("Software information", &c);
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
  ns_row_info (g, "Kernel", u.release);
  ns_row_info (g, "Kernel build", u.version);
  ns_row_info (g, "Settings", NS_VERSION);
  ns_push (page);
}

/* ---- Mouse, touchpad and hardware keyboard: mutter follows GNOME's peripherals settings ---- */

static void
speed_row (GtkWidget *g, GSettings *s)
{
  GtkWidget *scale;
  ns_row_scale (g, "Pointer speed", "input-mouse-symbolic", -1, 1, 0.1, &scale);
  g_settings_bind (s, "speed", gtk_range_get_adjustment (GTK_RANGE (scale)), "value", G_SETTINGS_BIND_DEFAULT);
}

static const guint delays[] = { 250, 500, 800 };
static const char *const delay_labels[] = { "Short", "Medium", "Long", NULL };
static const guint rates[] = { 15, 30, 60 };
static const char *const rate_labels[] = { "Fast", "Medium", "Slow", NULL };

static void
uint_picked (GtkWidget *row, guint i, gpointer s)
{
  const guint *values = g_object_get_data (G_OBJECT (row), "ns-uints");
  g_settings_set_uint (s, g_object_get_data (G_OBJECT (row), "ns-key"), values[i]);
}

static GtkWidget *
uint_row (GtkWidget *g, const char *title, GSettings *s, const char *key, const guint *values, guint n,
          const char *const *labels)
{
  guint v = g_settings_get_uint (s, key), sel = 0;
  for (guint i = 0; i < n; i++)
    if (ABS ((int) values[i] - (int) v) < ABS ((int) values[sel] - (int) v))
      sel = i;
  GtkWidget *r = ns_row_pick (g, title, labels, sel, uint_picked, s);
  g_object_set_data (G_OBJECT (r), "ns-uints", (gpointer) values);
  g_object_set_data (G_OBJECT (r), "ns-key", (gpointer) key);
  return r;
}

void
ns_page_mouse (GtkWidget *row, gpointer data)
{
  GtkWidget *c;
  NsPage *page = ns_page_new ("Mouse & touchpad", &c);
  GSettings *mouse = ns_page_settings (page, "org.gnome.desktop.peripherals.mouse");
  GSettings *pad = ns_page_settings (page, "org.gnome.desktop.peripherals.touchpad");
  GSettings *kbd = ns_page_settings (page, "org.gnome.desktop.peripherals.keyboard");
  if (mouse) {
    GtkWidget *g = ns_group (c, "Mouse");
    speed_row (g, mouse);
    ns_row_switch (g, "Natural scrolling", "The content moves with the wheel, like a touchscreen", mouse,
                   "natural-scroll");
    ns_row_switch (g, "Left-handed", "The right button is the main one", mouse, "left-handed");
  }
  if (pad) {
    GtkWidget *g = ns_group (c, "Touchpad");
    speed_row (g, pad);
    ns_row_switch (g, "Tap to click", "A tap counts as a click", pad, "tap-to-click");
    ns_row_switch (g, "Natural scrolling", "The content moves with your fingers", pad, "natural-scroll");
  }
  if (kbd) {
    GtkWidget *g = ns_group (c, "Hardware keyboard");
    ns_row_switch (g, "Repeat keys", "A held key types again and again", kbd, "repeat");
    GtkWidget *r = uint_row (g, "Delay before repeating", kbd, "delay", delays, G_N_ELEMENTS (delays), delay_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
    r = uint_row (g, "Repeat speed", kbd, "repeat-interval", rates, G_N_ELEMENTS (rates), rate_labels);
    g_settings_bind (kbd, "repeat", r, "sensitive", G_SETTINGS_BIND_GET);
  }
  ns_note (c, "For a Bluetooth or USB mouse, touchpad or keyboard.");
  ns_push (page);
}
