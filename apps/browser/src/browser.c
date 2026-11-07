/* Neo Browser: Chrome for Android's phone UI (dark) over WebKitGTK 6, in C. The port of the GJS de.yesman.NeoBrowser
 * (yesman gnome/neobrowser), same pages, same data:
 * toolbar (home, address pill, tab count, menu) • tab switcher grid • menu sheet • address entry with suggestions •
 * new-tab page • history / bookmarks / downloads pages • find in page • long-press link sheet • ad blocking.
 *
 * Session, history and bookmarks are JSON under ~/.local/share/de.yesman.NeoBrowser (store.c); downloads go to
 * ~/Downloads. Lists scroll with libspongy's stretch when it is there.
 *
 * The phone's back swipe (gnome-phone's a11y.js) clicks a showing "Back" button when the app has one, and otherwise
 * sends the app home. Chrome has no back button on screen, so the window carries an invisible one that is
 * sensitive exactly while back has somewhere to go here: a sheet, a page over the browser, find, the page's own
 * history, another tab. At the start of everything it is off and the swipe goes home, as Chrome's does. */
#include <math.h>
#include <string.h>

#include <adwaita.h>
#include <libsoup/soup.h>
#include <webkit/webkit.h>
#ifdef HAVE_SPONGY
#include <spongy-gtk.h>
#endif

#include "store.h"

#define APP_ID "de.yesman.NeoBrowser"
/* The engine is WebKit, so the page is told so: an iPhone's Safari gets every site's mobile layout and no "your
 * browser is not supported" for a Chrome name over a WebKit engine. Desktop mode is Safari on a Mac. */
#define MOBILE_UA "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                  "Version/18.0 Mobile/15E148 Safari/604.1"
#define DESKTOP_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                   "Version/18.0 Safari/605.1.15"
#define SEARCH_URL "https://www.google.com/search?q="
/* Ad blocking: EasyList in WebKit's content-blocker format, compiled once into WebKit's filter store (the network
 * process then blocks without asking us), fetched again once a week. A copy beside the app covers the first start. */
#define FILTER_URL "https://easylist-downloads.adblockplus.org/easylist_min_content_blocker.json"
#define FILTER_ID "easylist"
#define FILTER_MAX_AGE (7 * 24 * 3600)
#define MEMORY_LIMIT_MB 768

/* Android's scrollbar: a thin rounded thumb over the page, no track and no arrow buttons */
static const char SCROLLBAR_CSS[] =
  "::-webkit-scrollbar{width:5px!important;height:5px!important;background:transparent!important}"
  "::-webkit-scrollbar-track,::-webkit-scrollbar-corner{background:transparent!important}"
  "::-webkit-scrollbar-button{display:none!important;width:0!important;height:0!important}"
  "::-webkit-scrollbar-thumb{background:rgba(128,128,128,.55)!important;border-radius:3px!important;"
  "border:none!important}";

static const char CSS[] =
  "window.neo, .neo-bg { background: #121316; color: #e3e3e6; }"
  ".neo-bar { background: #1b1c1f; min-height: 56px; padding: 0 4px; }"
  ".neo-bar.incognito, .ntp.incognito { background: #3c3f45; }"
  ".ntp { background: #121316; }"
  "button.neo-icon { min-width: 40px; min-height: 40px; padding: 4px; margin: 4px; border-radius: 999px;"
  " background: none; box-shadow: none; }"
  "button.neo-icon:active { background: alpha(#e3e3e6, 0.12); }"
  "button.neo-icon:disabled { opacity: 0.38; }"
  "button.back-probe { min-width: 1px; min-height: 1px; padding: 0; margin: 0; border: none; background: none;"
  " box-shadow: none; opacity: 0; }"
  "button.pill { background: #2e3035; border-radius: 20px; min-height: 40px; padding: 0 14px 0 12px;"
  " font-weight: normal; font-size: 16px; }"
  ".pill-box { background: #2e3035; border-radius: 20px; min-height: 40px; padding: 0 0 0 14px; }"
  ".tabcount { border: 2px solid #e3e3e6; border-radius: 5px; min-width: 18px; min-height: 18px;"
  " font-weight: bold; font-size: 12px; }"
  ".tabcount.small { font-size: 10px; }"
  "progressbar.neo-progress trough { min-height: 2px; background: #3a3c41; border-radius: 0; }"
  "progressbar.neo-progress progress { min-height: 2px; background: #a8c7fa; border-radius: 0; }"
  "entry.neo-entry, entry.neo-entry:focus-within { background: none; border: none; box-shadow: none;"
  " outline: none; min-height: 40px; font-size: 16px; color: #e3e3e6; padding: 0; }"
  ".sheet { background: #2b2d31; border-radius: 24px; padding: 8px 0; }"
  ".sheet.bottom { border-radius: 24px 24px 0 0; padding: 16px 0 8px 0; }"
  ".scrim { background: alpha(black, 0.5); }"
  ".scrim-clear { background: transparent; }"
  "button.menu-row { border-radius: 0; min-height: 52px; padding: 0 20px; font-size: 16px; font-weight: normal;"
  " background: none; box-shadow: none; }"
  "button.menu-row:active { background: alpha(#e3e3e6, 0.08); }"
  ".menu-top button { background: #3b3d42; border-radius: 26px; min-width: 52px; min-height: 52px;"
  " margin: 4px 2px; }"
  ".menu-top button:disabled { opacity: 0.38; }"
  ".divider { background: #3a3c41; min-height: 1px; }"
  ".ntp-title { font-size: 44px; font-weight: 500; }"
  "button.search-pill { background: #2e3035; border-radius: 26px; min-height: 52px; padding: 0 18px;"
  " color: #a9abb1; font-size: 16px; font-weight: normal; }"
  ".site-dot { background: #2e3035; border-radius: 24px; min-width: 48px; min-height: 48px; font-size: 20px; }"
  ".site-name { font-size: 12px; }"
  "button.site { background: none; box-shadow: none; padding: 4px 0; }"
  ".dim { color: #a9abb1; }"
  ".url { color: #a8c7fa; font-size: 13px; }"
  ".sub { color: #a9abb1; font-size: 13px; }"
  ".title-big { font-size: 20px; }"
  ".row-dot { background: #2e3035; border-radius: 20px; min-width: 40px; min-height: 40px; font-size: 16px; }"
  "list.neo-list, list.neo-list row { background: none; }"
  "list.neo-list row { min-height: 60px; padding: 0 8px 0 16px; }"
  "list.neo-list row:active { background: alpha(#e3e3e6, 0.08); }"
  ".switcher { background: #1b1c20; }"
  "button.fab { background: #a8c7fa; border-radius: 16px; min-width: 56px; min-height: 48px; padding: 0; }"
  ".segment { background: #2a2c31; border-radius: 24px; padding: 4px; }"
  ".segment button { border-radius: 20px; min-width: 56px; min-height: 40px; background: none; box-shadow: none;"
  " padding: 0; }"
  ".segment button.on { background: #3b3d42; }"
  ".tab-search { background: #2a2c31; border-radius: 26px; min-height: 52px; padding: 0 0 0 16px;"
  " margin: 0 14px 8px 14px; }"
  ".tab-search entry { font-size: 17px; }"
  ".tab-card { background: #2b2d31; border-radius: 22px; padding: 0 6px 6px 6px; }"
  ".tab-card.sel { background: #a8c7fa; color: #062e6f; }"
  ".tab-thumb { background: #ffffff; border-radius: 16px; }"
  ".tab-card.incognito .tab-thumb { background: #3c3f45; }"
  ".tab-thumb label { color: #5f6368; font-size: 13px; }"
  ".favletter { background: #2e3035; border-radius: 6px; min-width: 22px; min-height: 22px; font-size: 12px;"
  " font-weight: bold; }"
  ".tab-card.sel .favletter { background: #062e6f; color: #a8c7fa; }"
  ".tab-title { font-size: 15px; font-weight: 500; }"
  "flowbox.tabs flowboxchild { padding: 6px; background: none; }"
  ".turn-up { -gtk-icon-transform: rotate(90deg); }"
  ".turn-down { -gtk-icon-transform: rotate(-90deg); }";

/* ---- types ---- */

typedef struct {
  guint          id;
  char          *url;
  char          *title;
  gboolean       incognito;
  gboolean       desktop;
  gboolean       ntp;      /* the new-tab page shows over this tab (a new tab, or Home was pressed) */
  WebKitWebView *view;     /* made once the tab is shown: restored tabs cost nothing until then */
  GdkTexture    *thumb;
} Tab;

typedef struct {
  GtkWidget *scrim;
  GtkWidget *holder;
} Sheet;

typedef struct {
  WebKitDownload *d;
  char           *name;
  char           *path;
  gboolean        done;
  gboolean        failed;
  GtkWidget      *row;     /* weak: the downloads page's row while it shows */
} Download;

typedef struct Browser Browser;
typedef void (*RowFn) (Browser *b, const char *arg);

struct Browser {
  GtkApplication *app;
  NbStore        *store;
  GPtrArray      *tabs;              /* Tab */
  Tab            *current;
  guint           next_tab_id;
  GPtrArray      *downloads;         /* Download, newest first */
  GHashTable     *dl_seen;           /* WebKitDownload */
  char           *data_dir, *cache_dir, *downloads_dir;

  WebKitNetworkSession         *session;
  WebKitNetworkSession         *incog_session;
  WebKitUserContentManager     *ucm;
  WebKitUserContentFilterStore *filters;
  WebKitUserContentFilter      *filter;

  GtkWidget *win, *toasts, *overlay, *pages;
  /* browse */
  GtkWidget *bar_stack, *toolbar, *addr_icon, *addr_label, *count_label, *back_probe;
  GtkWidget *find_entry, *find_label, *progress, *view_stack, *ntp;
  guint      find_count, find_index;
  /* address entry */
  GtkWidget *omni_entry, *omni_clear, *suggest_list;
  gboolean   omni_fresh;             /* until the first keystroke the list is the recent history */
  gboolean   omni_setting;
  /* tab switcher */
  GtkWidget *seg_normal, *seg_normal_count, *seg_incog, *tab_filter, *tab_grid, *close_all_label, *switcher_pop;
  gboolean   switcher_incognito;
  /* history, bookmarks, downloads */
  GtkWidget *list_title, *list_delete, *list, *list_scroll;
  const char *list_kind;
  guint      list_gen, list_idle, list_i;
  Sheet      menu, link_sheet;
  guint      session_id;
};

static void select_tab (Browser *b, Tab *t);
static Tab *new_tab (Browser *b, const char *url, gboolean incognito, gboolean foreground, WebKitWebView *related);
static void close_tab (Browser *b, Tab *t);
static void show_current (Browser *b);
static void sync_ui (Browser *b);
static void sync_progress (Browser *b);
static void sync_back (Browser *b);
static void save_session (Browser *b);
static void open_omnibox (Browser *b, const char *text);
static void fill_switcher (Browser *b);
static void toast (Browser *b, const char *text);
static void track_download (Browser *b, WebKitDownload *d);
static void ask_clear_data (Browser *b);
static void sheet_close (Sheet *s);
static void show_page (Browser *b, const char *name);

/* ---- little widgets ---- */

static GIcon *
file_icon (const char *name)
{
  g_autofree char *path = g_strdup_printf ("%s/%s.svg", NB_ICONDIR, name);
  g_autoptr (GFile) file = g_file_new_for_path (path);
  return g_file_icon_new (file);
}

static GtkWidget *
icon (const char *name, int size)
{
  g_autoptr (GIcon) gicon = file_icon (name);
  GtkWidget *image = gtk_image_new_from_gicon (gicon);
  gtk_image_set_pixel_size (GTK_IMAGE (image), size);
  return image;
}

static void
set_icon (GtkWidget *image, const char *name)
{
  g_autoptr (GIcon) gicon = file_icon (name);
  gtk_image_set_from_gicon (GTK_IMAGE (image), gicon);
}

static GtkWidget *
label (const char *text, const char *cls, PangoEllipsizeMode ellipsize)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  if (cls)
    gtk_widget_add_css_class (l, cls);
  if (ellipsize != PANGO_ELLIPSIZE_NONE)
    gtk_label_set_ellipsize (GTK_LABEL (l), ellipsize);
  return l;
}

static void
set_back_name (GtkWidget *button)
{
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Back", -1);
}

