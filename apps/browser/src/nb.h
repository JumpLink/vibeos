/* The browser: LineageOS's Jelly over WebKitGTK (spec/jelly.md). What every part shares.
 *
 *   nb-app.c        the engine (one WebKit network session, the incognito one, the ad filter), the session file,
 *                   downloads, shortcuts, the settings that reach WebKit
 *   nb-tab.c        a tab: its own window (Jelly's tabs are Android tasks), the WebView, back, dialogs
 *   nb-urlbar.c     the URL bar, its suggestions, find in page
 *   nb-menu.c       the popup menu, the link sheet, Send to, the shortcut sheet
 *   nb-pages.c      History, Favorites, Downloads, Desktop shortcuts
 *   nb-settings.c   Settings
 *   nb-widgets.c    dialogs, sheets, switches … in Jelly's AppCompat look
 *   nb-navigator.c  the activity transitions between a tab's pages
 *   nb-refresh.c    pull to refresh
 *   store.c         history, favourites, prefs (JSON, the old browser's files) */
#pragma once

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "store.h"

G_BEGIN_DECLS

#define NB_APP_ID "de.yesman.NeoBrowser"
#define NB_RESOURCE_ICONS "/de/yesman/NeoBrowser/icons/scalable/actions/"
#define NB_DEFAULT_HOME "https://google.com"
#define NB_DEFAULT_ENGINE "https://google.com/search?ie=UTF-8&source=android-browser&q={searchTerms}"

typedef struct _NbTab NbTab;

typedef struct {
  WebKitDownload *d;          /* NULL for a file found in ~/Downloads */
  char           *name;
  char           *path;
  char           *mime;
  guint64         size;
  gint64          time;       /* seconds since the epoch */
  gboolean        done, failed;
} NbDownload;

typedef struct {
  char *id;
  char *name;
  char *url;
  char *desktop_file;
} NbShortcut;

typedef struct {
  GtkApplication               *gapp;
  NbStore                      *store;
  char                         *data_dir, *cache_dir, *downloads_dir;
  WebKitNetworkSession         *session, *incog_session;
  WebKitUserContentManager     *ucm;
  WebKitUserContentFilterStore *filters;
  WebKitUserContentFilter      *filter;
  GPtrArray                    *tabs;          /* NbTab, oldest first */
  NbTab                        *last_active;
  GPtrArray                    *downloads;     /* NbDownload, newest first */
  GHashTable                   *dl_seen;
  guint                         session_id;
} NbApp;

extern NbApp *nb;

struct _NbTab {
  GtkWidget     *win;
  GtkWidget     *nav;             /* NbNavigator: the browser page, then History … over it */
  GtkWidget     *page;            /* the browser page (an overlay: the suggestions drop over it) */
  GtkWidget     *column;          /* the bar and the view, top to bottom (reach mode: the other way) */
  GtkWidget     *view_holder;
  GtkWidget     *view_overlay;    /* in view_holder: the view, the pull-to-refresh circle over it */
  GtkWidget     *refresh;
  WebKitWebView *view;            /* made when the window is first active */
  WebKitWebView *related;         /* a popup's opener, until then */
  char          *url;             /* what the view loads when it is made */
  char          *title;
  gboolean       incognito, desktop, fullscreen, pwa_fullscreen;
  char          *shortcut_id;
  gboolean       closing;
  /* the URL bar (nb-urlbar.c) */
  GtkWidget     *bar, *bar_stack, *incog_icon, *lock, *entry, *more, *progress;
  GtkWidget     *find_entry, *find_count, *find_clear, *find_prev, *find_next;
  GtkWidget     *dropdown, *dropdown_rows;
  GCancellable  *suggest_cancel;
  gboolean       editing, finding, setting_text;
  guint          find_n;
  int            find_i;
  char          *find_text;
  /* the page's TLS state, for the lock */
  GTlsCertificate     *cert;
  GTlsCertificateFlags tls_errors;
  gboolean             tls_failed, tls_error_page;
  /* over the page */
  GtkWidget     *menu;            /* the popup menu while it shows */
  GtkWidget     *snackbar;
  GtkWidget     *back_probe;
  GtkWidget     *downloads_page;  /* weak, while it is open */
};

