/* nb-app: the engine, the session, downloads and shortcuts. */
#include "nb.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

/* Ad blocking: EasyList in WebKit's content-blocker format, compiled once into WebKit's filter store (the network
 * process then blocks without asking us), fetched again once a week. A copy beside the app covers the first start.
 * Not from Jelly: the old browser's, kept. */
#define FILTER_URL "https://easylist-downloads.adblockplus.org/easylist_min_content_blocker.json"
#define FILTER_ID "easylist"
#define FILTER_MAX_AGE (7 * 24 * 3600)
#define MEMORY_LIMIT_MB 768
#define SHORTCUT_PREFIX NB_APP_ID ".shortcut-"

/* a phone's scrollbar: a thin rounded thumb over the page, no track and no arrow buttons */
static const char SCROLLBAR_CSS[] =
  "::-webkit-scrollbar{width:5px!important;height:5px!important;background:transparent!important}"
  "::-webkit-scrollbar-track,::-webkit-scrollbar-corner{background:transparent!important}"
  "::-webkit-scrollbar-button{display:none!important;width:0!important;height:0!important}"
  "::-webkit-scrollbar-thumb{background:rgba(128,128,128,.55)!important;border-radius:3px!important;"
  "border:none!important}";

const char *const nb_engine_names[] = { "Baidu", "Bing", "Brave", "DuckDuckGo", "Ecosia", "GOOD", "Google",
                                        "Startpage", "Yahoo", "Yandex", "Qwant", NULL };
const char *const nb_engine_urls[] = {
  "https://www.baidu.com/s?wd={searchTerms}",
  "https://www.bing.com/search?q={searchTerms}",
  "https://search.brave.com/search?q={searchTerms}",
  "https://duckduckgo.com/?q={searchTerms}",
  "https://www.ecosia.org/search?method=index&q={searchTerms}",
  "https://good-search.org/search/?q={searchTerms}",
  NB_DEFAULT_ENGINE,
  "https://www.startpage.com/sp/search?q={searchTerms}",
  "https://search.yahoo.com/search?p={searchTerms}",
  "https://yandex.com/?q={searchTerms}",
  "https://www.qwant.com/?l=en&q={searchTerms}",
  NULL };

NbApp *nb;

/* ---- the ad filter ---- */

static void
apply_filter (WebKitUserContentFilter *f)
{
  if (nb->filter) {
    webkit_user_content_manager_remove_filter (nb->ucm, nb->filter);
    webkit_user_content_filter_unref (nb->filter);
  }
  nb->filter = f;
  if (f)
    webkit_user_content_manager_add_filter (nb->ucm, f);
}

static void fetch_filter (void);

static void
on_filter_saved (GObject *src, GAsyncResult *res, gpointer data)
{
  gboolean fetched = GPOINTER_TO_INT (data);
  g_autoptr (GError) error = NULL;
  WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish (WEBKIT_USER_CONTENT_FILTER_STORE (src),
                                                                             res, &error);
  if (!f) {
    g_message ("vibeos-browser: ad filter not compiled: %s", error->message);
    return;
  }
  /* the bundled copy: fetch again soon */
  nb_store_set_filter_time (nb->store, fetched ? g_get_real_time () / 1e6 : 1);
  if (nb_store_adblock (nb->store))
    apply_filter (f);
  else
    webkit_user_content_filter_unref (f);
  if (!fetched)
    fetch_filter ();
}

static void
compile_filter (GBytes *bytes, gboolean fetched)
{
  webkit_user_content_filter_store_save (nb->filters, FILTER_ID, bytes, NULL, on_filter_saved,
                                         GINT_TO_POINTER (fetched));
}

static void
on_filter_fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (!bytes)
    g_message ("vibeos-browser: ad filter not fetched: %s", error->message);
  else if (!msg || soup_message_get_status (msg) != 200 || g_bytes_get_size (bytes) < 10000)
    g_message ("vibeos-browser: ad filter not fetched: %zu bytes", g_bytes_get_size (bytes));
  else
    compile_filter (bytes, TRUE);
  g_object_unref (src);
}