static GtkWidget *
icon_button (const char *name, GCallback cb, gpointer data)
{
  GtkWidget *button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (button), icon (name, 24));
  gtk_widget_add_css_class (button, "neo-icon");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  if (cb)
    g_signal_connect_swapped (button, "clicked", cb, data);
  return button;
}

static GtkWidget *
hbox (int spacing)
{
  return gtk_box_new (GTK_ORIENTATION_HORIZONTAL, spacing);
}

static GtkWidget *
vbox (int spacing)
{
  return gtk_box_new (GTK_ORIENTATION_VERTICAL, spacing);
}

/* a list's scroller: libspongy's stretch and fling when it is installed, a scrolled window otherwise */
static GtkWidget *
scroller_new (GtkWidget *child)
{
#ifdef HAVE_SPONGY
  GtkWidget *s = spongy_scroll_view_new ();
  if (child)
    spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (s), child);
#else
  GtkWidget *s = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  if (child)
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s), child);
#endif
  gtk_widget_set_vexpand (s, TRUE);
  return s;
}

static void
scroller_set_child (GtkWidget *s, GtkWidget *child)
{
#ifdef HAVE_SPONGY
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (s), child);
#else
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s), child);
#endif
}

static void
scroller_to_top (GtkWidget *s)
{
#ifdef HAVE_SPONGY
  spongy_scroll_view_scroll_to (SPONGY_SCROLL_VIEW (s), 0);
#else
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (s)), 0);
#endif
}

static Tab *
tab_by_id (Browser *b, guint id)
{
  for (guint i = 0; i < b->tabs->len; i++)
    if (((Tab *) b->tabs->pdata[i])->id == id)
      return b->tabs->pdata[i];
  return NULL;
}

static const char *
tab_uri (Tab *t)
{
  const char *u = t->view ? webkit_web_view_get_uri (t->view) : NULL;
  return u && *u ? u : t->url ? t->url : "";
}

static const char *
tab_name (Tab *t)
{
  const char *n = t->view ? webkit_web_view_get_title (t->view) : NULL;
  return n && *n ? n : t->title ? t->title : "";
}

static guint
count_tabs (Browser *b, gboolean incognito)
{
  guint n = 0;
  for (guint i = 0; i < b->tabs->len; i++)
    n += ((Tab *) b->tabs->pdata[i])->incognito == incognito;
  return n;
}

static void
tab_free (Tab *t)
{
  g_free (t->url);
  g_free (t->title);
  g_clear_object (&t->thumb);
  g_free (t);
}

/* ---- engine: a persistent session, an incognito one made when needed, the shared page styles and filters ---- */

static void
on_download_started (WebKitNetworkSession *s, WebKitDownload *d, gpointer data)
{
  track_download (data, d);
}

static WebKitNetworkSession *
incognito_session (Browser *b)
{
  if (!b->incog_session) {
    b->incog_session = webkit_network_session_new_ephemeral ();
    g_signal_connect (b->incog_session, "download-started", G_CALLBACK (on_download_started), b);
  }
  return b->incog_session;
}

static void
apply_filter (Browser *b, WebKitUserContentFilter *f)
{
  if (b->filter) {
    webkit_user_content_manager_remove_filter (b->ucm, b->filter);
    webkit_user_content_filter_unref (b->filter);
  }
  b->filter = f;
  if (f)
    webkit_user_content_manager_add_filter (b->ucm, f);
}

static void fetch_filter (Browser *b);

typedef struct {
  Browser *b;
  gboolean fetched;
} Compile;

static void
on_filter_saved (GObject *src, GAsyncResult *res, gpointer data)
{
  Compile *c = data;
  Browser *b = c->b;
  g_autoptr (GError) error = NULL;
  WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish (WEBKIT_USER_CONTENT_FILTER_STORE (src),
                                                                             res, &error);
  if (!f) {
    g_message ("neo-browser: ad filter not compiled: %s", error->message);
  } else {
    /* the bundled copy: fetch again soon */
    nb_store_set_filter_time (b->store, c->fetched ? g_get_real_time () / 1e6 : 1);
    if (nb_store_adblock (b->store))
      apply_filter (b, f);
    else
      webkit_user_content_filter_unref (f);
    if (!c->fetched)
      fetch_filter (b);
  }
  g_free (c);
}

static void
compile_filter (Browser *b, GBytes *bytes, gboolean fetched)
{
  Compile *c = g_new0 (Compile, 1);
  c->b = b;
  c->fetched = fetched;
  webkit_user_content_filter_store_save (b->filters, FILTER_ID, bytes, NULL, on_filter_saved, c);
}

static void
on_filter_fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  Browser *b = data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (!bytes)
    g_message ("neo-browser: ad filter not fetched: %s", error->message);
  else if (!msg || soup_message_get_status (msg) != 200 || g_bytes_get_size (bytes) < 10000)
    g_message ("neo-browser: ad filter not fetched: %zu bytes", g_bytes_get_size (bytes));
  else
    compile_filter (b, bytes, TRUE);
  g_object_unref (src);
}

static void
fetch_filter (Browser *b)
{
  SoupSession *session = soup_session_new_with_options ("timeout", 60, NULL);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", FILTER_URL);
  soup_session_send_and_read_async (session, msg, G_PRIORITY_LOW, NULL, on_filter_fetched, b);
}

/* the compiled filter if there is one (and fresh), else compile the copy beside the app or fetch the list */
static void
on_filter_loaded (GObject *src, GAsyncResult *res, gpointer data)
{
  Browser *b = data;
  WebKitUserContentFilter *f = webkit_user_content_filter_store_load_finish (WEBKIT_USER_CONTENT_FILTER_STORE (src),
                                                                             res, NULL);
  apply_filter (b, f);
  double stamp = nb_store_filter_time (b->store);
  g_autofree char *bundled = g_build_filename (NB_DATADIR, "easylist.json", NULL);
  if (!b->filter && !stamp && g_file_test (bundled, G_FILE_TEST_EXISTS)) {
    g_autoptr (GMappedFile) mapped = g_mapped_file_new (bundled, FALSE, NULL);
    if (mapped) {
      g_autoptr (GBytes) bytes = g_mapped_file_get_bytes (mapped);
      compile_filter (b, bytes, FALSE);
      return;
    }
  }
  if (g_get_real_time () / 1e6 - stamp > FILTER_MAX_AGE)
    fetch_filter (b);
}

static void
load_filter (Browser *b)
{
  webkit_user_content_filter_store_load (b->filters, FILTER_ID, NULL, on_filter_loaded, b);
}

static void
set_adblock (Browser *b, gboolean on)
{
  nb_store_set_adblock (b->store, on);
  if (on)
    load_filter (b);
  else
    apply_filter (b, NULL);
  if (b->current && b->current->view)
    webkit_web_view_reload (b->current->view);
}

static void
setup_engine (Browser *b)
{
  /* a heavy page grows its web process without end: past 768 MB WebKit kills and reloads it */
  WebKitMemoryPressureSettings *m = webkit_memory_pressure_settings_new ();
  webkit_memory_pressure_settings_set_memory_limit (m, MEMORY_LIMIT_MB);
  webkit_network_session_set_memory_pressure_settings (m);
  webkit_memory_pressure_settings_free (m);

  g_autofree char *web = g_build_filename (b->data_dir, "web", NULL);
  g_autofree char *cookies = g_build_filename (b->data_dir, "cookies.sqlite", NULL);
  g_mkdir_with_parents (web, 0700);
  b->session = webkit_network_session_new (web, b->cache_dir);
  webkit_cookie_manager_set_persistent_storage (webkit_network_session_get_cookie_manager (b->session), cookies,
                                                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_website_data_manager_set_favicons_enabled (webkit_network_session_get_website_data_manager (b->session),
                                                    TRUE);
  g_signal_connect (b->session, "download-started", G_CALLBACK (on_download_started), b);

  b->ucm = webkit_user_content_manager_new ();
  WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new (SCROLLBAR_CSS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                             WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
  webkit_user_content_manager_add_style_sheet (b->ucm, sheet);
  webkit_user_style_sheet_unref (sheet);
  g_autofree char *filters = g_build_filename (b->data_dir, "filters", NULL);
  b->filters = webkit_user_content_filter_store_new (filters);
  if (nb_store_adblock (b->store))
    load_filter (b);
}

/* ---- a tab's view ---- */

static gboolean
is_current (Browser *b, Tab *t)
{
  return t == b->current;
}

static Tab *
tab_of_view (Browser *b, WebKitWebView *v)
{
  for (guint i = 0; i < b->tabs->len; i++)
    if (((Tab *) b->tabs->pdata[i])->view == v)
      return b->tabs->pdata[i];
  return NULL;
}

static void
on_title (WebKitWebView *v, GParamSpec *pspec, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  if (!t)
    return;
  if (!t->incognito && !webkit_web_view_is_loading (v))
    nb_store_retitle (b->store, webkit_web_view_get_uri (v), webkit_web_view_get_title (v));
  if (is_current (b, t))
    sync_ui (b);
}

static void
on_uri (WebKitWebView *v, GParamSpec *pspec, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  if (t && is_current (b, t))
    sync_ui (b);
  save_session (b);
}

static void
on_progress (WebKitWebView *v, GParamSpec *pspec, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  if (t && is_current (b, t)) {
    sync_progress (b);
    sync_back (b);
  }
}

static void
on_load_changed (WebKitWebView *v, WebKitLoadEvent ev, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  if (!t)
    return;
  if (ev == WEBKIT_LOAD_FINISHED && !t->incognito)
    nb_store_visit (b->store, webkit_web_view_get_uri (v), webkit_web_view_get_title (v));
  if (ev == WEBKIT_LOAD_STARTED && t->ntp && is_current (b, t) && webkit_web_view_get_uri (v)) {
    t->ntp = FALSE;
    show_current (b);
  }
  if (is_current (b, t))
    sync_back (b);
}

static GtkWidget *
on_create (WebKitWebView *v, WebKitNavigationAction *action, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  Tab *n = new_tab (b, NULL, t && t->incognito, TRUE, v);
  return GTK_WIDGET (n->view);
}

static void
on_close_view (WebKitWebView *v, Browser *b)
{
  Tab *t = tab_of_view (b, v);
  if (t)
    close_tab (b, t);
}

static void
on_uri_launched (GObject *src, GAsyncResult *res, gpointer data)
{
  if (!gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (src), res, NULL))
    toast (data, "No app opens this link");
}

static gboolean
on_decide (WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, Browser *b)
{
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION (d);
    if (!webkit_response_policy_decision_is_mime_type_supported (r) &&
        webkit_response_policy_decision_is_main_frame_main_resource (r)) {
      webkit_policy_decision_download (d);
      return TRUE;
    }
    return FALSE;
  }
  /* tel:, mailto:, geo: … go to the app for them (Calls, the mail client) */
  WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION (d);
  WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action (nav);
  WebKitURIRequest *req = a ? webkit_navigation_action_get_request (a) : NULL;
  const char *uri = req ? webkit_uri_request_get_uri (req) : NULL;
  if (!uri || !*uri)
    return FALSE;
  const char *scheme = g_uri_peek_scheme (uri);   /* lower case, interned */
  static const char *const ours[] = {"http", "https", "about", "data", "blob", "file", "javascript", NULL};
  if (!scheme || g_strv_contains (ours, scheme))
    return FALSE;
  webkit_policy_decision_ignore (d);
  g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
  gtk_uri_launcher_launch (launcher, GTK_WINDOW (b->win), NULL, on_uri_launched, b);
  return TRUE;
}

static void context_sheet (Browser *b, Tab *t, WebKitHitTestResult *hit);

static gboolean
on_context_menu (WebKitWebView *v, WebKitContextMenu *menu, WebKitHitTestResult *hit, Browser *b)
{
  /* a text field keeps WebKit's own menu (paste); links and images get the sheet; elsewhere nothing */
  if (webkit_hit_test_result_context_is_editable (hit) || webkit_hit_test_result_context_is_selection (hit))
    return FALSE;
  Tab *t = tab_of_view (b, v);
  if (t)
    context_sheet (b, t, hit);
  return TRUE;
}

typedef struct {
  Browser                 *b;
  WebKitPermissionRequest *req;
} Ask;

static void
on_permission_response (AdwAlertDialog *d, const char *response, gpointer data)
{
  Ask *ask = data;
  if (g_str_equal (response, "allow"))
    webkit_permission_request_allow (ask->req);
  else
    webkit_permission_request_deny (ask->req);
  g_object_unref (ask->req);
  g_free (ask);
}