/* nb-app.c */
void        nb_app_startup        (GtkApplication *app);
void        nb_app_shutdown       (void);
/* restores the session (or opens the home page) when there is no window, else raises the most recent one */
void        nb_app_activate       (void);
/* the command line's tabs (an Intent's data): one new tab per URL, or one with the home page when there is none */
void        nb_app_open           (const char *const *urls, gboolean incognito, const char *shortcut_id);
/* a new tab in a window of its own (TabUtils.openInNewTab): `url` NULL for the home page */
NbTab      *nb_open_tab           (const char *url, gboolean incognito, const char *shortcut_id,
                                   WebKitWebView *related);
void        nb_tab_forget         (NbTab *t);
void        nb_save_session       (void);
WebKitNetworkSession *nb_session_for (gboolean incognito);
char       *nb_smart_url          (const char *text);
const char *nb_home_page          (void);
gboolean    nb_home_autoload      (void);
void        nb_apply_settings     (void);          /* JavaScript, cookies, DNT, reach mode: to every tab */
void        nb_apply_view_settings (NbTab *t);
void        nb_clear_cookies      (void);
void        nb_track_download     (WebKitDownload *d);
void        nb_downloads_changed  (void);
void        nb_scan_downloads     (void);          /* adds the files in ~/Downloads not listed yet */
gboolean    nb_add_shortcut       (const char *name, const char *url, GdkTexture *favicon, GError **error);
GPtrArray  *nb_list_shortcuts     (void);          /* NbShortcut */
void        nb_shortcut_free      (NbShortcut *s);

/* nb-tab.c */
NbTab      *nb_tab_new            (const char *url, gboolean incognito, gboolean desktop, const char *shortcut_id,
                                   WebKitWebView *related);
void        nb_tab_present        (NbTab *t);
void        nb_tab_ensure_view    (NbTab *t);
void        nb_tab_load           (NbTab *t, const char *text);    /* through smartUrlFilter */
const char *nb_tab_uri            (NbTab *t);
const char *nb_tab_title          (NbTab *t);
void        nb_tab_sync_back      (NbTab *t);
void        nb_tab_push_page      (NbTab *t, GtkWidget *page);
void        nb_tab_pop_page       (NbTab *t);
void        nb_tab_snackbar       (NbTab *t, const char *text, const char *action, void (*cb) (gpointer),
                                   gpointer data);
void        nb_tab_set_desktop    (NbTab *t, gboolean desktop);
void        nb_tab_add_favorite   (NbTab *t, const char *title, const char *url);
void        nb_tab_ask_download   (NbTab *t, const char *url, const char *name);
void        nb_tab_show_certificate (NbTab *t);
void        nb_tab_apply_reach    (NbTab *t);
void        nb_tab_print          (NbTab *t);
GdkTexture *nb_tab_favicon        (NbTab *t);

/* nb-urlbar.c */
GtkWidget  *nb_urlbar_new         (NbTab *t);
void        nb_urlbar_sync        (NbTab *t);
void        nb_urlbar_progress    (NbTab *t);
void        nb_urlbar_set_finding (NbTab *t, gboolean finding);
void        nb_urlbar_stop_editing (NbTab *t);
void        nb_urlbar_find_counted (NbTab *t, guint n, gboolean failed);

/* nb-menu.c */
void        nb_menu_open          (NbTab *t);
void        nb_menu_close         (NbTab *t);
void        nb_link_sheet         (NbTab *t, const char *url, gboolean image);
void        nb_send_to            (NbTab *t, const char *url);
void        nb_shortcut_sheet     (NbTab *t);

/* nb-refresh.c */
void        nb_refresh_setup      (void);          /* the page script and its message handler, on nb->ucm */
GtkWidget  *nb_refresh_new        (NbTab *t);
void        nb_refresh_progress   (NbTab *t);      /* the view's load state changed */

/* nb-pages.c, nb-settings.c */
GtkWidget  *nb_history_page_new   (NbTab *t);
GtkWidget  *nb_favorites_page_new (NbTab *t);
GtkWidget  *nb_downloads_page_new (NbTab *t);
void        nb_downloads_page_refresh (GtkWidget *page);
GtkWidget  *nb_desktop_shortcuts_page_new (NbTab *t);
GtkWidget  *nb_settings_page_new  (NbTab *t);

/* the search engines (V/search_engines.xml) */
extern const char *const nb_engine_names[];
extern const char *const nb_engine_urls[];

G_END_DECLS