static void
fetch_filter (void)
{
  SoupSession *session = soup_session_new_with_options ("timeout", 60, NULL);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", FILTER_URL);
  soup_session_send_and_read_async (session, msg, G_PRIORITY_LOW, NULL, on_filter_fetched, NULL);
}

/* the compiled filter if there is one (and fresh), else compile the copy beside the app or fetch the list */
static void
on_filter_loaded (GObject *src, GAsyncResult *res, gpointer data)
{
  WebKitUserContentFilter *f = webkit_user_content_filter_store_load_finish (WEBKIT_USER_CONTENT_FILTER_STORE (src),
                                                                             res, NULL);
  apply_filter (f);
  double stamp = nb_store_filter_time (nb->store);
  g_autofree char *bundled = g_build_filename (NB_DATADIR, "easylist.json", NULL);
  if (!nb->filter && !stamp && g_file_test (bundled, G_FILE_TEST_EXISTS)) {
    g_autoptr (GMappedFile) mapped = g_mapped_file_new (bundled, FALSE, NULL);
    if (mapped) {
      g_autoptr (GBytes) bytes = g_mapped_file_get_bytes (mapped);
      compile_filter (bytes, FALSE);
      return;
    }
  }
  if (g_get_real_time () / 1e6 - stamp > FILTER_MAX_AGE)
    fetch_filter ();
}

/* ---- the engine ---- */

static void
on_download_started (WebKitNetworkSession *s, WebKitDownload *d, gpointer data)
{
  nb_track_download (d);
}

static void
cookie_policy (WebKitNetworkSession *s, gboolean incognito)
{
  /* cookies: on unless incognito or turned off in Settings */
  gboolean on = !incognito && nb_store_get_bool (nb->store, "cookies", TRUE);
  webkit_cookie_manager_set_accept_policy (webkit_network_session_get_cookie_manager (s),
                                           on ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY
                                              : WEBKIT_COOKIE_POLICY_ACCEPT_NEVER);
}

WebKitNetworkSession *
nb_session_for (gboolean incognito)
{
  if (!incognito)
    return nb->session;
  if (!nb->incog_session) {
    nb->incog_session = webkit_network_session_new_ephemeral ();
    g_signal_connect (nb->incog_session, "download-started", G_CALLBACK (on_download_started), NULL);
    cookie_policy (nb->incog_session, TRUE);
  }
  return nb->incog_session;
}