static gboolean
on_permission (WebKitWebView *v, WebKitPermissionRequest *req, Browser *b)
{
  const char *what = "use a device feature";

  if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST (req))
    what = "know your location";
  else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST (req))
    what = "show notifications";
  else if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST (req))
    what = "see text and images copied to the clipboard";
  else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req)) {
    WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST (req);
    gboolean audio = webkit_user_media_permission_is_for_audio_device (m);
    gboolean video = webkit_user_media_permission_is_for_video_device (m);
    what = audio && video ? "use your camera and microphone" : video ? "use your camera" : "use your microphone";
  } else if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST (req))
    what = "play protected content";

  g_autofree char *host = nb_host (webkit_web_view_get_uri (v));
  g_autofree char *heading = g_strdup_printf ("%s wants to %s", host, what);
  AdwDialog *d = adw_alert_dialog_new (heading, NULL);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "block", "Block");
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "allow", "Allow");
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "allow", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "block");
  Ask *ask = g_new0 (Ask, 1);
  ask->b = b;
  ask->req = g_object_ref (req);
  g_signal_connect (d, "response", G_CALLBACK (on_permission_response), ask);
  adw_dialog_present (d, b->win);
  return TRUE;
}

static void
set_fullscreen (Browser *b, gboolean on)
{
  gtk_widget_set_visible (b->bar_stack, !on);
  gtk_widget_set_visible (b->progress, !on);
  if (on)
    gtk_window_fullscreen (GTK_WINDOW (b->win));
  else
    gtk_window_unfullscreen (GTK_WINDOW (b->win));
}

static gboolean
on_enter_fullscreen (WebKitWebView *v, Browser *b)
{
  set_fullscreen (b, TRUE);
  return FALSE;
}

static gboolean
on_leave_fullscreen (WebKitWebView *v, Browser *b)
{
  set_fullscreen (b, FALSE);
  return FALSE;
}

static void sync_find (Browser *b);

static void
on_counted (WebKitFindController *fc, guint n, Browser *b)
{
  Tab *t = tab_of_view (b, webkit_find_controller_get_web_view (fc));
  if (t && is_current (b, t)) {
    b->find_count = n;
    sync_find (b);
  }
}

static void
on_not_found (WebKitFindController *fc, Browser *b)
{
  Tab *t = tab_of_view (b, webkit_find_controller_get_web_view (fc));
  if (t && is_current (b, t)) {
    b->find_count = 0;
    sync_find (b);
  }
}

static void
make_view (Browser *b, Tab *t, WebKitWebView *related)
{
  WebKitSettings *settings = webkit_settings_new ();
  webkit_settings_set_user_agent (settings, t->desktop ? DESKTOP_UA : MOBILE_UA);
  webkit_settings_set_enable_smooth_scrolling (settings, TRUE);
  webkit_settings_set_enable_developer_extras (settings, FALSE);
  webkit_settings_set_javascript_can_open_windows_automatically (settings, FALSE);
  webkit_settings_set_media_playback_requires_user_gesture (settings, TRUE);
  /* WebKit holds a page's rendering near 60 fps (Safari's choice for 120 Hz screens): on a 120 Hz panel a scroll
   * then ran at 40-60 fps. Without it, 117 fps measured. */
  WebKitFeatureList *features = webkit_settings_get_all_features ();
  for (gsize i = 0; i < webkit_feature_list_get_length (features); i++) {
    WebKitFeature *f = webkit_feature_list_get (features, i);
    if (g_strcmp0 (webkit_feature_get_identifier (f), "PreferPageRenderingUpdatesNear60FPS") == 0)
      webkit_settings_set_feature_enabled (settings, f, FALSE);
  }
  webkit_feature_list_unref (features);

  WebKitWebView *v;
  if (related)
    v = g_object_new (WEBKIT_TYPE_WEB_VIEW, "settings", settings, "user-content-manager", b->ucm,
                      "related-view", related, "hexpand", TRUE, "vexpand", TRUE, NULL);
  else
    v = g_object_new (WEBKIT_TYPE_WEB_VIEW, "settings", settings, "user-content-manager", b->ucm,
                      "network-session", t->incognito ? incognito_session (b) : b->session,
                      "hexpand", TRUE, "vexpand", TRUE, NULL);
  g_object_unref (settings);
  t->view = v;
  if (t->desktop)
    webkit_web_view_set_zoom_level (v, 0.5);
  g_signal_connect (v, "notify::title", G_CALLBACK (on_title), b);
  g_signal_connect (v, "notify::uri", G_CALLBACK (on_uri), b);
  g_signal_connect (v, "notify::estimated-load-progress", G_CALLBACK (on_progress), b);
  g_signal_connect (v, "notify::is-loading", G_CALLBACK (on_progress), b);
  g_signal_connect (v, "load-changed", G_CALLBACK (on_load_changed), b);
  g_signal_connect (v, "create", G_CALLBACK (on_create), b);
  g_signal_connect (v, "close", G_CALLBACK (on_close_view), b);
  g_signal_connect (v, "decide-policy", G_CALLBACK (on_decide), b);
  g_signal_connect (v, "context-menu", G_CALLBACK (on_context_menu), b);
  g_signal_connect (v, "permission-request", G_CALLBACK (on_permission), b);
  g_signal_connect (v, "enter-fullscreen", G_CALLBACK (on_enter_fullscreen), b);
  g_signal_connect (v, "leave-fullscreen", G_CALLBACK (on_leave_fullscreen), b);
  WebKitFindController *fc = webkit_web_view_get_find_controller (v);
  g_signal_connect (fc, "counted-matches", G_CALLBACK (on_counted), b);
  g_signal_connect (fc, "failed-to-find-text", G_CALLBACK (on_not_found), b);
  gtk_stack_add_child (GTK_STACK (b->view_stack), GTK_WIDGET (v));
}

/* ---- tabs ---- */

static Tab *
new_tab (Browser *b, const char *url, gboolean incognito, gboolean foreground, WebKitWebView *related)
{
  Tab *t = g_new0 (Tab, 1);

  t->id = ++b->next_tab_id;
  t->url = g_strdup (url ? url : "");
  t->incognito = incognito;
  t->ntp = !url || !*url;
  g_ptr_array_add (b->tabs, t);
  if (related) {
    t->ntp = FALSE;
    make_view (b, t, related);
  } else if (url && *url) {
    make_view (b, t, NULL);
    webkit_web_view_load_uri (t->view, url);
  }
  if (foreground)
    select_tab (b, t);
  else
    sync_ui (b);
  save_session (b);
  return t;
}

static void
select_tab (Browser *b, Tab *t)
{
  b->current = t;
  if (!t->ntp && !t->view && *t->url) {
    make_view (b, t, NULL);
    webkit_web_view_load_uri (t->view, t->url);
  }
  show_current (b);
  show_page (b, "browse");
  save_session (b);
}

static void fill_ntp (Browser *b);

static void
show_current (Browser *b)
{
  Tab *t = b->current;
  if (!t)
    return;
  if (t->ntp || !t->view) {
    fill_ntp (b);
    gtk_stack_set_visible_child (GTK_STACK (b->view_stack), b->ntp);
  } else {
    gtk_stack_set_visible_child (GTK_STACK (b->view_stack), GTK_WIDGET (t->view));
  }
  sync_ui (b);
}

static void
close_tab (Browser *b, Tab *t)
{
  guint i;
  if (!g_ptr_array_find (b->tabs, t, &i))
    return;
  g_ptr_array_remove_index (b->tabs, i);
  if (t->view) {
    gtk_stack_remove (GTK_STACK (b->view_stack), GTK_WIDGET (t->view));
    t->view = NULL;
  }
  if (!count_tabs (b, TRUE))
    g_clear_object (&b->incog_session);   /* nothing of the incognito tabs is kept */
  if (b->current == t) {
    g_autoptr (GPtrArray) same = g_ptr_array_new ();
    for (guint k = 0; k < b->tabs->len; k++)
      if (((Tab *) b->tabs->pdata[k])->incognito == t->incognito)
        g_ptr_array_add (same, b->tabs->pdata[k]);
    Tab *next = same->len ? same->pdata[MIN (i, same->len - 1)]
              : b->tabs->len ? b->tabs->pdata[b->tabs->len - 1] : NULL;
    b->current = NULL;
    if (next)
      select_tab (b, next);
  }
  tab_free (t);
  if (!b->tabs->len)
    new_tab (b, NULL, FALSE, TRUE, NULL);
  save_session (b);
  if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (b->pages)), "tabs"))
    fill_switcher (b);
}

static void
close_all (Browser *b, gboolean incognito)
{
  g_autoptr (GPtrArray) doomed = g_ptr_array_new ();
  for (guint i = 0; i < b->tabs->len; i++)
    if (((Tab *) b->tabs->pdata[i])->incognito == incognito)
      g_ptr_array_add (doomed, b->tabs->pdata[i]);
  for (guint i = 0; i < doomed->len; i++)
    close_tab (b, doomed->pdata[i]);
}

static void
write_session (Browser *b)
{
  JsonArray *tabs = json_array_new ();
  int current = 0, n = 0;

  for (guint i = 0; i < b->tabs->len; i++) {
    Tab *t = b->tabs->pdata[i];
    if (t->incognito)
      continue;
    /* a tab showing the new-tab page over a site (Home) keeps the site: the GJS browser lost the tab */
    const char *url = tab_uri (t);
    if (!*url)
      continue;
    if (t == b->current)
      current = n;
    JsonObject *o = json_object_new ();
    json_object_set_string_member (o, "url", url);
    json_object_set_string_member (o, "title", tab_name (t));
    json_array_add_object_element (tabs, o);
    n++;
  }
  JsonObject *root = json_object_new ();
  json_object_set_array_member (root, "tabs", tabs);
  json_object_set_int_member (root, "current", current);
  JsonNode *node = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (node, root);
  nb_store_write_node (b->store, "session.json", node);
  json_node_unref (node);
}

static gboolean
on_session_timeout (gpointer data)
{
  Browser *b = data;
  b->session_id = 0;
  write_session (b);
  return G_SOURCE_REMOVE;
}

static void
save_session (Browser *b)
{
  if (!b->session_id)
    b->session_id = g_timeout_add_full (G_PRIORITY_LOW, 500, on_session_timeout, b, NULL);
}

static void
browse (Browser *b, const char *text)
{
  g_autofree char *s = g_strstrip (g_strdup (text ? text : ""));
  g_autofree char *url = NULL;

  if (!*s)
    return;
  if (g_regex_match_simple ("^[a-z]+://", s, G_REGEX_CASELESS, 0) ||
      g_regex_match_simple ("^(about|data|file):", s, G_REGEX_CASELESS, 0))
    url = g_strdup (s);
  else if (!g_regex_match_simple ("\\s", s, 0, 0) &&
           g_regex_match_simple ("^[^\\s]+\\.[a-z]{2,}(:\\d+)?(/.*)?$", s, G_REGEX_CASELESS, 0))
    url = g_strdup_printf ("https://%s", s);
  else {
    g_autofree char *q = g_uri_escape_string (s, NULL, TRUE);
    url = g_strconcat (SEARCH_URL, q, NULL);
  }
  if (!b->current)
    new_tab (b, NULL, FALSE, TRUE, NULL);
  Tab *t = b->current;
  t->ntp = FALSE;
  if (!t->view)
    make_view (b, t, NULL);
  webkit_web_view_load_uri (t->view, url);
  select_tab (b, t);
}

/* ---- back ---- */

static gboolean
sheet_shown (Sheet *s)
{
  return s->holder && gtk_widget_get_visible (s->holder);
}

static const char *
page_name (Browser *b)
{
  return gtk_stack_get_visible_child_name (GTK_STACK (b->pages));
}

static gboolean
finding (Browser *b)
{
  return g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (b->bar_stack)), "find");
}

/* whether back has somewhere to go inside the browser (the probe button is on exactly then) */
static gboolean
can_step_back (Browser *b)
{
  Tab *t = b->current;
  if (sheet_shown (&b->menu) || sheet_shown (&b->link_sheet) || !g_str_equal (page_name (b), "browse") || finding (b))
    return TRUE;
  if (!t)
    return FALSE;
  if (t->ntp)
    return t->view && webkit_web_view_get_uri (t->view);
  if (t->view && webkit_web_view_can_go_back (t->view))
    return TRUE;
  return b->tabs->len > 1;      /* back closes this tab, as Chrome does for a tab with no history */
}

static void close_find (Browser *b);

static void
step_back (Browser *b)
{
  if (sheet_shown (&b->menu)) {
    sheet_close (&b->menu);
    return;
  }
  if (sheet_shown (&b->link_sheet)) {
    sheet_close (&b->link_sheet);
    return;
  }
  if (!g_str_equal (page_name (b), "browse")) {
    show_page (b, "browse");
    return;
  }
  if (finding (b)) {
    close_find (b);
    return;
  }
  Tab *t = b->current;
  if (!t)
    return;
  if (t->ntp && t->view && webkit_web_view_get_uri (t->view)) {
    t->ntp = FALSE;
    show_current (b);
    return;
  }
  if (t->view && webkit_web_view_can_go_back (t->view)) {
    webkit_web_view_go_back (t->view);
    return;
  }
  if (!t->ntp)
    close_tab (b, t);
}

static void
go_back (Browser *b)
{
  step_back (b);
  sync_back (b);
}

static void
sync_back (Browser *b)
{
  if (b->back_probe)
    gtk_widget_set_sensitive (b->back_probe, can_step_back (b));
}

static void
show_page (Browser *b, const char *name)
{
  gtk_stack_set_visible_child_name (GTK_STACK (b->pages), name);
  sync_back (b);
}

/* ---- the browse page: toolbar / find bar, progress, the pages ---- */

static void
on_home (Browser *b)
{
  if (!b->current)
    return;
  b->current->ntp = TRUE;
  show_current (b);
  save_session (b);
}

static void
on_pill (Browser *b)
{
  open_omnibox (b, b->current && !b->current->ntp ? tab_uri (b->current) : "");
}

static void open_switcher (Browser *b);
static void open_menu (Browser *b);
static void find_changed (Browser *b);
static void find_step (Browser *b, int dir);

static void
on_find_up (Browser *b)
{
  find_step (b, -1);
}

static void
on_find_down (Browser *b)
{
  find_step (b, 1);
}

static GtkWidget *
build_browse (Browser *b)
{
  GtkWidget *box = vbox (0);

  b->bar_stack = gtk_stack_new ();
  /* toolbar */
  b->toolbar = hbox (0);
  gtk_widget_add_css_class (b->toolbar, "neo-bar");
  gtk_box_append (GTK_BOX (b->toolbar), icon_button ("home", G_CALLBACK (on_home), b));
  b->addr_icon = icon ("globe", 24);
  gtk_widget_set_opacity (b->addr_icon, 0.85);
  b->addr_label = label ("", NULL, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (b->addr_label, TRUE);
  GtkWidget *pill_box = hbox (8);
  gtk_box_append (GTK_BOX (pill_box), b->addr_icon);
  gtk_box_append (GTK_BOX (pill_box), b->addr_label);
  GtkWidget *pill = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (pill), pill_box);
  gtk_widget_add_css_class (pill, "pill");
  gtk_widget_set_hexpand (pill, TRUE);
  gtk_widget_set_valign (pill, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (pill, "clicked", G_CALLBACK (on_pill), b);
  gtk_box_append (GTK_BOX (b->toolbar), pill);
  b->count_label = gtk_label_new ("");
  gtk_widget_add_css_class (b->count_label, "tabcount");
  GtkWidget *count = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (count), b->count_label);
  gtk_widget_add_css_class (count, "neo-icon");
  gtk_widget_set_valign (count, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (count, "clicked", G_CALLBACK (open_switcher), b);
  gtk_box_append (GTK_BOX (b->toolbar), count);
  gtk_box_append (GTK_BOX (b->toolbar), icon_button ("more", G_CALLBACK (open_menu), b));
  gtk_stack_add_named (GTK_STACK (b->bar_stack), b->toolbar, "bar");

  /* find in page */
  GtkWidget *find = hbox (0);
  gtk_widget_add_css_class (find, "neo-bar");
  GtkWidget *find_back = icon_button ("back", G_CALLBACK (close_find), b);
  set_back_name (find_back);
  gtk_box_append (GTK_BOX (find), find_back);
  b->find_entry = gtk_entry_new ();
  gtk_widget_add_css_class (b->find_entry, "neo-entry");
  gtk_widget_set_hexpand (b->find_entry, TRUE);
  gtk_widget_set_valign (b->find_entry, GTK_ALIGN_CENTER);
  gtk_entry_set_placeholder_text (GTK_ENTRY (b->find_entry), "Find in page");
  g_signal_connect_swapped (b->find_entry, "changed", G_CALLBACK (find_changed), b);
  g_signal_connect_swapped (b->find_entry, "activate", G_CALLBACK (on_find_down), b);
  gtk_box_append (GTK_BOX (find), b->find_entry);
  b->find_label = label ("", "dim", PANGO_ELLIPSIZE_NONE);
  gtk_box_append (GTK_BOX (find), b->find_label);
  /* up and down are the back arrow turned (-gtk-icon-transform) */
  GtkWidget *up = icon_button ("back", G_CALLBACK (on_find_up), b);
  gtk_widget_add_css_class (gtk_button_get_child (GTK_BUTTON (up)), "turn-up");
  GtkWidget *down = icon_button ("back", G_CALLBACK (on_find_down), b);
  gtk_widget_add_css_class (gtk_button_get_child (GTK_BUTTON (down)), "turn-down");
  gtk_box_append (GTK_BOX (find), up);
  gtk_box_append (GTK_BOX (find), down);
  gtk_stack_add_named (GTK_STACK (b->bar_stack), find, "find");
  gtk_box_append (GTK_BOX (box), b->bar_stack);

  b->progress = gtk_progress_bar_new ();
  gtk_widget_add_css_class (b->progress, "neo-progress");
  gtk_box_append (GTK_BOX (box), b->progress);

  /* the pages, and the new-tab page */
  b->view_stack = gtk_stack_new ();
  gtk_widget_set_vexpand (b->view_stack, TRUE);
  gtk_stack_set_hhomogeneous (GTK_STACK (b->view_stack), FALSE);
  gtk_stack_set_vhomogeneous (GTK_STACK (b->view_stack), FALSE);
  b->ntp = scroller_new (NULL);
  gtk_widget_add_css_class (b->ntp, "ntp");
  gtk_stack_add_child (GTK_STACK (b->view_stack), b->ntp);
  gtk_box_append (GTK_BOX (box), b->view_stack);
  return box;
}

static void
on_url_clicked (GtkButton *button, Browser *b)
{
  browse (b, g_object_get_data (G_OBJECT (button), "url"));
}

static void
on_search_pill (Browser *b)
{
  open_omnibox (b, "");
}

static void
fill_ntp (Browser *b)
{
  Tab *t = b->current;
  gboolean incog = t && t->incognito;

  if (incog)
    gtk_widget_add_css_class (b->ntp, "incognito");
  else
    gtk_widget_remove_css_class (b->ntp, "incognito");
  GtkWidget *col = vbox (28);
  gtk_widget_set_margin_top (col, 56);
  gtk_widget_set_margin_start (col, 20);
  gtk_widget_set_margin_end (col, 20);
  gtk_widget_set_margin_bottom (col, 20);
  GtkWidget *title = gtk_label_new (incog ? "Incognito" : "Neo");
  gtk_widget_add_css_class (title, "ntp-title");
  gtk_box_append (GTK_BOX (col), title);
  GtkWidget *sb = hbox (12);
  GtkWidget *si = icon ("search", 24);
  gtk_widget_set_opacity (si, 0.8);
  gtk_box_append (GTK_BOX (sb), si);
  gtk_box_append (GTK_BOX (sb), label ("Search or type URL", NULL, PANGO_ELLIPSIZE_NONE));
  GtkWidget *search = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (search), sb);
  gtk_widget_add_css_class (search, "search-pill");
  g_signal_connect_swapped (search, "clicked", G_CALLBACK (on_search_pill), b);
  gtk_box_append (GTK_BOX (col), search);
  if (incog) {
    GtkWidget *note = label ("Pages you view in Incognito tabs won't stay in history, cookies or the site data store "
                             "once you close them. Downloads and bookmarks are kept.", "dim", PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap (GTK_LABEL (note), TRUE);
    gtk_box_append (GTK_BOX (col), note);
  } else {
    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
    gtk_widget_set_halign (grid, GTK_ALIGN_CENTER);
    g_autoptr (GPtrArray) top = nb_store_top_sites (b->store, 8);
    for (guint i = 0; i < top->len; i++) {
      NbEntry *h = top->pdata[i];
      GtkWidget *cell = vbox (8);
      gtk_widget_set_size_request (cell, 72, -1);
      g_autofree char *letter = nb_letter (h->url);
      GtkWidget *dot = gtk_label_new (letter);
      gtk_widget_add_css_class (dot, "site-dot");
      gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (cell), dot);
      g_autofree char *host = nb_host (h->url);
      GtkWidget *name = gtk_label_new (*h->title ? h->title : host);
      gtk_widget_add_css_class (name, "site-name");
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (name), 9);
      gtk_box_append (GTK_BOX (cell), name);
      GtkWidget *button = gtk_button_new ();
      gtk_button_set_child (GTK_BUTTON (button), cell);
      gtk_widget_add_css_class (button, "site");
      g_object_set_data_full (G_OBJECT (button), "url", g_strdup (h->url), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_url_clicked), b);
      gtk_grid_attach (GTK_GRID (grid), button, i % 4, i / 4, 1, 1);
    }
    gtk_box_append (GTK_BOX (col), grid);
  }
  scroller_set_child (b->ntp, col);
}

static void
sync_ui (Browser *b)
{
  Tab *t = b->current;
  if (!t)
    return;
  if (t->incognito)
    gtk_widget_add_css_class (b->toolbar, "incognito");
  else
    gtk_widget_remove_css_class (b->toolbar, "incognito");
  const char *uri = t->ntp ? "" : tab_uri (t);
  g_autofree char *host = nb_host (uri);
  gtk_label_set_text (GTK_LABEL (b->addr_label), *uri ? host : "Search or type URL");
  gtk_widget_set_opacity (b->addr_label, *uri ? 1 : 0.7);
  set_icon (b->addr_icon, g_str_has_prefix (uri, "https") ? "tune" : "globe");
  guint n = count_tabs (b, t->incognito);
  g_autofree char *count = n > 99 ? g_strdup (":D") : g_strdup_printf ("%u", n);
  gtk_label_set_text (GTK_LABEL (b->count_label), count);
  if (n > 9)
    gtk_widget_add_css_class (b->count_label, "small");
  else
    gtk_widget_remove_css_class (b->count_label, "small");
  const char *name = tab_name (t);
  gtk_window_set_title (GTK_WINDOW (b->win), !t->ntp && *name ? name : "Neo Browser");
  sync_progress (b);
  sync_back (b);
}

static void
sync_progress (Browser *b)
{
  WebKitWebView *v = b->current && !b->current->ntp ? b->current->view : NULL;
  double f = v && webkit_web_view_is_loading (v) ? MAX (0.05, webkit_web_view_get_estimated_load_progress (v)) : 0;
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (b->progress), f);
}

/* ---- find in page ---- */

static void
open_find (Browser *b)
{
  if (!b->current || !b->current->view || b->current->ntp)
    return;
  gtk_editable_set_text (GTK_EDITABLE (b->find_entry), "");
  b->find_count = 0;
  b->find_index = 0;
  sync_find (b);
  gtk_stack_set_visible_child_name (GTK_STACK (b->bar_stack), "find");
  gtk_widget_grab_focus (b->find_entry);
  sync_back (b);
}

static void
close_find (Browser *b)
{
  if (b->current && b->current->view)
    webkit_find_controller_search_finish (webkit_web_view_get_find_controller (b->current->view));
  gtk_stack_set_visible_child_name (GTK_STACK (b->bar_stack), "bar");
  sync_back (b);
}

static void
find_changed (Browser *b)
{
  if (!b->current || !b->current->view)
    return;
  WebKitFindController *fc = webkit_web_view_get_find_controller (b->current->view);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (b->find_entry));
  if (!*text) {
    webkit_find_controller_search_finish (fc);
    b->find_count = 0;
    sync_find (b);
    return;
  }
  guint opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
  b->find_index = 1;
  webkit_find_controller_count_matches (fc, text, opts, 1000);
  webkit_find_controller_search (fc, text, opts, 1000);
}