static void
setup_engine (void)
{
  /* a heavy page grows its web process without end: past 768 MB WebKit kills and reloads it */
  WebKitMemoryPressureSettings *m = webkit_memory_pressure_settings_new ();
  webkit_memory_pressure_settings_set_memory_limit (m, MEMORY_LIMIT_MB);
  webkit_network_session_set_memory_pressure_settings (m);
  webkit_memory_pressure_settings_free (m);

  g_autofree char *web = g_build_filename (nb->data_dir, "web", NULL);
  g_autofree char *cookies = g_build_filename (nb->data_dir, "cookies.sqlite", NULL);
  g_mkdir_with_parents (web, 0700);
  nb->session = webkit_network_session_new (web, nb->cache_dir);
  webkit_cookie_manager_set_persistent_storage (webkit_network_session_get_cookie_manager (nb->session), cookies,
                                                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_website_data_manager_set_favicons_enabled (webkit_network_session_get_website_data_manager (nb->session),
                                                    TRUE);
  g_signal_connect (nb->session, "download-started", G_CALLBACK (on_download_started), NULL);
  cookie_policy (nb->session, FALSE);

  nb->ucm = webkit_user_content_manager_new ();
  WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new (SCROLLBAR_CSS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                             WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
  webkit_user_content_manager_add_style_sheet (nb->ucm, sheet);
  webkit_user_style_sheet_unref (sheet);
  nb_refresh_setup ();
  g_autofree char *filters = g_build_filename (nb->data_dir, "filters", NULL);
  nb->filters = webkit_user_content_filter_store_new (filters);
  if (nb_store_adblock (nb->store))
    webkit_user_content_filter_store_load (nb->filters, FILTER_ID, NULL, on_filter_loaded, NULL);
}

void
nb_apply_settings (void)
{
  cookie_policy (nb->session, FALSE);
  if (nb->incog_session)
    cookie_policy (nb->incog_session, TRUE);
  for (guint i = 0; i < nb->tabs->len; i++) {
    NbTab *t = nb->tabs->pdata[i];
    nb_apply_view_settings (t);
    nb_tab_apply_reach (t);
  }
}

static void
on_cookies_cleared (GObject *src, GAsyncResult *res, gpointer data)
{
  webkit_website_data_manager_clear_finish (WEBKIT_WEBSITE_DATA_MANAGER (src), res, NULL);
}

void
nb_clear_cookies (void)
{
  /* every cookie, of every site */
  webkit_website_data_manager_clear (webkit_network_session_get_website_data_manager (nb->session),
                                     WEBKIT_WEBSITE_DATA_COOKIES, 0, NULL, on_cookies_cleared, NULL);
}

/* ---- preferences ---- */

const char *
nb_home_page (void)
{
  return nb_store_get_string (nb->store, "homePage", NB_DEFAULT_HOME);
}

gboolean
nb_home_autoload (void)
{
  return nb_store_get_bool (nb->store, "homePageAutoload", TRUE);
}

/* what the user typed as a URL: a scheme → as it is; one word that looks like a web address → https://…; anything else
 * → the search engine */
char *
nb_smart_url (const char *text)
{
  g_autofree char *s = g_strstrip (g_strdup (text ? text : ""));

  if (!*s)
    return NULL;
  if (g_regex_match_simple ("^[a-zA-Z][a-zA-Z0-9+.-]*:", s, 0, 0) &&
      !g_regex_match_simple ("^[^\\s/]+\\.[a-z]{2,}:\\d+", s, G_REGEX_CASELESS, 0) &&
      !g_regex_match_simple ("^localhost:\\d+", s, G_REGEX_CASELESS, 0))
    return g_strdup (s);
  if (!g_regex_match_simple ("\\s", s, 0, 0) &&
      (g_regex_match_simple ("^[^\\s/?#]+\\.[a-z]{2,63}(:\\d+)?([/?#].*)?$", s, G_REGEX_CASELESS, 0) ||
       g_regex_match_simple ("^\\d{1,3}(\\.\\d{1,3}){3}(:\\d+)?([/?#].*)?$", s, 0, 0) ||
       g_regex_match_simple ("^localhost(:\\d+)?([/?#].*)?$", s, G_REGEX_CASELESS, 0)))
    return g_strdup_printf ("https://%s", s);
  const char *engine = nb_store_get_string (nb->store, "searchEngine", NB_DEFAULT_ENGINE);
  g_autofree char *q = g_uri_escape_string (s, NULL, TRUE);
  g_auto (GStrv) parts = g_strsplit (engine, "{searchTerms}", -1);
  return g_strjoinv (q, parts);
}

/* ---- the session: the non-incognito tabs, the last one used marked ---- */

static void
write_session (void)
{
  /* the window is going: its last write kept every tab, which go one by one after it */
  if (nb->quitting)
    return;
  JsonArray *tabs = json_array_new ();
  int current = 0, n = 0;

  for (guint i = 0; i < nb->tabs->len; i++) {
    NbTab *t = nb->tabs->pdata[i];
    const char *url = nb_tab_uri (t);
    if (t->incognito || t->closing || !url || !*url)
      continue;
    if (t == nb->last_active)
      current = n;
    JsonObject *o = json_object_new ();
    json_object_set_string_member (o, "url", url);
    json_object_set_string_member (o, "title", nb_tab_title (t));
    if (t->desktop)
      json_object_set_boolean_member (o, "desktop", TRUE);
    json_array_add_object_element (tabs, o);
    n++;
  }
  JsonObject *root = json_object_new ();
  json_object_set_array_member (root, "tabs", tabs);
  json_object_set_int_member (root, "current", current);
  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, root);
  nb_store_write_node (nb->store, "session.json", node);
  json_node_unref (node);
}

static gboolean
on_session_timeout (gpointer data)
{
  nb->session_id = 0;
  write_session ();
  return G_SOURCE_REMOVE;
}

void
nb_save_session (void)
{
  if (!nb->session_id && !nb->quitting)
    nb->session_id = g_timeout_add_full (G_PRIORITY_LOW, 500, on_session_timeout, NULL, NULL);
}

void
nb_save_session_now (void)
{
  g_clear_handle_id (&nb->session_id, g_source_remove);
  write_session ();
}

static gboolean
restore_session (void)
{
  g_autoptr (JsonNode) node = nb_store_load (nb->store, "session.json");
  if (!node || !JSON_NODE_HOLDS_OBJECT (node))
    return FALSE;
  JsonObject *root = json_node_get_object (node);
  JsonArray *tabs = json_object_has_member (root, "tabs") ? json_object_get_array_member (root, "tabs") : NULL;
  if (!tabs || !json_array_get_length (tabs))
    return FALSE;
  guint len = json_array_get_length (tabs);
  guint current = MIN ((guint) MAX (0, json_object_get_int_member_with_default (root, "current", 0)), len - 1);
  NbTab *top = NULL;
  for (guint i = 0; i < len; i++) {
    JsonNode *n = json_array_get_element (tabs, i);
    if (!JSON_NODE_HOLDS_OBJECT (n))
      continue;
    JsonObject *o = json_node_get_object (n);
    const char *url = json_object_get_string_member_with_default (o, "url", NULL);
    if (!url || !*url)
      continue;
    NbTab *t = nb_tab_new (url, FALSE, json_object_get_boolean_member_with_default (o, "desktop", FALSE), NULL, NULL);
    g_free (t->title);
    t->title = g_strdup (json_object_get_string_member_with_default (o, "title", ""));
    if (i == current || !top)
      top = t;
  }
  /* the one used last is shown; the others load when they are picked in the switcher */
  if (top)
    nb_tab_present (top);
  return nb->tabs->len > 0;
}

void
nb_app_activate (void)
{
  if (nb->tabs->len) {
    /* the browser as it was left: the tab, or the switcher over the tabs */
    if (nb->switcher_open)
      gtk_window_present (GTK_WINDOW (nb_window_get ()));
    else
      nb_tab_present (nb->last_active ? nb->last_active : nb->tabs->pdata[nb->tabs->len - 1]);
    return;
  }
  if (!restore_session ())
    nb_open_tab (NULL, FALSE, NULL, NULL);
}

void
nb_app_open (const char *const *urls, gboolean incognito, const char *shortcut_id)
{
  /* a cold start brings the saved tabs back first, the new one goes on top */
  if (!nb->tabs->len)
    restore_session ();
  if (!urls || !urls[0]) {
    nb_open_tab (NULL, incognito, shortcut_id, NULL);
    return;
  }
  for (guint i = 0; urls[i]; i++)
    nb_open_tab (urls[i], incognito, shortcut_id, NULL);
}

NbTab *
nb_open_tab (const char *url, gboolean incognito, const char *shortcut_id, WebKitWebView *related)
{
  g_auto (GStrv) desktop_ids = nb_store_get_strv (nb->store, "desktopShortcuts");
  gboolean desktop = shortcut_id && g_strv_contains ((const char *const *) desktop_ids, shortcut_id);
  /* the URL asked for, else the home page when it autoloads, else nothing */
  const char *load = url && *url ? url : related ? NULL : nb_home_autoload () ? nb_home_page () : NULL;
  NbTab *t = nb_tab_new (load, incognito, desktop, shortcut_id, related);

  nb_tab_present (t);
  nb_save_session ();
  return t;
}

void
nb_tab_forget (NbTab *t)
{
  g_ptr_array_remove (nb->tabs, t);
  if (nb->last_active == t)
    nb->last_active = nb->tabs->len ? nb->tabs->pdata[nb->tabs->len - 1] : NULL;
  gboolean incognito_left = FALSE;
  for (guint i = 0; i < nb->tabs->len; i++)
    incognito_left |= ((NbTab *) nb->tabs->pdata[i])->incognito;
  /* nothing of the incognito tabs is kept once the last one is gone */
  if (!incognito_left)
    g_clear_object (&nb->incog_session);
  /* now: the last tab closed by its page ends the process */
  nb_save_session_now ();
}

/* ---- downloads: to ~/Downloads, a notification when done ---- */

static NbDownload *
download_of (WebKitDownload *d)
{
  for (guint i = 0; i < nb->downloads->len; i++)
    if (((NbDownload *) nb->downloads->pdata[i])->d == d)
      return nb->downloads->pdata[i];
  return NULL;
}

void
nb_downloads_changed (void)
{
  for (guint i = 0; i < nb->tabs->len; i++) {
    NbTab *t = nb->tabs->pdata[i];
    if (t->downloads_page)
      nb_downloads_page_refresh (t->downloads_page);
  }
}

static gboolean
on_decide_destination (WebKitDownload *d, const char *suggested, gpointer data)
{
  NbDownload *e = download_of (d);
  if (!e)
    return FALSE;
  g_mkdir_with_parents (nb->downloads_dir, 0755);
  g_autofree char *base = g_strdup (suggested && *suggested ? suggested : "download");
  g_strdelimit (base, "/", '_');
  const char *dot = strrchr (base, '.');
  gsize stem = dot && dot != base ? (gsize) (dot - base) : strlen (base);
  g_autofree char *path = g_build_filename (nb->downloads_dir, base, NULL);
  for (int n = 1; g_file_test (path, G_FILE_TEST_EXISTS); n++) {
    g_autofree char *stem_s = g_strndup (base, stem);
    g_autofree char *name = g_strdup_printf ("%s (%d)%s", stem_s, n, base + stem);
    g_free (path);
    path = g_build_filename (nb->downloads_dir, name, NULL);
  }
  webkit_download_set_destination (d, path);
  g_free (e->path);
  e->path = g_strdup (path);
  g_free (e->name);
  e->name = g_path_get_basename (path);
  WebKitURIResponse *r = webkit_download_get_response (d);
  if (r && webkit_uri_response_get_mime_type (r)) {
    g_free (e->mime);
    e->mime = g_strdup (webkit_uri_response_get_mime_type (r));
  }
  nb_downloads_changed ();
  return TRUE;
}

static void
on_download_failed (WebKitDownload *d, GError *error, gpointer data)
{
  NbDownload *e = download_of (d);
  if (e && !g_error_matches (error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
    e->failed = TRUE;
    nb_downloads_changed ();
  }
}

static void
on_download_finished (WebKitDownload *d, gpointer data)
{
  NbDownload *e = download_of (d);
  if (!e)
    return;
  e->done = TRUE;
  e->size = webkit_download_get_received_data_length (d);
  e->time = g_get_real_time () / G_USEC_PER_SEC;
  nb_downloads_changed ();
  if (e->failed || !e->path)
    return;
  /* a finished download's notification: the file's name, "Download complete"; a tap opens it */
  g_autoptr (GNotification) n = g_notification_new (e->name);
  g_notification_set_body (n, "Download complete");
  g_notification_set_default_action_and_target (n, "app.open-file", "s", e->path);
  g_autofree char *id = g_strdup_printf ("download-%s", e->path);
  g_application_send_notification (G_APPLICATION (nb->gapp), id, n);
}

static void
on_download_progress (WebKitDownload *d, GParamSpec *pspec, gpointer data)
{
  nb_downloads_changed ();
}

void
nb_track_download (WebKitDownload *d)
{
  if (!d || g_hash_table_contains (nb->dl_seen, d))
    return;
  g_hash_table_add (nb->dl_seen, g_object_ref (d));
  NbDownload *e = g_new0 (NbDownload, 1);
  e->d = d;
  e->time = g_get_real_time () / G_USEC_PER_SEC;
  g_ptr_array_insert (nb->downloads, 0, e);
  g_signal_connect (d, "decide-destination", G_CALLBACK (on_decide_destination), NULL);
  g_signal_connect (d, "notify::estimated-progress", G_CALLBACK (on_download_progress), NULL);
  g_signal_connect (d, "failed", G_CALLBACK (on_download_failed), NULL);
  g_signal_connect (d, "finished", G_CALLBACK (on_download_finished), NULL);
}

static void
download_free (gpointer data)
{
  NbDownload *e = data;
  g_free (e->name);
  g_free (e->path);
  g_free (e->mime);
  g_free (e);
}

/* the files already in ~/Downloads, newest first after this session's own */
void
nb_scan_downloads (void)
{
  g_autoptr (GFile) dir = g_file_new_for_path (nb->downloads_dir);
  g_autoptr (GFileEnumerator) en = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                              G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                                              G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                                              G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
                                                              G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                                              G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                              G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (!en)
    return;
  /* forget the files found last time: they are listed again as they are now */
  for (guint i = nb->downloads->len; i-- > 0;)
    if (!((NbDownload *) nb->downloads->pdata[i])->d)
      g_ptr_array_remove_index (nb->downloads, i);
  g_autoptr (GPtrArray) found = g_ptr_array_new ();
  GFileInfo *info;
  while ((info = g_file_enumerator_next_file (en, NULL, NULL))) {
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR && !g_file_info_get_is_hidden (info)) {
      g_autofree char *path = g_build_filename (nb->downloads_dir, g_file_info_get_name (info), NULL);
      gboolean listed = FALSE;
      for (guint i = 0; i < nb->downloads->len && !listed; i++)
        listed = !g_strcmp0 (((NbDownload *) nb->downloads->pdata[i])->path, path);
      if (!listed) {
        NbDownload *e = g_new0 (NbDownload, 1);
        e->name = g_strdup (g_file_info_get_name (info));
        e->path = g_steal_pointer (&path);
        e->size = g_file_info_get_size (info);
        e->mime = g_strdup (g_file_info_get_content_type (info));
        g_autoptr (GDateTime) mtime = g_file_info_get_modification_date_time (info);
        e->time = mtime ? g_date_time_to_unix (mtime) : 0;
        e->done = TRUE;
        g_ptr_array_add (found, e);
      }
    }
    g_object_unref (info);
  }
  /* newest first */
  for (guint i = 0; i < found->len; i++)
    for (guint j = i + 1; j < found->len; j++)
      if (((NbDownload *) found->pdata[j])->time > ((NbDownload *) found->pdata[i])->time) {
        gpointer x = found->pdata[i];
        found->pdata[i] = found->pdata[j];
        found->pdata[j] = x;
      }
  for (guint i = 0; i < found->len; i++)
    g_ptr_array_add (nb->downloads, found->pdata[i]);
}

/* ---- shortcuts: a desktop entry the launcher puts on the home screen ---- */

void
nb_shortcut_free (NbShortcut *s)
{
  g_free (s->id);
  g_free (s->name);
  g_free (s->url);
  g_free (s->desktop_file);
  g_free (s);
}

static char *
applications_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "applications", NULL);
}

/* a desktop entry's Exec argument: quoted, with the characters the Desktop Entry Specification reserves escaped,
 * and %% for % */
static char *
exec_quote (const char *arg)
{
  GString *s = g_string_new ("\"");
  for (const char *p = arg; *p; p++) {
    if (*p == '"' || *p == '`' || *p == '$' || *p == '\\')
      g_string_append_c (s, '\\');   /* one: GKeyFile doubles it on the way out */
    if (*p == '%')
      g_string_append_c (s, '%');
    g_string_append_c (s, *p);
  }
  g_string_append_c (s, '"');
  return g_string_free (s, FALSE);
}

/* a shortcut's icon: the favicon drawn in the middle of a white circle, 192 px */
static gboolean
write_icon (GdkTexture *favicon, const char *path)
{
  const int size = 192;
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create (surface);
  cairo_arc (cr, size / 2.0, size / 2.0, size / 2.0, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
  if (favicon) {
    int w = gdk_texture_get_width (favicon), h = gdk_texture_get_height (favicon);
    cairo_surface_t *icon = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush (icon);
    gdk_texture_download (favicon, cairo_image_surface_get_data (icon), cairo_image_surface_get_stride (icon));
    cairo_surface_mark_dirty (icon);
    double target = size * 0.5, scale = target / MAX (w, h);
    cairo_translate (cr, (size - w * scale) / 2, (size - h * scale) / 2);
    cairo_scale (cr, scale, scale);
    cairo_set_source_surface (cr, icon, 0, 0);
    cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
    cairo_paint (cr);
    cairo_surface_destroy (icon);
  }
  cairo_destroy (cr);
  gboolean ok = cairo_surface_write_to_png (surface, path) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy (surface);
  return ok;
}

gboolean
nb_add_shortcut (const char *name, const char *url, GdkTexture *favicon, GError **error)
{
  g_autofree char *id = g_strdup_printf ("%" G_GINT64_FORMAT, g_get_real_time () / 1000);
  g_autofree char *icons = g_build_filename (nb->data_dir, "shortcuts", NULL);
  g_autofree char *icon_file = g_strdup_printf ("%s.png", id);
  g_autofree char *icon = g_build_filename (icons, icon_file, NULL);
  g_autofree char *apps = applications_dir ();
  g_autofree char *file_name = g_strdup_printf (SHORTCUT_PREFIX "%s.desktop", id);
  g_autofree char *file = g_build_filename (apps, file_name, NULL);
  g_autofree char *q_id = exec_quote (id);
  g_autofree char *q_url = exec_quote (url);
  const char *icon_value = "vibeos.Browser";

  g_mkdir_with_parents (icons, 0700);
  g_mkdir_with_parents (apps, 0755);
  /* without a favicon: the app's own icon */
  if (favicon && write_icon (favicon, icon))
    icon_value = icon;
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, "Application");
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, name && *name ? name : url);
  g_autofree char *exec = g_strdup_printf ("vibeos-browser --shortcut=%s %s", q_id, q_url);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, icon_value);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_STARTUP_WM_CLASS, NB_APP_ID);
  g_key_file_set_boolean (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TERMINAL, FALSE);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, "X-VibeosBrowser-Shortcut", id);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, "X-VibeosBrowser-Url", url);
  g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP, "X-Purism-FormFactor", "Workstation;Mobile;");
  return g_key_file_save_to_file (kf, file, error);
}