static void
find_step (Browser *b, int dir)
{
  if (!b->current || !b->current->view || !b->find_count)
    return;
  WebKitFindController *fc = webkit_web_view_get_find_controller (b->current->view);
  if (dir > 0)
    webkit_find_controller_search_next (fc);
  else
    webkit_find_controller_search_previous (fc);
  b->find_index = ((b->find_index - 1 + dir + b->find_count) % b->find_count) + 1;
  sync_find (b);
}

static void
sync_find (Browser *b)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (b->find_entry));
  g_autofree char *s = *text ? g_strdup_printf ("%u/%u", b->find_count ? b->find_index : 0, b->find_count)
                             : g_strdup ("");
  gtk_label_set_text (GTK_LABEL (b->find_label), s);
}

/* ---- address entry with suggestions ---- */

static void fill_suggestions (Browser *b);

static void
on_omni_changed (Browser *b)
{
  if (b->omni_setting)
    return;
  b->omni_fresh = FALSE;
  fill_suggestions (b);
}

static void
on_omni_activate (Browser *b)
{
  browse (b, gtk_editable_get_text (GTK_EDITABLE (b->omni_entry)));
}

static void
on_omni_clear (Browser *b)
{
  gtk_editable_set_text (GTK_EDITABLE (b->omni_entry), "");
  gtk_widget_grab_focus (b->omni_entry);
}

static void
on_suggestion (GtkListBox *list, GtkListBoxRow *row, Browser *b)
{
  browse (b, g_object_get_data (G_OBJECT (row), "target"));
}

static void
on_back_to_browse (Browser *b)
{
  show_page (b, "browse");
}

static GtkWidget *
page_back_button (Browser *b)
{
  GtkWidget *button = icon_button ("back", G_CALLBACK (on_back_to_browse), b);
  set_back_name (button);
  return button;
}

static GtkWidget *
build_omnibox (Browser *b)
{
  GtkWidget *box = vbox (0);
  gtk_widget_add_css_class (box, "neo-bg");
  GtkWidget *bar = hbox (4);
  gtk_widget_add_css_class (bar, "neo-bar");
  gtk_box_append (GTK_BOX (bar), page_back_button (b));
  GtkWidget *pill = hbox (0);
  gtk_widget_add_css_class (pill, "pill-box");
  gtk_widget_set_hexpand (pill, TRUE);
  gtk_widget_set_valign (pill, GTK_ALIGN_CENTER);
  b->omni_entry = gtk_entry_new ();
  gtk_widget_add_css_class (b->omni_entry, "neo-entry");
  gtk_widget_set_hexpand (b->omni_entry, TRUE);
  gtk_entry_set_input_purpose (GTK_ENTRY (b->omni_entry), GTK_INPUT_PURPOSE_URL);
  gtk_entry_set_input_hints (GTK_ENTRY (b->omni_entry),
                             GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_LOWERCASE | GTK_INPUT_HINT_NO_EMOJI);
  g_signal_connect_swapped (b->omni_entry, "changed", G_CALLBACK (on_omni_changed), b);
  g_signal_connect_swapped (b->omni_entry, "activate", G_CALLBACK (on_omni_activate), b);
  gtk_box_append (GTK_BOX (pill), b->omni_entry);
  b->omni_clear = icon_button ("close", G_CALLBACK (on_omni_clear), b);
  gtk_box_append (GTK_BOX (pill), b->omni_clear);
  gtk_box_append (GTK_BOX (bar), pill);
  gtk_box_append (GTK_BOX (box), bar);
  b->suggest_list = gtk_list_box_new ();
  gtk_widget_add_css_class (b->suggest_list, "neo-list");
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (b->suggest_list), GTK_SELECTION_NONE);
  g_signal_connect (b->suggest_list, "row-activated", G_CALLBACK (on_suggestion), b);
  gtk_box_append (GTK_BOX (box), scroller_new (b->suggest_list));
  return box;
}

static void
open_omnibox (Browser *b, const char *text)
{
  show_page (b, "omni");
  b->omni_setting = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (b->omni_entry), text);
  b->omni_setting = FALSE;
  b->omni_fresh = TRUE;
  gtk_widget_grab_focus (b->omni_entry);
  gtk_editable_select_region (GTK_EDITABLE (b->omni_entry), 0, -1);
  fill_suggestions (b);
}

static void
on_fill_in (GtkButton *button, Browser *b)
{
  gtk_editable_set_text (GTK_EDITABLE (b->omni_entry), g_object_get_data (G_OBJECT (button), "fill"));
  gtk_widget_grab_focus (b->omni_entry);
  gtk_editable_set_position (GTK_EDITABLE (b->omni_entry), -1);
}

static void
add_suggestion (Browser *b, const char *kind, const char *title, const char *url)
{
  GtkWidget *row = hbox (20);
  GtkWidget *ic = icon (kind, 24);
  gtk_widget_set_opacity (ic, 0.8);
  gtk_box_append (GTK_BOX (row), ic);
  GtkWidget *col = vbox (0);
  gtk_widget_set_hexpand (col, TRUE);
  gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (col), label (title, NULL, PANGO_ELLIPSIZE_END));
  if (url && *url)
    gtk_box_append (GTK_BOX (col), label (url, "url", PANGO_ELLIPSIZE_END));
  gtk_box_append (GTK_BOX (row), col);
  const char *target = url && *url ? url : title;
  GtkWidget *fill = icon_button ("arrow_up_left", NULL, NULL);
  g_object_set_data_full (G_OBJECT (fill), "fill", g_strdup (target), g_free);
  g_signal_connect (fill, "clicked", G_CALLBACK (on_fill_in), b);
  gtk_box_append (GTK_BOX (row), fill);
  GtkWidget *lr = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (lr), row);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (lr), TRUE);
  g_object_set_data_full (G_OBJECT (lr), "target", g_strdup (target), g_free);
  gtk_list_box_append (GTK_LIST_BOX (b->suggest_list), lr);
}

static void
fill_suggestions (Browser *b)
{
  const char *raw = gtk_editable_get_text (GTK_EDITABLE (b->omni_entry));
  g_autofree char *text = g_strstrip (g_strdup (raw));

  gtk_widget_set_visible (b->omni_clear, *raw != '\0');
  gtk_list_box_remove_all (GTK_LIST_BOX (b->suggest_list));
  if (!*text || b->omni_fresh) {
    /* a tap on the address bar: the pages visited last, as a list under it (the open page not among them) */
    guint n = 0;
    for (guint i = 0; i < b->store->history->len && n < 12; i++) {
      NbEntry *h = b->store->history->pdata[i];
      if (g_str_equal (h->url, raw))
        continue;
      add_suggestion (b, "history", *h->title ? h->title : h->url, h->url);
      n++;
    }
    return;
  }
  add_suggestion (b, "search", text, "");
  g_autoptr (GPtrArray) hits = nb_store_suggest (b->store, text, 6);
  for (guint i = 0; i < hits->len; i++) {
    NbEntry *h = hits->pdata[i];
    add_suggestion (b, "history", *h->title ? h->title : h->url, h->url);
  }
}

/* ---- tab switcher ---- */

static void
on_fab (Browser *b)
{
  new_tab (b, NULL, b->switcher_incognito, TRUE, NULL);
  if (!b->switcher_incognito)
    open_omnibox (b, "");
}

static void
on_seg_normal (Browser *b)
{
  b->switcher_incognito = FALSE;
  fill_switcher (b);
}

static void
on_seg_incog (Browser *b)
{
  b->switcher_incognito = TRUE;
  fill_switcher (b);
}

static void
on_switcher_more (Browser *b)
{
  gtk_label_set_text (GTK_LABEL (b->close_all_label),
                      b->switcher_incognito ? "Close Incognito tabs" : "Close all tabs");
  gtk_popover_popup (GTK_POPOVER (b->switcher_pop));
}

static void
on_close_all (Browser *b)
{
  gtk_popover_popdown (GTK_POPOVER (b->switcher_pop));
  close_all (b, b->switcher_incognito);
  fill_switcher (b);
}

static GtkWidget *
build_switcher (Browser *b)
{
  GtkWidget *box = vbox (0);
  gtk_widget_add_css_class (box, "switcher");
  GtkWidget *head = gtk_center_box_new ();
  gtk_widget_set_margin_start (head, 14);
  gtk_widget_set_margin_end (head, 4);
  gtk_widget_set_size_request (head, -1, 64);
  GtkWidget *fab = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (fab), icon ("add_dark", 24));
  gtk_widget_add_css_class (fab, "fab");
  gtk_widget_set_valign (fab, GTK_ALIGN_CENTER);
  g_signal_connect_swapped (fab, "clicked", G_CALLBACK (on_fab), b);
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (head), fab);
  GtkWidget *seg = hbox (8);
  gtk_widget_add_css_class (seg, "segment");
  gtk_widget_set_valign (seg, GTK_ALIGN_CENTER);
  b->seg_normal_count = gtk_label_new ("");
  gtk_widget_add_css_class (b->seg_normal_count, "tabcount");
  b->seg_normal = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (b->seg_normal), b->seg_normal_count);
  g_signal_connect_swapped (b->seg_normal, "clicked", G_CALLBACK (on_seg_normal), b);
  b->seg_incog = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (b->seg_incog), icon ("incognito", 24));
  g_signal_connect_swapped (b->seg_incog, "clicked", G_CALLBACK (on_seg_incog), b);
  gtk_box_append (GTK_BOX (seg), b->seg_normal);
  gtk_box_append (GTK_BOX (seg), b->seg_incog);
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (head), seg);
  GtkWidget *more = icon_button ("more", G_CALLBACK (on_switcher_more), b);
  b->switcher_pop = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (b->switcher_pop), FALSE);
  GtkWidget *close_box = hbox (20);
  b->close_all_label = label ("Close all tabs", NULL, PANGO_ELLIPSIZE_NONE);
  gtk_box_append (GTK_BOX (close_box), icon ("close", 24));
  gtk_box_append (GTK_BOX (close_box), b->close_all_label);
  GtkWidget *close_button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (close_button), close_box);
  gtk_widget_add_css_class (close_button, "menu-row");
  g_signal_connect_swapped (close_button, "clicked", G_CALLBACK (on_close_all), b);
  gtk_popover_set_child (GTK_POPOVER (b->switcher_pop), close_button);
  gtk_widget_set_parent (b->switcher_pop, more);
  g_signal_connect_swapped (more, "destroy", G_CALLBACK (gtk_widget_unparent), b->switcher_pop);
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (head), more);
  gtk_box_append (GTK_BOX (box), head);

  GtkWidget *search = hbox (12);
  gtk_widget_add_css_class (search, "tab-search");
  GtkWidget *si = icon ("search", 24);
  gtk_widget_set_opacity (si, 0.8);
  gtk_box_append (GTK_BOX (search), si);
  b->tab_filter = gtk_entry_new ();
  gtk_widget_add_css_class (b->tab_filter, "neo-entry");
  gtk_widget_set_hexpand (b->tab_filter, TRUE);
  gtk_widget_set_valign (b->tab_filter, GTK_ALIGN_CENTER);
  gtk_entry_set_placeholder_text (GTK_ENTRY (b->tab_filter), "Search your tabs");
  g_signal_connect_swapped (b->tab_filter, "changed", G_CALLBACK (fill_switcher), b);
  gtk_box_append (GTK_BOX (search), b->tab_filter);
  gtk_box_append (GTK_BOX (box), search);

  b->tab_grid = gtk_flow_box_new ();
  gtk_widget_add_css_class (b->tab_grid, "tabs");
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (b->tab_grid), TRUE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (b->tab_grid), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (b->tab_grid), 6);
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (b->tab_grid), GTK_SELECTION_NONE);
  gtk_widget_set_valign (b->tab_grid, GTK_ALIGN_START);
  gtk_widget_set_margin_start (b->tab_grid, 4);
  gtk_widget_set_margin_end (b->tab_grid, 4);
  gtk_box_append (GTK_BOX (box), scroller_new (b->tab_grid));
  return box;
}

typedef struct {
  Browser *b;
  guint    tab_id;
} SnapshotJob;

/* the current page as a small picture for its card: WebKit draws it at the screen's size, the GPU shrinks it */
static void
on_snapshot (GObject *src, GAsyncResult *res, gpointer data)
{
  SnapshotJob *job = data;
  Browser *b = job->b;
  Tab *t = tab_by_id (b, job->tab_id);
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) tex = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (src), res, &error);

  g_free (job);
  if (!tex) {
    g_message ("neo-browser: no snapshot: %s", error->message);
    return;
  }
  if (!t)
    return;
  double w = gdk_texture_get_width (tex), h = gdk_texture_get_height (tex), k = MIN (1.0, 480.0 / w);
  double ch = MIN (h, w * 1.3) * k;   /* the top of the page: what a card shows */
  GtkSnapshot *s = gtk_snapshot_new ();
  gtk_snapshot_scale (s, k, k);
  gdk_paintable_snapshot (GDK_PAINTABLE (tex), GDK_SNAPSHOT (s), w, h);
  GskRenderNode *node = gtk_snapshot_free_to_node (s);
  if (!node)
    return;
  graphene_rect_t rect = GRAPHENE_RECT_INIT (0, 0, w * k, ch);
  g_clear_object (&t->thumb);
  t->thumb = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (b->win)), node, &rect);
  gsk_render_node_unref (node);
  if (g_str_equal (page_name (b), "tabs"))
    fill_switcher (b);
}

static void
snapshot (Browser *b, Tab *t)
{
  if (!t || !t->view || t->ntp)
    return;
  SnapshotJob *job = g_new0 (SnapshotJob, 1);
  job->b = b;
  job->tab_id = t->id;
  webkit_web_view_get_snapshot (t->view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL,
                                on_snapshot, job);
}

static void
open_switcher (Browser *b)
{
  b->switcher_incognito = b->current && b->current->incognito;
  gtk_editable_set_text (GTK_EDITABLE (b->tab_filter), "");
  show_page (b, "tabs");
  fill_switcher (b);
  snapshot (b, b->current);
}

static void
on_card_close (GtkButton *button, Browser *b)
{
  Tab *t = tab_by_id (b, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "tab")));
  if (t)
    close_tab (b, t);
}

static void
on_card_pick (GtkGestureClick *click, int n, double x, double y, Browser *b)
{
  GtkWidget *w = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (click));
  Tab *t = tab_by_id (b, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (w), "tab")));
  if (t)
    select_tab (b, t);
}

static void
fill_switcher (Browser *b)
{
  gboolean inc = b->switcher_incognito;
  g_autofree char *normal = g_strdup_printf ("%u", count_tabs (b, FALSE));
  gtk_label_set_text (GTK_LABEL (b->seg_normal_count), normal);
  if (inc) {
    gtk_widget_remove_css_class (b->seg_normal, "on");
    gtk_widget_add_css_class (b->seg_incog, "on");
  } else {
    gtk_widget_add_css_class (b->seg_normal, "on");
    gtk_widget_remove_css_class (b->seg_incog, "on");
  }
  g_autofree char *f = g_utf8_strdown (gtk_editable_get_text (GTK_EDITABLE (b->tab_filter)), -1);
  gtk_flow_box_remove_all (GTK_FLOW_BOX (b->tab_grid));
  for (guint i = 0; i < b->tabs->len; i++) {
    Tab *t = b->tabs->pdata[i];
    if (t->incognito != inc)
      continue;
    if (*f) {
      g_autofree char *hay = g_strdup_printf ("%s %s", tab_name (t), tab_uri (t));
      g_autofree char *lower = g_utf8_strdown (hay, -1);
      if (!strstr (lower, f))
        continue;
    }
    gboolean sel = t == b->current;
    g_autofree char *host = nb_host (tab_uri (t));
    GtkWidget *card = vbox (0);
    gtk_widget_add_css_class (card, "tab-card");
    if (sel)
      gtk_widget_add_css_class (card, "sel");
    if (t->incognito)
      gtk_widget_add_css_class (card, "incognito");
    gtk_widget_set_size_request (card, -1, 250);
    GtkWidget *top = hbox (8);
    gtk_widget_set_margin_start (top, 6);
    gtk_widget_set_size_request (top, -1, 44);
    GdkTexture *fav = t->view ? webkit_web_view_get_favicon (t->view) : NULL;
    if (fav) {
      GtkWidget *image = gtk_image_new_from_paintable (GDK_PAINTABLE (fav));
      gtk_image_set_pixel_size (GTK_IMAGE (image), 22);
      gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (top), image);
    } else {
      g_autofree char *letter = t->ntp ? g_strdup ("N") : nb_letter (tab_uri (t));
      GtkWidget *l = gtk_label_new (letter);
      gtk_widget_add_css_class (l, "favletter");
      gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (top), l);
    }
    const char *name = tab_name (t);
    GtkWidget *title = label (t->ntp ? "New tab" : *name ? name : *host ? host : "New tab", "tab-title",
                              PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (top), title);
    GtkWidget *close = gtk_button_new ();
    gtk_button_set_child (GTK_BUTTON (close), icon (sel ? "close_dark" : "close", 22));
    gtk_widget_add_css_class (close, "neo-icon");
    gtk_widget_set_valign (close, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (close), "tab", GUINT_TO_POINTER (t->id));
    g_signal_connect (close, "clicked", G_CALLBACK (on_card_close), b);
    gtk_box_append (GTK_BOX (top), close);
    gtk_box_append (GTK_BOX (card), top);
    GtkWidget *thumb = hbox (0);
    gtk_widget_add_css_class (thumb, "tab-thumb");
    gtk_widget_set_vexpand (thumb, TRUE);
    gtk_widget_set_overflow (thumb, GTK_OVERFLOW_HIDDEN);
    if (t->thumb && !t->ntp) {
      GtkWidget *pic = gtk_picture_new_for_paintable (GDK_PAINTABLE (t->thumb));
      gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
      gtk_picture_set_can_shrink (GTK_PICTURE (pic), TRUE);
      gtk_widget_set_hexpand (pic, TRUE);
      gtk_widget_set_vexpand (pic, TRUE);
      gtk_box_append (GTK_BOX (thumb), pic);
    } else {
      GtkWidget *l = gtk_label_new (t->ntp ? "New tab" : *host ? host : "New tab");
      gtk_widget_set_hexpand (l, TRUE);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gtk_label_set_justify (GTK_LABEL (l), GTK_JUSTIFY_CENTER);
      gtk_box_append (GTK_BOX (thumb), l);
    }
    g_object_set_data (G_OBJECT (thumb), "tab", GUINT_TO_POINTER (t->id));
    GtkGesture *click = gtk_gesture_click_new ();
    g_signal_connect (click, "released", G_CALLBACK (on_card_pick), b);
    gtk_widget_add_controller (thumb, GTK_EVENT_CONTROLLER (click));
    gtk_box_append (GTK_BOX (card), thumb);
    gtk_flow_box_append (GTK_FLOW_BOX (b->tab_grid), card);
  }
}

/* ---- history, bookmarks, downloads ---- */

static void
on_list_row (GtkListBox *list, GtkListBoxRow *row, Browser *b)
{
  RowFn fn = g_object_get_data (G_OBJECT (row), "fn");
  if (fn)
    fn (b, g_object_get_data (G_OBJECT (row), "arg"));
}

static GtkWidget *
build_list (Browser *b)
{
  GtkWidget *box = vbox (0);
  gtk_widget_add_css_class (box, "neo-bg");
  GtkWidget *bar = hbox (4);
  gtk_widget_add_css_class (bar, "neo-bar");
  gtk_box_append (GTK_BOX (bar), page_back_button (b));
  b->list_title = label ("", "title-big", PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_hexpand (b->list_title, TRUE);
  gtk_box_append (GTK_BOX (bar), b->list_title);
  b->list_delete = icon_button ("delete", G_CALLBACK (ask_clear_data), b);
  gtk_box_append (GTK_BOX (bar), b->list_delete);
  gtk_box_append (GTK_BOX (box), bar);
  b->list = gtk_list_box_new ();
  gtk_widget_add_css_class (b->list, "neo-list");
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (b->list), GTK_SELECTION_NONE);
  g_signal_connect (b->list, "row-activated", G_CALLBACK (on_list_row), b);
  b->list_scroll = scroller_new (b->list);
  gtk_box_append (GTK_BOX (box), b->list_scroll);
  return box;
}

static GtkWidget *
list_row (const char *dot, const char *title, const char *sub, RowFn fn, const char *arg)
{
  GtkWidget *box = hbox (16);
  GtkWidget *d = gtk_label_new (dot);
  gtk_widget_add_css_class (d, "row-dot");
  gtk_widget_set_valign (d, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), d);
  GtkWidget *col = vbox (0);
  gtk_widget_set_hexpand (col, TRUE);
  gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
  GtkWidget *t = label (title, NULL, PANGO_ELLIPSIZE_END);
  GtkWidget *s = label (sub, "sub", PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (col), t);
  gtk_box_append (GTK_BOX (col), s);
  gtk_box_append (GTK_BOX (box), col);
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data (G_OBJECT (row), "fn", fn);
  g_object_set_data_full (G_OBJECT (row), "arg", g_strdup (arg), g_free);
  g_object_set_data (G_OBJECT (row), "title", t);
  g_object_set_data (G_OBJECT (row), "sub", s);
  return row;
}

static void
open_url (Browser *b, const char *url)
{
  browse (b, url);
}

static GtkWidget *download_row (Browser *b, Download *e);

/* 2000 rows at once is a second's freeze on the phone: the first screenful now, the rest as it idles */
static gboolean
list_step (gpointer data)
{
  Browser *b = data;
  GPtrArray *items = g_str_equal (b->list_kind, "history") ? b->store->history : b->store->bookmarks;
  guint end = MIN (items->len, b->list_i + (b->list_i ? 60 : 20));

  for (; b->list_i < end; b->list_i++) {
    NbEntry *h = items->pdata[b->list_i];
    g_autofree char *letter = nb_letter (h->url);
    g_autofree char *host = nb_host (h->url);
    gtk_list_box_append (GTK_LIST_BOX (b->list),
                         list_row (letter, *h->title ? h->title : h->url, host, open_url, h->url));
  }
  if (b->list_i < items->len)
    return G_SOURCE_CONTINUE;
  b->list_idle = 0;
  return G_SOURCE_REMOVE;
}

static void
show_list (Browser *b, const char *kind)
{
  b->list_kind = kind;
  gtk_label_set_text (GTK_LABEL (b->list_title), g_str_equal (kind, "history") ? "History"
                                                 : g_str_equal (kind, "bookmarks") ? "Bookmarks" : "Downloads");
  gtk_widget_set_visible (b->list_delete, g_str_equal (kind, "history"));
  g_clear_handle_id (&b->list_idle, g_source_remove);
  gtk_list_box_remove_all (GTK_LIST_BOX (b->list));
  scroller_to_top (b->list_scroll);
  show_page (b, "list");
  if (g_str_equal (kind, "downloads")) {
    for (guint i = 0; i < b->downloads->len; i++)
      gtk_list_box_append (GTK_LIST_BOX (b->list), download_row (b, b->downloads->pdata[i]));
    return;
  }
  b->list_i = 0;
  if (list_step (b) == G_SOURCE_CONTINUE)
    b->list_idle = g_idle_add_full (G_PRIORITY_LOW, list_step, b, NULL);
}

/* ---- downloads ---- */

static char *
download_text (Download *e)
{
  if (e->failed)
    return g_strdup ("Failed");
  if (e->done) {
    g_autofree char *size = g_format_size (webkit_download_get_received_data_length (e->d));
    return g_strdup_printf ("Done · %s", size);
  }
  return g_strdup_printf ("%d %%", (int) round (100 * webkit_download_get_estimated_progress (e->d)));
}

static void
sync_download (Browser *b, Download *e)
{
  if (!e->row || !b->list_kind || !g_str_equal (b->list_kind, "downloads"))
    return;
  g_autofree char *text = download_text (e);
  gtk_label_set_text (g_object_get_data (G_OBJECT (e->row), "sub"), text);
  gtk_label_set_text (g_object_get_data (G_OBJECT (e->row), "title"), e->name ? e->name : "Download");
}

static void
open_download (Browser *b, const char *path)
{
  for (guint i = 0; i < b->downloads->len; i++) {
    Download *e = b->downloads->pdata[i];
    if (e->path && g_str_equal (e->path, path) && e->done && !e->failed) {
      g_autoptr (GFile) file = g_file_new_for_path (path);
      g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
      gtk_file_launcher_launch (launcher, GTK_WINDOW (b->win), NULL, NULL, NULL);
      return;
    }
  }
}