GPtrArray *
nb_list_shortcuts (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) nb_shortcut_free);
  g_autofree char *apps = applications_dir ();
  g_autoptr (GDir) dir = g_dir_open (apps, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name (dir))) {
    if (!g_str_has_prefix (name, SHORTCUT_PREFIX) || !g_str_has_suffix (name, ".desktop"))
      continue;
    g_autofree char *path = g_build_filename (apps, name, NULL);
    g_autoptr (GKeyFile) kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
      continue;
    NbShortcut *s = g_new0 (NbShortcut, 1);
    s->id = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, "X-VibeosBrowser-Shortcut", NULL);
    s->name = g_key_file_get_locale_string (kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
    s->url = g_key_file_get_string (kf, G_KEY_FILE_DESKTOP_GROUP, "X-VibeosBrowser-Url", NULL);
    s->desktop_file = g_steal_pointer (&path);
    if (s->id && s->name)
      g_ptr_array_add (out, s);
    else
      nb_shortcut_free (s);
  }
  return out;
}

/* ---- the application ---- */

static void
on_open_file (GSimpleAction *action, GVariant *param, gpointer data)
{
  g_autoptr (GFile) file = g_file_new_for_path (g_variant_get_string (param, NULL));
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
  NbTab *t = nb->last_active;
  gtk_file_launcher_launch (launcher, t ? GTK_WINDOW (t->win) : NULL, NULL, NULL, NULL);
}

void
nb_app_startup (GtkApplication *app)
{
  nb = g_new0 (NbApp, 1);
  nb->gapp = app;
  nb->data_dir = g_build_filename (g_get_user_data_dir (), NB_APP_ID, NULL);
  nb->cache_dir = g_build_filename (g_get_user_cache_dir (), NB_APP_ID, NULL);
  const char *dl = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
  nb->downloads_dir = dl ? g_strdup (dl) : g_build_filename (g_get_home_dir (), "Downloads", NULL);
  g_autofree char *old = g_build_filename (g_get_user_data_dir (), "neobrowser", NULL);
  nb->store = nb_store_new (nb->data_dir, old);
  nb->tabs = g_ptr_array_new ();
  nb->downloads = g_ptr_array_new_with_free_func (download_free);
  nb->dl_seen = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  setup_engine ();

  static const GActionEntry actions[] = { { "open-file", on_open_file, "s", NULL, NULL, { 0 } } };
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), NULL);
}

void
nb_app_shutdown (void)
{
  if (!nb)
    return;
  if (nb->session_id) {
    g_clear_handle_id (&nb->session_id, g_source_remove);
    write_session ();
  }
  nb_store_free (nb->store);
}