static GtkWidget *
download_row (Browser *b, Download *e)
{
  g_autofree char *text = download_text (e);
  GtkWidget *row = list_row ("↓", e->name ? e->name : "Download", text, open_download, e->path ? e->path : "");
  g_set_weak_pointer (&e->row, row);
  return row;
}

static Download *
download_of (Browser *b, WebKitDownload *d)
{
  for (guint i = 0; i < b->downloads->len; i++)
    if (((Download *) b->downloads->pdata[i])->d == d)
      return b->downloads->pdata[i];
  return NULL;
}

static gboolean
on_decide_destination (WebKitDownload *d, const char *suggested, Browser *b)
{
  Download *e = download_of (b, d);
  if (!e)
    return FALSE;
  g_mkdir_with_parents (b->downloads_dir, 0755);
  g_autofree char *base = g_strdup (suggested && *suggested ? suggested : "download");
  g_strdelimit (base, "/", '_');
  const char *dot = strrchr (base, '.');
  gsize stem = dot && dot != base ? (gsize) (dot - base) : strlen (base);
  g_autofree char *path = g_build_filename (b->downloads_dir, base, NULL);
  for (int n = 1; g_file_test (path, G_FILE_TEST_EXISTS); n++) {
    g_autofree char *stem_s = g_strndup (base, stem);
    g_autofree char *name = g_strdup_printf ("%s (%d)%s", stem_s, n, base + stem);
    g_free (path);
    path = g_build_filename (b->downloads_dir, name, NULL);
  }
  webkit_download_set_destination (d, path);
  g_free (e->path);
  e->path = g_strdup (path);
  g_free (e->name);
  e->name = g_path_get_basename (path);
  g_autofree char *msg = g_strdup_printf ("Downloading %s", e->name);
  toast (b, msg);
  sync_download (b, e);
  return TRUE;
}

static void
on_download_progress (WebKitDownload *d, GParamSpec *pspec, Browser *b)
{
  Download *e = download_of (b, d);
  if (e)
    sync_download (b, e);
}

static void
on_download_failed (WebKitDownload *d, GError *error, Browser *b)
{
  Download *e = download_of (b, d);
  if (!e)
    return;
  e->failed = TRUE;
  sync_download (b, e);
}

static void
on_download_finished (WebKitDownload *d, Browser *b)
{
  Download *e = download_of (b, d);
  if (!e)
    return;
  e->done = TRUE;
  sync_download (b, e);
  if (!e->failed) {
    g_autofree char *msg = g_strdup_printf ("Downloaded %s", e->name ? e->name : "the file");
    toast (b, msg);
  }
}

static void
track_download (Browser *b, WebKitDownload *d)
{
  if (!d || g_hash_table_contains (b->dl_seen, d))
    return;
  g_hash_table_add (b->dl_seen, g_object_ref (d));
  Download *e = g_new0 (Download, 1);
  e->d = d;
  g_ptr_array_insert (b->downloads, 0, e);
  g_signal_connect (d, "decide-destination", G_CALLBACK (on_decide_destination), b);
  g_signal_connect (d, "notify::estimated-progress", G_CALLBACK (on_download_progress), b);
  g_signal_connect (d, "failed", G_CALLBACK (on_download_failed), b);
  g_signal_connect (d, "finished", G_CALLBACK (on_download_finished), b);
}

typedef struct {
  Browser *b;
  char    *path;
} SaveJob;

static void
on_saved (GObject *src, GAsyncResult *res, gpointer data)
{
  SaveJob *job = data;
  if (webkit_web_view_save_to_file_finish (WEBKIT_WEB_VIEW (src), res, NULL)) {
    g_autofree char *name = g_path_get_basename (job->path);
    g_autofree char *msg = g_strdup_printf ("Saved %s", name);
    toast (job->b, msg);
  } else {
    toast (job->b, "Page not saved");
  }
  g_free (job->path);
  g_free (job);
}

static void
save_page (Browser *b)
{
  WebKitWebView *v = b->current ? b->current->view : NULL;
  if (!v)
    return;
  g_autofree char *host = nb_host (webkit_web_view_get_uri (v));
  const char *title = webkit_web_view_get_title (v);
  g_autofree char *base = g_strdup (title && *title ? title : *host ? host : "page");
  g_strdelimit (base, "/\\:*?\"<>|", '_');
  if (g_utf8_strlen (base, -1) > 80)
    *g_utf8_offset_to_pointer (base, 80) = '\0';
  g_mkdir_with_parents (b->downloads_dir, 0755);
  g_autofree char *name = g_strdup_printf ("%s.mhtml", base);
  g_autofree char *path = g_build_filename (b->downloads_dir, name, NULL);
  for (int n = 1; g_file_test (path, G_FILE_TEST_EXISTS); n++) {
    g_autofree char *other = g_strdup_printf ("%s (%d).mhtml", base, n);
    g_free (path);
    path = g_build_filename (b->downloads_dir, other, NULL);
  }
  g_autoptr (GFile) file = g_file_new_for_path (path);
  SaveJob *job = g_new0 (SaveJob, 1);
  job->b = b;
  job->path = g_strdup (path);
  webkit_web_view_save_to_file (v, file, WEBKIT_SAVE_MODE_MHTML, NULL, on_saved, job);
}

/* ---- sheets: the menu (top right, no dimming) and the link sheet (bottom, dimmed) ---- */

static void
sheet_close (Sheet *s)
{
  gtk_widget_set_visible (s->scrim, FALSE);
  gtk_widget_set_visible (s->holder, FALSE);
}

static void
on_scrim (GtkGestureClick *click, int n, double x, double y, Browser *b)
{
  sheet_close (&b->menu);
  sheet_close (&b->link_sheet);
  sync_back (b);
}

static void
sheet_init (Browser *b, Sheet *s, GtkAlign halign, GtkAlign valign, gboolean dim)
{
  s->scrim = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (s->scrim, dim ? "scrim" : "scrim-clear");
  gtk_widget_set_visible (s->scrim, FALSE);
  gtk_widget_set_hexpand (s->scrim, TRUE);
  gtk_widget_set_vexpand (s->scrim, TRUE);
  s->holder = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (s->holder), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (s->holder), TRUE);
  gtk_widget_add_css_class (s->holder, "sheet");
  if (valign == GTK_ALIGN_END)
    gtk_widget_add_css_class (s->holder, "bottom");
  gtk_widget_set_halign (s->holder, halign);
  gtk_widget_set_valign (s->holder, valign);
  gtk_widget_set_visible (s->holder, FALSE);
  gtk_widget_set_overflow (s->holder, GTK_OVERFLOW_HIDDEN);
  if (valign == GTK_ALIGN_START) {
    gtk_widget_set_margin_top (s->holder, 8);
    gtk_widget_set_margin_end (s->holder, 8);
    gtk_widget_set_size_request (s->holder, 300, -1);
  }
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_scrim), b);
  gtk_widget_add_controller (s->scrim, GTK_EVENT_CONTROLLER (click));
  gtk_overlay_add_overlay (GTK_OVERLAY (b->overlay), s->scrim);
  gtk_overlay_add_overlay (GTK_OVERLAY (b->overlay), s->holder);
}

static void
sheet_open (Browser *b, Sheet *s, GtkWidget *child)
{
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s->holder), child);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (s->holder),
                                              MAX (200, gtk_widget_get_height (b->win) - 24));
  gtk_widget_set_visible (s->scrim, TRUE);
  gtk_widget_set_visible (s->holder, TRUE);
  sync_back (b);
}

static void
on_menu_row (GtkButton *button, Browser *b)
{
  RowFn fn = g_object_get_data (G_OBJECT (button), "fn");
  g_autofree char *arg = g_strdup (g_object_get_data (G_OBJECT (button), "arg"));
  sheet_close (&b->menu);
  sheet_close (&b->link_sheet);
  sync_back (b);
  fn (b, arg);
}

static GtkWidget *
menu_row (Browser *b, const char *icon_name, const char *text, RowFn fn, const char *arg, int check)
{
  GtkWidget *box = hbox (20);
  gtk_box_append (GTK_BOX (box), icon (icon_name, 24));
  GtkWidget *l = label (text, NULL, PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_hexpand (l, TRUE);
  gtk_box_append (GTK_BOX (box), l);
  if (check >= 0) {
    GtkWidget *cb = gtk_check_button_new ();
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cb), check);
    gtk_widget_set_can_target (cb, FALSE);
    gtk_box_append (GTK_BOX (box), cb);
  }
  GtkWidget *button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (button), box);
  gtk_widget_add_css_class (button, "menu-row");
  g_object_set_data (G_OBJECT (button), "fn", fn);
  g_object_set_data_full (G_OBJECT (button), "arg", g_strdup (arg), g_free);
  g_signal_connect (button, "clicked", G_CALLBACK (on_menu_row), b);
  return button;
}

static GtkWidget *
divider (void)
{
  GtkWidget *d = hbox (0);
  gtk_widget_add_css_class (d, "divider");
  return d;
}

static void
act_back (Browser *b, const char *a)
{
  if (b->current && b->current->view)
    webkit_web_view_go_back (b->current->view);
}

static void
act_forward (Browser *b, const char *a)
{
  if (b->current && b->current->view)
    webkit_web_view_go_forward (b->current->view);
}

static void
act_save (Browser *b, const char *a)
{
  save_page (b);
}

static void
act_history (Browser *b, const char *a)
{
  show_list (b, "history");
}

static void
act_downloads (Browser *b, const char *a)
{
  show_list (b, "downloads");
}

static void
act_bookmarks (Browser *b, const char *a)
{
  show_list (b, "bookmarks");
}

static void
act_clear (Browser *b, const char *a)
{
  ask_clear_data (b);
}

static void
act_find (Browser *b, const char *a)
{
  open_find (b);
}

static void
act_reload (Browser *b, const char *a)
{
  WebKitWebView *v = b->current ? b->current->view : NULL;
  if (!v)
    return;
  if (webkit_web_view_is_loading (v))
    webkit_web_view_stop_loading (v);
  else
    webkit_web_view_reload (v);
}

static void
act_bookmark (Browser *b, const char *uri)
{
  WebKitWebView *v = b->current ? b->current->view : NULL;
  nb_store_toggle_bookmark (b->store, uri, v ? webkit_web_view_get_title (v) : "");
  toast (b, nb_store_is_bookmarked (b->store, uri) ? "Bookmarked" : "Bookmark removed");
}

static void
act_new_tab (Browser *b, const char *a)
{
  new_tab (b, NULL, FALSE, TRUE, NULL);
  open_omnibox (b, "");
}

static void
act_new_incognito (Browser *b, const char *a)
{
  new_tab (b, NULL, TRUE, TRUE, NULL);
}

static void
copy (Browser *b, const char *text)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (b->win), text);
}

static void
act_copy_link (Browser *b, const char *uri)
{
  if (uri && *uri) {
    copy (b, uri);
    toast (b, "Link copied");
  }
}

static void
act_desktop (Browser *b, const char *a)
{
  Tab *t = b->current;
  if (!t)
    return;
  t->desktop = !t->desktop;
  if (!t->view)
    return;
  webkit_settings_set_user_agent (webkit_web_view_get_settings (t->view), t->desktop ? DESKTOP_UA : MOBILE_UA);
  /* a desktop page laid out ~720 px wide, as a phone's desktop mode */
  webkit_web_view_set_zoom_level (t->view, t->desktop ? 0.5 : 1);
  webkit_web_view_reload (t->view);
}

static void
act_adblock (Browser *b, const char *a)
{
  set_adblock (b, !nb_store_adblock (b->store));
  toast (b, nb_store_adblock (b->store) ? "Ads are blocked" : "Ads are allowed");
}

static void
top_button (Browser *b, GtkWidget *top, const char *icon_name, gboolean on, RowFn fn, const char *arg)
{
  GtkWidget *button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (button), icon (icon_name, 24));
  gtk_widget_set_sensitive (button, on);
  g_object_set_data (G_OBJECT (button), "fn", fn);
  g_object_set_data_full (G_OBJECT (button), "arg", g_strdup (arg), g_free);
  g_signal_connect (button, "clicked", G_CALLBACK (on_menu_row), b);
  gtk_box_append (GTK_BOX (top), button);
}

static void
open_menu (Browser *b)
{
  Tab *t = b->current;
  WebKitWebView *v = t && !t->ntp ? t->view : NULL;
  const char *uri = v && webkit_web_view_get_uri (v) ? webkit_web_view_get_uri (v) : "";
  GtkWidget *col = vbox (0);
  GtkWidget *top = hbox (0);
  gtk_widget_add_css_class (top, "menu-top");
  gtk_widget_set_halign (top, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom (top, 4);
  top_button (b, top, "back", v && webkit_web_view_can_go_back (v), act_back, NULL);
  top_button (b, top, "forward", v && webkit_web_view_can_go_forward (v), act_forward, NULL);
  gboolean marked = *uri && nb_store_is_bookmarked (b->store, uri);
  top_button (b, top, marked ? "star" : "star_border", *uri != '\0', act_bookmark, uri);
  top_button (b, top, "download", *uri != '\0', act_save, NULL);
  top_button (b, top, v && webkit_web_view_is_loading (v) ? "close" : "refresh", v != NULL, act_reload, NULL);
  gtk_box_append (GTK_BOX (col), top);
  gtk_box_append (GTK_BOX (col), menu_row (b, "add", "New tab", act_new_tab, NULL, -1));
  gtk_box_append (GTK_BOX (col), menu_row (b, "incognito", "New Incognito tab", act_new_incognito, NULL, -1));
  gtk_box_append (GTK_BOX (col), divider ());
  gtk_box_append (GTK_BOX (col), menu_row (b, "history", "History", act_history, NULL, -1));
  gtk_box_append (GTK_BOX (col), menu_row (b, "delete", "Delete browsing data", act_clear, NULL, -1));
  gtk_box_append (GTK_BOX (col), divider ());
  gtk_box_append (GTK_BOX (col), menu_row (b, "download", "Downloads", act_downloads, NULL, -1));
  gtk_box_append (GTK_BOX (col), menu_row (b, "bookmarks", "Bookmarks", act_bookmarks, NULL, -1));
  gtk_box_append (GTK_BOX (col), divider ());
  gtk_box_append (GTK_BOX (col), menu_row (b, "share", "Copy link", act_copy_link, uri, -1));
  gtk_box_append (GTK_BOX (col), menu_row (b, "find", "Find in page", act_find, NULL, -1));
  gtk_box_append (GTK_BOX (col), menu_row (b, "desktop", "Desktop site", act_desktop, NULL, t && t->desktop));
  gtk_box_append (GTK_BOX (col), menu_row (b, "shield", "Block ads", act_adblock, NULL, nb_store_adblock (b->store)));
  sheet_open (b, &b->menu, col);
}

static void
act_open_background (Browser *b, const char *link)
{
  Tab *t = b->current;
  new_tab (b, link, t && t->incognito, FALSE, NULL);
  toast (b, "Opened in a new tab");
}

static void
act_open_incognito (Browser *b, const char *link)
{
  new_tab (b, link, TRUE, TRUE, NULL);
}

static void
act_open_image (Browser *b, const char *media)
{
  Tab *t = b->current;
  new_tab (b, media, t && t->incognito, TRUE, NULL);
}

static void
act_download (Browser *b, const char *uri)
{
  if (!b->current || !b->current->view)
    return;
  WebKitDownload *d = webkit_web_view_download_uri (b->current->view, uri);
  track_download (b, d);
  g_object_unref (d);
}

static void
context_sheet (Browser *b, Tab *t, WebKitHitTestResult *hit)
{
  const char *link = webkit_hit_test_result_context_is_link (hit) ? webkit_hit_test_result_get_link_uri (hit) : NULL;
  const char *media = webkit_hit_test_result_context_is_image (hit) ? webkit_hit_test_result_get_image_uri (hit)
                                                                    : NULL;
  if (!link && !media)
    return;
  GtkWidget *col = vbox (0);
  GtkWidget *head = label (link ? link : media, "dim", PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_margin_start (head, 20);
  gtk_widget_set_margin_end (head, 20);
  gtk_widget_set_margin_bottom (head, 8);
  gtk_box_append (GTK_BOX (col), head);
  if (link) {
    gtk_box_append (GTK_BOX (col), menu_row (b, "add", "Open in new tab", act_open_background, link, -1));
    gtk_box_append (GTK_BOX (col), menu_row (b, "incognito", "Open in Incognito tab", act_open_incognito, link, -1));
    gtk_box_append (GTK_BOX (col), menu_row (b, "share", "Copy link address", act_copy_link, link, -1));
    gtk_box_append (GTK_BOX (col), menu_row (b, "download", "Download link", act_download, link, -1));
  }
  if (media) {
    gtk_box_append (GTK_BOX (col), menu_row (b, "download", "Download image", act_download, media, -1));
    gtk_box_append (GTK_BOX (col), menu_row (b, "add", "Open image in new tab", act_open_image, media, -1));
  }
  sheet_open (b, &b->link_sheet, col);
}

static void
on_clear_response (AdwAlertDialog *d, const char *response, Browser *b)
{
  if (!g_str_equal (response, "delete"))
    return;
  nb_store_clear_history (b->store);
  webkit_website_data_manager_clear (webkit_network_session_get_website_data_manager (b->session),
                                     WEBKIT_WEBSITE_DATA_ALL, 0, NULL, NULL, NULL);
  if (b->list_kind && g_str_equal (b->list_kind, "history") && g_str_equal (page_name (b), "list"))
    show_list (b, "history");
  toast (b, "Browsing data deleted");
}

static void
ask_clear_data (Browser *b)
{
  AdwDialog *d = adw_alert_dialog_new ("Delete browsing data",
                                       "Deletes history, cookies, site data and cached images and files. "
                                       "Bookmarks and downloads stay.");
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", "Cancel");
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "delete", "Delete data");
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect (d, "response", G_CALLBACK (on_clear_response), b);
  adw_dialog_present (d, b->win);
}

static void
toast (Browser *b, const char *text)
{
  AdwToast *t = adw_toast_new (text);
  adw_toast_set_timeout (t, 2);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (b->toasts), t);
}

/* ---- shortcuts ---- */

static gboolean
sc_back (GtkWidget *w, GVariant *args, gpointer data)
{
  go_back (data);
  return TRUE;
}

static gboolean
sc_address (GtkWidget *w, GVariant *args, gpointer data)
{
  on_pill (data);
  return TRUE;
}

static gboolean
sc_new_tab (GtkWidget *w, GVariant *args, gpointer data)
{
  act_new_tab (data, NULL);
  return TRUE;
}

static gboolean
sc_find (GtkWidget *w, GVariant *args, gpointer data)
{
  open_find (data);
  return TRUE;
}

static void
add_shortcut (GtkEventController *sc, const char *trigger, GtkShortcutFunc fn, Browser *b)
{
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (sc),
                                        gtk_shortcut_new (gtk_shortcut_trigger_parse_string (trigger),
                                                          gtk_callback_action_new (fn, b, NULL)));
}

static void
shortcuts (Browser *b)
{
  GtkEventController *sc = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (sc, GTK_PHASE_CAPTURE);
  gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (sc), GTK_SHORTCUT_SCOPE_GLOBAL);
  add_shortcut (sc, "<Alt>Left|Back", sc_back, b);   /* the shell's back swipe sends Alt+Left when it can't click */
  add_shortcut (sc, "<Control>l", sc_address, b);
  add_shortcut (sc, "<Control>t", sc_new_tab, b);
  add_shortcut (sc, "<Control>f", sc_find, b);
  gtk_widget_add_controller (b->win, sc);
}

/* ---- the window ---- */

static gboolean
on_close_request (GtkWindow *win, Browser *b)
{
  g_clear_handle_id (&b->session_id, g_source_remove);
  write_session (b);
  nb_store_flush (b->store);
  return FALSE;
}

static Browser *
browser_new (GtkApplication *app)
{
  Browser *b = g_new0 (Browser, 1);

  b->app = app;
  b->data_dir = g_build_filename (g_get_user_data_dir (), APP_ID, NULL);
  b->cache_dir = g_build_filename (g_get_user_cache_dir (), APP_ID, NULL);
  g_autofree char *old = g_build_filename (g_get_user_data_dir (), "neobrowser", NULL);
  const char *dl = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
  b->downloads_dir = dl ? g_strdup (dl) : g_build_filename (g_get_home_dir (), "Downloads", NULL);
  b->store = nb_store_new (b->data_dir, old);
  b->tabs = g_ptr_array_new ();
  b->downloads = g_ptr_array_new ();
  b->dl_seen = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);

  adw_style_manager_set_color_scheme (adw_style_manager_get_default (), ADW_COLOR_SCHEME_FORCE_DARK);
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, CSS);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  setup_engine (b);
  b->win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (b->win), "Neo Browser");
  gtk_window_set_default_size (GTK_WINDOW (b->win), 360, 750);
  gtk_widget_add_css_class (b->win, "neo");
  b->toasts = adw_toast_overlay_new ();
  b->overlay = gtk_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (b->toasts), b->overlay);
  b->pages = gtk_stack_new ();
  gtk_widget_add_css_class (b->pages, "neo-bg");
  gtk_overlay_set_child (GTK_OVERLAY (b->overlay), b->pages);
  gtk_stack_add_named (GTK_STACK (b->pages), build_browse (b), "browse");
  gtk_stack_add_named (GTK_STACK (b->pages), build_omnibox (b), "omni");
  gtk_stack_add_named (GTK_STACK (b->pages), build_switcher (b), "tabs");
  gtk_stack_add_named (GTK_STACK (b->pages), build_list (b), "list");
  /* over every page (the switcher has no back button of its own), in the top band the probe walks */
  b->back_probe = gtk_button_new ();
  gtk_widget_add_css_class (b->back_probe, "back-probe");
  gtk_widget_set_focusable (b->back_probe, FALSE);
  gtk_widget_set_can_target (b->back_probe, FALSE);
  gtk_widget_set_halign (b->back_probe, GTK_ALIGN_START);
  gtk_widget_set_valign (b->back_probe, GTK_ALIGN_START);
  set_back_name (b->back_probe);
  g_signal_connect_swapped (b->back_probe, "clicked", G_CALLBACK (go_back), b);
  gtk_overlay_add_overlay (GTK_OVERLAY (b->overlay), b->back_probe);
  sheet_init (b, &b->menu, GTK_ALIGN_END, GTK_ALIGN_START, FALSE);
  sheet_init (b, &b->link_sheet, GTK_ALIGN_FILL, GTK_ALIGN_END, TRUE);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (b->win), b->toasts);
  shortcuts (b);
  g_signal_connect (b->win, "close-request", G_CALLBACK (on_close_request), b);

  JsonNode *session = nb_store_load (b->store, "session.json");
  int current = 0;
  if (session && JSON_NODE_HOLDS_OBJECT (session)) {
    JsonObject *o = json_node_get_object (session);
    current = (int) json_object_get_int_member_with_default (o, "current", 0);
    JsonArray *tabs = json_object_has_member (o, "tabs") ? json_object_get_array_member (o, "tabs") : NULL;
    for (guint i = 0; tabs && i < json_array_get_length (tabs); i++) {
      JsonNode *n = json_array_get_element (tabs, i);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      const char *url = json_object_get_string_member_with_default (json_node_get_object (n), "url", "");
      if (!*url)
        continue;
      Tab *t = g_new0 (Tab, 1);
      t->id = ++b->next_tab_id;
      t->url = g_strdup (url);
      t->title = g_strdup (json_object_get_string_member_with_default (json_node_get_object (n), "title", ""));
      g_ptr_array_add (b->tabs, t);
    }
  }
  g_clear_pointer (&session, json_node_unref);
  if (b->tabs->len)
    select_tab (b, b->tabs->pdata[CLAMP (current, 0, (int) b->tabs->len - 1)]);
  else
    new_tab (b, NULL, FALSE, TRUE, NULL);
  gtk_window_present (GTK_WINDOW (b->win));
  return b;
}

static Browser *browser;

static void
on_activate (GtkApplication *app)
{
  if (!browser)
    browser = browser_new (app);
  gtk_window_present (GTK_WINDOW (browser->win));
}

static void
on_open (GApplication *app, GFile **files, int n, const char *hint)
{
  on_activate (GTK_APPLICATION (app));
  for (int i = 0; i < n; i++) {
    g_autofree char *uri = g_file_get_uri (files[i]);
    new_tab (browser, uri, FALSE, TRUE, NULL);
  }
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  if (browser)
    nb_store_free (browser->store);
  return status;
}
