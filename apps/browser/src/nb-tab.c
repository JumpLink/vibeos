/* nb-tab: one tab, a page of the browser's one window (nb-switcher.c): the navigator with the browser page at its
 * root, the web view and what it asks for, back. */
#include "nb.h"
#include "nb-navigator.h"
#include "nb-widgets.h"

#include <vibeos.h>
#include <spongy-gtk.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

/* The engine is WebKit, so the page is told so: an iPhone's Safari gets every site's mobile layout and no "your
 * browser is not supported" for a Chrome name over a WebKit engine. Desktop mode is Safari on a Mac. */
#define MOBILE_UA "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                  "Version/18.0 Mobile/15E148 Safari/604.1"
#define DESKTOP_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                   "Version/18.0 Safari/605.1.15"
/* On a tablet, a tablet's Safari: no "Mobile", so sites send their tablet (or desktop) layout, at its own zoom */
#define TABLET_UA "Mozilla/5.0 (iPad; CPU OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                  "Version/18.0 Safari/604.1"

static void sync_title (NbTab *t);

const char *
nb_tab_uri (NbTab *t)
{
  const char *uri = t->view ? webkit_web_view_get_uri (t->view) : NULL;
  return uri && *uri ? uri : t->url ? t->url : "";
}

const char *
nb_tab_title (NbTab *t)
{
  const char *title = t->view ? webkit_web_view_get_title (t->view) : NULL;
  return title && *title ? title : t->title ? t->title : "";
}

GdkTexture *
nb_tab_favicon (NbTab *t)
{
  return t->view ? webkit_web_view_get_favicon (t->view) : NULL;
}

/* ---- back ---- */

static gboolean
can_step_back (NbTab *t)
{
  if (t->menu || t->editing || t->finding || t->fullscreen)
    return TRUE;
  return t->view && webkit_web_view_can_go_back (t->view);
}

void
nb_tab_sync_back (NbTab *t)
{
  if (t->back_probe)
    gtk_widget_set_sensitive (t->back_probe, can_step_back (t));
}

static void
step_back (NbTab *t)
{
  if (t->menu)
    nb_menu_close (t);
  else if (t->editing)
    nb_urlbar_stop_editing (t);
  else if (t->finding)
    nb_urlbar_set_finding (t, FALSE);
  else if (t->fullscreen)
    /* the page leaves full screen too */
    webkit_web_view_evaluate_javascript (t->view, "document.exitFullscreen ()", -1, NULL, NULL, NULL, NULL, NULL);
  else if (t->view && webkit_web_view_can_go_back (t->view))
    webkit_web_view_go_back (t->view);
  nb_tab_sync_back (t);
}

void
nb_tab_push_page (NbTab *t, GtkWidget *page)
{
  nb_navigator_push (NB_NAVIGATOR (t->nav), page);
}

void
nb_tab_pop_page (NbTab *t)
{
  nb_navigator_pop (NB_NAVIGATOR (t->nav));
}

/* ---- the snackbar (inverse colours, shown for a long time) ---- */

void
nb_tab_snackbar (NbTab *t, const char *text, const char *action, void (*cb) (gpointer), gpointer data)
{
  vibeos_snackbar_show (VIBEOS_SNACKBAR (t->snackbar), text, action, cb, data, VIBEOS_SNACKBAR_LONG);
}

/* ---- favourites: the favicon's vibrant colour, else primary50 ---- */

/* Palette's vibrant swatch, near enough: the most common saturated mid-light hue among the favicon's pixels */
static char *
vibrant_color (GdkTexture *texture)
{
  if (!texture)
    return NULL;
  int w = gdk_texture_get_width (texture), h = gdk_texture_get_height (texture);
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
    return NULL;
  g_autofree guchar *px = g_malloc ((gsize) w * h * 4);
  gdk_texture_download (texture, px, w * 4);
  double sum[36][3] = { { 0 } };
  int count[36] = { 0 };
  for (int i = 0; i < w * h; i++) {
    /* premultiplied BGRA (GDK_MEMORY_DEFAULT on little endian) */
    double a = px[i * 4 + 3] / 255.0;
    if (a < 0.5)
      continue;
    double b = px[i * 4] / 255.0 / a, g = px[i * 4 + 1] / 255.0 / a, r = px[i * 4 + 2] / 255.0 / a;
    double mx = MAX (r, MAX (g, b)), mn = MIN (r, MIN (g, b)), l = (mx + mn) / 2, d = mx - mn;
    if (d < 1e-6)
      continue;
    double s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (s < 0.35 || l < 0.3 || l > 0.7)
      continue;
    double hue = mx == r ? fmod ((g - b) / d, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    int bin = ((int) (hue * 6 + 36)) % 36;
    sum[bin][0] += r;
    sum[bin][1] += g;
    sum[bin][2] += b;
    count[bin]++;
  }
  int best = -1;
  for (int i = 0; i < 36; i++)
    if (count[i] && (best < 0 || count[i] > count[best]))
      best = i;
  if (best < 0)
    return NULL;
  return g_strdup_printf ("#%02x%02x%02x", (int) (255 * sum[best][0] / count[best]),
                          (int) (255 * sum[best][1] / count[best]), (int) (255 * sum[best][2] / count[best]));
}

void
nb_tab_add_favorite (NbTab *t, const char *title, const char *url)
{
  if (!url || !*url)
    return;
  g_autofree char *color = vibrant_color (nb_tab_favicon (t));
  /* without a colour of its own (no favicon) it is saved with none: its card is then in the theme's colours */
  nb_store_add_favorite (nb->store, url, title && *title ? title : url, color);
  nb_tab_snackbar (t, _("Added to favorites"), NULL, NULL, NULL);
}

/* ---- desktop mode ---- */

void
nb_apply_view_settings (NbTab *t)
{
  if (!t->view)
    return;
  WebKitSettings *s = webkit_web_view_get_settings (t->view);
  webkit_settings_set_user_agent (s, t->desktop ? DESKTOP_UA : nb_is_tablet () ? TABLET_UA : MOBILE_UA);
  /* a desktop site drawn at half size fits a phone's width; a tablet is wide enough for it at its own size */
  webkit_web_view_set_zoom_level (t->view, t->desktop && !nb_is_tablet () ? 0.5 : 1);
  webkit_settings_set_enable_javascript (s, nb_store_get_bool (nb->store, "javascript", TRUE));
  /* windows opened without a user's tap: only with Dynamic popups on */
  webkit_settings_set_javascript_can_open_windows_automatically (s, nb_store_get_bool (nb->store, "dynamicPopups",
                                                                                       FALSE));
  /* no DOM storage in incognito */
  webkit_settings_set_enable_html5_local_storage (s, !t->incognito);
  webkit_settings_set_enable_html5_database (s, !t->incognito);
}

void
nb_tab_set_desktop (NbTab *t, gboolean desktop)
{
  if (t->desktop == !!desktop)
    return;
  t->desktop = !!desktop;
  nb_apply_view_settings (t);
  if (t->view)
    webkit_web_view_reload (t->view);
  nb_save_session ();
}

/* ---- reach mode: the bar at the bottom ---- */

void
nb_tab_apply_reach (NbTab *t)
{
  gboolean reach = nb_store_get_bool (nb->store, "reachMode", FALSE);
  GtkWidget *first = gtk_widget_get_first_child (t->column);

  if (reach && first == t->bar)
    gtk_box_reorder_child_after (GTK_BOX (t->column), t->bar, t->view_holder);
  else if (!reach && first != t->bar)
    gtk_box_reorder_child_after (GTK_BOX (t->column), t->bar, NULL);
  gtk_widget_set_valign (t->dropdown, reach ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_margin_top (t->dropdown, reach ? 0 : 56);
  gtk_widget_set_margin_bottom (t->dropdown, reach ? 56 : 0);
}

/* ---- dialogs the page asks for ---- */

typedef struct {
  WebKitPermissionRequest *req;
  char                    *origin;
} Ask;

static void
ask_free (Ask *ask)
{
  g_object_unref (ask->req);
  g_free (ask->origin);
  g_free (ask);
}

static void
remember_location (const char *origin, gboolean allow)
{
  nb_store_set_origin (nb->store, "locationOrigins", origin, allow);
}

static void
on_location_response (NbDialog *d, int response, Ask *ask)
{
  /* Allow and Block are kept for the origin; Deny for now is not */
  if (response == NB_RESPONSE_OK) {
    remember_location (ask->origin, TRUE);
    webkit_permission_request_allow (ask->req);
  } else {
    if (response == NB_RESPONSE_OTHER)
      remember_location (ask->origin, FALSE);
    webkit_permission_request_deny (ask->req);
  }
}

static void
on_permission_response (NbDialog *d, int response, Ask *ask)
{
  if (response == NB_RESPONSE_OK)
    webkit_permission_request_allow (ask->req);
  else
    webkit_permission_request_deny (ask->req);
}

static char *
origin_of (WebKitWebView *v)
{
  const char *uri = webkit_web_view_get_uri (v);
  g_autoptr (GUri) u = uri ? g_uri_parse (uri, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!u || !g_uri_get_host (u))
    return g_strdup (uri ? uri : "");
  int port = g_uri_get_port (u);
  return port > 0 ? g_strdup_printf ("%s://%s:%d/", g_uri_get_scheme (u), g_uri_get_host (u), port)
                  : g_strdup_printf ("%s://%s/", g_uri_get_scheme (u), g_uri_get_host (u));
}

static gboolean
on_permission (WebKitWebView *v, WebKitPermissionRequest *req, NbTab *t)
{
  g_autofree char *origin = origin_of (v);
  NbDialog *d;

  if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST (req)) {
    if (!nb_store_get_bool (nb->store, "location", TRUE)) {
      webkit_permission_request_deny (req);
      return TRUE;
    }
    int kept = nb_store_get_origin (nb->store, "locationOrigins", origin);
    if (kept >= 0) {
      if (kept)
        webkit_permission_request_allow (req);
      else
        webkit_permission_request_deny (req);
      return TRUE;
    }
    /* a site asks for the location */
    d = nb_dialog_new (_("Provide location?"));
    /* TRANSLATORS: %s is the site (its origin, "https://example.org") */
    g_autofree char *msg = g_strdup_printf (_("%s wants to use your geolocation."), origin);
    nb_dialog_set_message (d, msg);
    nb_dialog_add_button (d, _("Deny for now"), NB_RESPONSE_NEUTRAL);
    nb_dialog_add_button (d, _("Block"), NB_RESPONSE_OTHER);
    nb_dialog_add_button (d, _("Allow"), NB_RESPONSE_OK);
    Ask *ask = g_new0 (Ask, 1);
    ask->req = g_object_ref (req);
    ask->origin = g_strdup (origin);
    g_object_set_data_full (G_OBJECT (d), "nb-data", ask, (GDestroyNotify) ask_free);
g_signal_connect (d, "response", G_CALLBACK (on_location_response), ask);
    nb_dialog_present (d, t->win);
    return TRUE;
  }
  if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST (req)) {
    /* a site asks to play protected media */
    d = nb_dialog_new (origin);
    nb_dialog_set_message (d, _("Allow protected media?"));
    nb_dialog_add_button (d, _("Deny"), NB_RESPONSE_CANCEL);
    nb_dialog_add_button (d, _("Grant"), NB_RESPONSE_OK);
  } else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST (req)) {
    /* the camera or the microphone: asked in the app's dialog style */
    WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST (req);
    gboolean audio = webkit_user_media_permission_is_for_audio_device (m);
    gboolean video = webkit_user_media_permission_is_for_video_device (m);
    /* TRANSLATORS: %s is the site (its origin, "https://example.org") */
    g_autofree char *msg = g_strdup_printf (audio && video ? _("%s wants to use your camera and microphone.")
                                            : video ? _("%s wants to use your camera.")
                                            : _("%s wants to use your microphone."), origin);
    d = nb_dialog_new (NULL);
    nb_dialog_set_message (d, msg);
    nb_dialog_add_button (d, _("Block"), NB_RESPONSE_CANCEL);
    nb_dialog_add_button (d, _("Allow"), NB_RESPONSE_OK);
  } else {
    /* notifications, the clipboard …: none of them is offered to a page */
    webkit_permission_request_deny (req);
    return TRUE;
  }
  Ask *ask = g_new0 (Ask, 1);
  ask->req = g_object_ref (req);
  g_object_set_data_full (G_OBJECT (d), "nb-data", ask, (GDestroyNotify) ask_free);
g_signal_connect (d, "response", G_CALLBACK (on_permission_response), ask);
  nb_dialog_present (d, t->win);
  return TRUE;
}

typedef struct {
  WebKitAuthenticationRequest *req;
  GtkWidget                   *user, *pass;
} Auth;

static void
auth_free (Auth *a)
{
  g_object_unref (a->req);
  g_free (a);
}

static void
on_auth_response (NbDialog *d, int response, Auth *a)
{
  if (response == NB_RESPONSE_OK) {
    WebKitCredential *c = webkit_credential_new (gtk_editable_get_text (GTK_EDITABLE (nb_field_get_entry (a->user))),
                                                 gtk_editable_get_text (GTK_EDITABLE (nb_field_get_entry (a->pass))),
                                                 WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
    webkit_authentication_request_authenticate (a->req, c);
    webkit_credential_free (c);
  } else {
    webkit_authentication_request_cancel (a->req);
  }
}

/* HTTP authentication: the user name and password dialog */
static gboolean
on_authenticate (WebKitWebView *v, WebKitAuthenticationRequest *req, NbTab *t)
{
  NbDialog *d = nb_dialog_new (_("Authentication Required"));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  /* TRANSLATORS: %s is the page's address */
  g_autofree char *detail = g_strdup_printf (_("%s requires a username and password."),
                                             webkit_web_view_get_uri (v));
  Auth *a = g_new0 (Auth, 1);

  gtk_widget_add_css_class (box, "nb-dialog-form");
  gtk_box_append (GTK_BOX (box), nb_label (detail, 14, "nb-dialog-text", FALSE));
  gtk_label_set_wrap (GTK_LABEL (gtk_widget_get_first_child (box)), TRUE);
  a->req = g_object_ref (req);
  a->user = nb_field_new (_("User Name:"), FALSE);
  a->pass = nb_field_new (_("Password:"), TRUE);
  gtk_box_append (GTK_BOX (box), a->user);
  gtk_box_append (GTK_BOX (box), a->pass);
  nb_dialog_set_content (d, box);
  nb_dialog_add_button (d, _("Cancel"), NB_RESPONSE_CANCEL);
  nb_dialog_add_button (d, _("Log in"), NB_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (d), "nb-data", a, (GDestroyNotify) auth_free);
g_signal_connect (d, "response", G_CALLBACK (on_auth_response), a);
  nb_dialog_present (d, t->win);
  return TRUE;
}

/* ---- downloads: asked first ---- */

typedef struct {
  NbTab *t;
  char  *url;
} DownloadAsk;

static void
download_ask_free (DownloadAsk *a)
{
  g_free (a->url);
  g_free (a);
}

static void
on_download_response (NbDialog *d, int response, DownloadAsk *a)
{
  if (response != NB_RESPONSE_OK)
    return;
  WebKitDownload *dl = a->t->view ? webkit_web_view_download_uri (a->t->view, a->url)
                                  : webkit_network_session_download_uri (nb_session_for (a->t->incognito), a->url);
  nb_track_download (dl);
  g_object_unref (dl);
}

/* a download's file name: the response's name, else the URL's last part */
static char *
guess_name (const char *url, const char *name)
{
  if (name && *name)
    return g_strdup (name);
  g_autoptr (GUri) u = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  const char *path = u ? g_uri_get_path (u) : NULL;
  const char *slash = path ? strrchr (path, '/') : NULL;
  if (slash && slash[1])
    return g_uri_unescape_string (slash + 1, NULL);
  return g_strdup ("downloadfile.bin");
}

void
nb_tab_ask_download (NbTab *t, const char *url, const char *name)
{
  g_autofree char *file = guess_name (url, name);
  /* TRANSLATORS: %s is the file's name */
  g_autofree char *msg = g_strdup_printf (_("Do you want to download %s?"), file);
  NbDialog *d = nb_dialog_new (_("Download file"));
  DownloadAsk *a = g_new0 (DownloadAsk, 1);

  a->t = t;
  a->url = g_strdup (url);
  nb_dialog_set_message (d, msg);
  nb_dialog_add_button (d, _("Dismiss"), NB_RESPONSE_CANCEL);
  nb_dialog_add_button (d, _("Download"), NB_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (d), "nb-data", a, (GDestroyNotify) download_ask_free);
g_signal_connect (d, "response", G_CALLBACK (on_download_response), a);
  nb_dialog_present (d, t->win);
}

/* ---- the certificate ---- */

/* "CN=x,O=y,OU=z" → the value of `key` */
static char *
dn_part (const char *dn, const char *key)
{
  if (!dn)
    return NULL;
  g_auto (GStrv) parts = g_strsplit (dn, ",", -1);
  for (int i = 0; parts[i]; i++) {
    char *p = g_strstrip (parts[i]);
    gsize n = strlen (key);
    if (g_ascii_strncasecmp (p, key, n) == 0 && p[n] == '=')
      return g_strdup (p + n + 1);
  }
  return NULL;
}

/* a heading of the certificate sheet, in capitals as Jelly shows them (the strings are not: as Jelly's, they are
 * capitalised in each language's own way) */
static GtkWidget *
cert_heading (const char *heading)
{
  g_autofree char *upper = g_utf8_strup (heading, -1);
  return nb_label (upper, 14, "nb-cert-heading", FALSE);
}

static void
cert_section (GtkWidget *box, const char *heading)
{
  GtkWidget *l = cert_heading (heading);
  gtk_widget_set_margin_top (l, 16);
  gtk_box_append (GTK_BOX (box), l);
}

static void
cert_value (GtkWidget *box, const char *key, const char *value)
{
  if (!value || !*value)
    return;
  GtkWidget *k = nb_label (key, 14, "nb-cert-key", FALSE);
  GtkWidget *v = nb_label (value, 14, "nb-cert-value", FALSE);
  gtk_label_set_wrap (GTK_LABEL (v), TRUE);
  gtk_widget_set_margin_top (k, 5);
  gtk_box_append (GTK_BOX (box), k);
  gtk_box_append (GTK_BOX (box), v);
}

static char *
format_date (GDateTime *dt)
{
  if (!dt)
    return NULL;
  g_autoptr (GDateTime) local = g_date_time_to_local (dt);
  /* the date and the time with its seconds, in the user's language and order ("Nov 10, 2026, 4:41:00 AM" in US
   * English), the hours by the 12/24-hour setting */
  return vibeos_format_date (local, "yMMMdjms");
}

void
nb_tab_show_certificate (NbTab *t)
{
  if (!t->cert)
    return;
  g_autofree char *subject = NULL, *issuer = NULL;
  g_autoptr (GDateTime) from = NULL, until = NULL;
  g_object_get (t->cert, "subject-name", &subject, "issuer-name", &issuer, "not-valid-before", &from,
                "not-valid-after", &until, NULL);
  NbDialog *d = nb_dialog_new (_("SSL Certificate Details"));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "nb-dialog-form");

  const char *trust = _("Connection is secure");
  if (t->tls_errors & G_TLS_CERTIFICATE_EXPIRED)
    trust = _("This certificate has expired.");
  else if (t->tls_errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
    trust = _("This certificate isn't valid yet.");
  else if (t->tls_errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
    trust = _("The name of the site doesn't match the name on the certificate.");
  else if (t->tls_errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
    trust = _("This certificate isn't from a trusted authority.");
  else if (t->tls_errors)
    trust = _("This certificate is invalid.");
  GtkWidget *l = cert_heading (_("Trust"));
  gtk_widget_set_margin_top (l, 8);
  gtk_box_append (GTK_BOX (box), l);
  gtk_box_append (GTK_BOX (box), nb_label (trust, 14, "nb-cert-value", FALSE));
  cert_section (box, _("Domain"));
  g_autoptr (GUri) u = g_uri_parse (nb_tab_uri (t), G_URI_FLAGS_NONE, NULL);
  gtk_box_append (GTK_BOX (box), nb_label (u && g_uri_get_host (u) ? g_uri_get_host (u) : "", 14, "nb-cert-value",
                                           FALSE));
  struct { const char *heading, *dn; } who[] = { { _("Issued to"), subject }, { _("Issued by"), issuer } };
  for (guint i = 0; i < G_N_ELEMENTS (who); i++) {
    cert_section (box, who[i].heading);
    g_autofree char *cn = dn_part (who[i].dn, "CN"), *o = dn_part (who[i].dn, "O"), *ou = dn_part (who[i].dn, "OU");
    cert_value (box, _("Common Name (CN)"), cn);
    cert_value (box, _("Organization (O)"), o);
    cert_value (box, _("Organizational Unit (OU)"), ou);
  }
  cert_section (box, _("Validity Period"));
  g_autofree char *on = format_date (from), *off = format_date (until);
  cert_value (box, _("Issued On"), on);
  cert_value (box, _("Expires On"), off);
  GtkWidget *scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), box);
  gtk_widget_set_vexpand (scroll, TRUE);
  nb_dialog_set_content (d, scroll);
  nb_dialog_add_button (d, _("Dismiss"), NB_RESPONSE_CANCEL);
  nb_dialog_present (d, t->win);
}

/* ---- print (GTK's print dialog) ---- */

void
nb_tab_print (NbTab *t)
{
  if (!t->view)
    return;
  g_autoptr (WebKitPrintOperation) op = webkit_print_operation_new (t->view);
  webkit_print_operation_run_dialog (op, GTK_WINDOW (t->win));
}

/* ---- the web view ---- */

static void
on_title (WebKitWebView *v, GParamSpec *pspec, NbTab *t)
{
  const char *title = webkit_web_view_get_title (v);
  if (!t->incognito && title && *title)
    nb_store_retitle (nb->store, webkit_web_view_get_uri (v), title);
  sync_title (t);
  nb_tabs_changed ();
  nb_save_session ();
}

static void
on_uri (WebKitWebView *v, GParamSpec *pspec, NbTab *t)
{
  nb_urlbar_sync (t);
  nb_save_session ();
}

static void
on_progress (WebKitWebView *v, GParamSpec *pspec, NbTab *t)
{
  nb_urlbar_progress (t);
  nb_tab_sync_back (t);
  nb_refresh_progress (t);
}

static void
on_load_changed (WebKitWebView *v, WebKitLoadEvent ev, NbTab *t)
{
  if (ev == WEBKIT_LOAD_STARTED && t->tls_error_page) {
    /* WebKit's error page for the refused certificate: its load keeps the warning sign and the certificate */
    t->tls_error_page = FALSE;
  } else if (ev == WEBKIT_LOAD_STARTED) {
    /* a load starts: the lock back, the certificate gone */
    g_clear_object (&t->cert);
    t->tls_errors = 0;
    t->tls_failed = FALSE;
  } else if (ev == WEBKIT_LOAD_COMMITTED) {
    GTlsCertificate *cert = NULL;
    GTlsCertificateFlags errors = 0;
    if (webkit_web_view_get_tls_info (v, &cert, &errors)) {
      g_set_object (&t->cert, cert);
      t->tls_errors = errors;
    }
  } else if (ev == WEBKIT_LOAD_FINISHED && !t->incognito) {
    nb_store_visit (nb->store, webkit_web_view_get_uri (v), webkit_web_view_get_title (v));
  }
  nb_urlbar_sync (t);
  nb_urlbar_progress (t);
  nb_tab_sync_back (t);
}

static gboolean
on_tls_error (WebKitWebView *v, const char *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, NbTab *t)
{
  /* a certificate error: the warning sign; the load stops (WebKit's error page) */
  g_set_object (&t->cert, cert);
  t->tls_errors = errors;
  t->tls_failed = TRUE;
  t->tls_error_page = TRUE;
  nb_urlbar_sync (t);
  return FALSE;
}

static GtkWidget *
on_create (WebKitWebView *v, WebKitNavigationAction *action, NbTab *t)
{
  /* a popup is a new tab; without a tap only with Dynamic popups on */
  if (!webkit_navigation_action_is_user_gesture (action) && !nb_store_get_bool (nb->store, "dynamicPopups", FALSE))
    return NULL;
  NbTab *n = nb_open_tab (NULL, t->incognito, NULL, v);
  return GTK_WIDGET (n->view);
}

static void
on_close_view (WebKitWebView *v, NbTab *t)
{
  /* the page's window.close (): its tab goes */
  nb_tab_close (t);
}

static void
on_uri_launched (GObject *src, GAsyncResult *res, gpointer data)
{
  NbTab *t = data;
  if (!gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (src), res, NULL) && !t->closing)
    nb_tab_snackbar (t, _("No app can handle this link"), NULL, NULL, NULL);
}

static gboolean
on_decide (WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, NbTab *t)
{
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION (d);
    if (!webkit_response_policy_decision_is_mime_type_supported (r) &&
        webkit_response_policy_decision_is_main_frame_main_resource (r)) {
      /* a download: the user is asked first */
      WebKitURIResponse *resp = webkit_response_policy_decision_get_response (r);
      g_autofree char *url = g_strdup (webkit_uri_response_get_uri (resp));
      g_autofree char *name = g_strdup (webkit_uri_response_get_suggested_filename (resp));
      webkit_policy_decision_ignore (d);
      nb_tab_ask_download (t, url, name);
      return TRUE;
    }
    return FALSE;
  }
  WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION (d);
  WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action (nav);
  WebKitURIRequest *req = a ? webkit_navigation_action_get_request (a) : NULL;
  const char *uri = req ? webkit_uri_request_get_uri (req) : NULL;
  if (!uri || !*uri)
    return FALSE;
  const char *scheme = g_uri_peek_scheme (uri);
  static const char *const ours[] = { "http", "https", "about", "data", "blob", "file", "javascript", NULL };
  if (scheme && !g_strv_contains (ours, scheme)) {
    /* tel:, mailto:, geo: … go to their app */
    webkit_policy_decision_ignore (d);
    g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
    gtk_uri_launcher_launch (launcher, GTK_WINDOW (t->win), NULL, on_uri_launched, t);
    return TRUE;
  }
  /* Do Not Track rides on the page's own loads: a link the page follows is loaded again
   * with the header */
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && nb_store_get_bool (nb->store, "doNotTrack", FALSE) &&
      g_strcmp0 (webkit_uri_request_get_http_method (req), "POST") != 0 &&
      (g_str_equal (scheme ? scheme : "", "http") || g_str_equal (scheme ? scheme : "", "https"))) {
    SoupMessageHeaders *h = webkit_uri_request_get_http_headers (req);
    if (!h || !soup_message_headers_get_one (h, "DNT")) {
      webkit_policy_decision_ignore (d);
      WebKitURIRequest *again = webkit_uri_request_new (uri);
      soup_message_headers_append (webkit_uri_request_get_http_headers (again), "DNT", "1");
      webkit_web_view_load_request (v, again);
      g_object_unref (again);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
on_context_menu (WebKitWebView *v, WebKitContextMenu *menu, WebKitHitTestResult *hit, NbTab *t)
{
  /* a text field keeps WebKit's own menu (paste); links and images get the sheet; elsewhere nothing */
  if (webkit_hit_test_result_context_is_editable (hit) || webkit_hit_test_result_context_is_selection (hit))
    return FALSE;
  gboolean image = webkit_hit_test_result_context_is_image (hit);
  const char *url = webkit_hit_test_result_context_is_link (hit) ? webkit_hit_test_result_get_link_uri (hit)
                  : image ? webkit_hit_test_result_get_image_uri (hit) : NULL;
  if (url && *url)
    nb_link_sheet (t, url, image);
  return TRUE;
}

static guint fullscreen_cookie;

static gboolean
on_enter_fullscreen (WebKitWebView *v, NbTab *t)
{
  /* full screen: the bar goes, immersive, the screen stays on */
  t->fullscreen = TRUE;
  gtk_widget_set_visible (t->bar, FALSE);
  gtk_window_fullscreen (GTK_WINDOW (t->win));
  if (!fullscreen_cookie)
    fullscreen_cookie = gtk_application_inhibit (nb->gapp, GTK_WINDOW (t->win), GTK_APPLICATION_INHIBIT_IDLE,
                                                 _("Playing full screen"));
  nb_tab_sync_back (t);
  return FALSE;
}

static gboolean
on_leave_fullscreen (WebKitWebView *v, NbTab *t)
{
  t->fullscreen = FALSE;
  gtk_widget_set_visible (t->bar, !t->pwa_fullscreen);
  gtk_window_unfullscreen (GTK_WINDOW (t->win));
  if (fullscreen_cookie) {
    gtk_application_uninhibit (nb->gapp, fullscreen_cookie);
    fullscreen_cookie = 0;
  }
  nb_tab_sync_back (t);
  return FALSE;
}

static void
on_counted (WebKitFindController *fc, guint n, NbTab *t)
{
  nb_urlbar_find_counted (t, n, FALSE);
}

static void
on_not_found (WebKitFindController *fc, NbTab *t)
{
  nb_urlbar_find_counted (t, 0, TRUE);
}

static void
make_view (NbTab *t)
{
  WebKitSettings *settings = webkit_settings_new ();
  webkit_settings_set_enable_smooth_scrolling (settings, TRUE);
  webkit_settings_set_enable_developer_extras (settings, FALSE);
  /* sound waits for a tap; a muted video still plays by itself (autoplay, or play() from the page), as in Chrome,
   * and one the page then unmutes pauses */
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
  if (t->related)
    v = g_object_new (WEBKIT_TYPE_WEB_VIEW, "settings", settings, "user-content-manager", nb->ucm,
                      "related-view", t->related, "hexpand", TRUE, "vexpand", TRUE, NULL);
  else
    v = g_object_new (WEBKIT_TYPE_WEB_VIEW, "settings", settings, "user-content-manager", nb->ucm,
                      "network-session", nb_session_for (t->incognito), "hexpand", TRUE, "vexpand", TRUE, NULL);
  g_object_unref (settings);
  g_clear_object (&t->related);
  t->view = v;
  nb_apply_view_settings (t);
  /* the web view's base background, white: a page that paints none (plain text, WebKit's error pages, a page without
   * CSS) reads black on white; one that declares color-scheme: dark gets WebKit's dark canvas */
  GdkRGBA bg = vibeos_color (VIBEOS_WHITE);
  webkit_web_view_set_background_color (v, &bg);
  g_signal_connect (v, "notify::title", G_CALLBACK (on_title), t);
  g_signal_connect (v, "notify::uri", G_CALLBACK (on_uri), t);
  g_signal_connect (v, "notify::estimated-load-progress", G_CALLBACK (on_progress), t);
  g_signal_connect (v, "notify::is-loading", G_CALLBACK (on_progress), t);
  g_signal_connect (v, "load-changed", G_CALLBACK (on_load_changed), t);
  g_signal_connect (v, "load-failed-with-tls-errors", G_CALLBACK (on_tls_error), t);
  g_signal_connect (v, "create", G_CALLBACK (on_create), t);
  g_signal_connect (v, "close", G_CALLBACK (on_close_view), t);
  g_signal_connect (v, "decide-policy", G_CALLBACK (on_decide), t);
  g_signal_connect (v, "context-menu", G_CALLBACK (on_context_menu), t);
  g_signal_connect (v, "permission-request", G_CALLBACK (on_permission), t);
  g_signal_connect (v, "authenticate", G_CALLBACK (on_authenticate), t);
  g_signal_connect (v, "enter-fullscreen", G_CALLBACK (on_enter_fullscreen), t);
  g_signal_connect (v, "leave-fullscreen", G_CALLBACK (on_leave_fullscreen), t);
  WebKitFindController *fc = webkit_web_view_get_find_controller (v);
  g_signal_connect (fc, "counted-matches", G_CALLBACK (on_counted), t);
  g_signal_connect (fc, "failed-to-find-text", G_CALLBACK (on_not_found), t);
  gtk_overlay_set_child (GTK_OVERLAY (t->view_overlay), GTK_WIDGET (v));
  /* the page has the focus, not the URL field (see nb_tab_new); a tab behind the one shown takes nothing */
  if (!t->editing && nb_tab_is_shown (t))
    gtk_window_set_focus (GTK_WINDOW (t->win), GTK_WIDGET (v));
}

static void
load_url (NbTab *t, const char *url)
{
  WebKitURIRequest *req = webkit_uri_request_new (url);
  if (nb_store_get_bool (nb->store, "doNotTrack", FALSE))
    soup_message_headers_append (webkit_uri_request_get_http_headers (req), "DNT", "1");
  webkit_web_view_load_request (t->view, req);
  g_object_unref (req);
}

void
nb_tab_ensure_view (NbTab *t)
{
  if (t->view || t->closing)
    return;
  gboolean popup = t->related != NULL;
  make_view (t);
  if (!popup && t->url && *t->url)
    load_url (t, t->url);
  nb_urlbar_sync (t);
}

void
nb_tab_load (NbTab *t, const char *text)
{
  g_autofree char *url = nb_smart_url (text);
  if (!url)
    return;
  nb_tab_ensure_view (t);
  g_free (t->url);
  t->url = g_strdup (url);
  load_url (t, url);
  nb_urlbar_sync (t);
}

/* ---- the tab in the window ---- */

gboolean
nb_tab_is_shown (NbTab *t)
{
  return t && t == nb->last_active && !t->closing && !nb->switcher_open;
}

static void
sync_title (NbTab *t)
{
  if (t != nb->last_active)
    return;
  const char *title = nb_tab_title (t);
  gtk_window_set_title (GTK_WINDOW (t->win), *title ? title : _("Browser"));
}

static gboolean
free_tab (gpointer data)
{
  NbTab *t = data;
  /* a suggestion fetch in flight must not come back to a tab that is gone */
  if (t->suggest_cancel)
    g_cancellable_cancel (t->suggest_cancel);
  g_clear_object (&t->suggest_cancel);
  if (t->thumb_cancel)
    g_cancellable_cancel (t->thumb_cancel);
  g_clear_object (&t->thumb_cancel);
  g_clear_object (&t->thumb);
  g_clear_object (&t->cert);
  g_clear_object (&t->related);
  g_free (t->url);
  g_free (t->title);
  g_free (t->shortcut_id);
  g_free (t->find_text);
  g_free (t);
  return G_SOURCE_REMOVE;
}

static void
on_destroy (GtkWidget *holder, NbTab *t)
{
  /* the tab's widgets go now; the tab itself once they are all gone (their handlers hold it). The window going
   * takes every tab with it: none is listed any more (the session was written before) */
  g_ptr_array_remove (nb->tabs, t);
  if (nb->last_active == t)
    nb->last_active = NULL;
  if (t->suggest_cancel)
    g_cancellable_cancel (t->suggest_cancel);
  if (t->thumb_cancel)
    g_cancellable_cancel (t->thumb_cancel);
  g_idle_add (free_tab, t);
}

static void
on_top_changed (NbNavigator *nav, NbTab *t)
{
  /* the browser page back on top: its URL bar is not being edited any more */
  if (nb_navigator_get_depth (nav) != 1)
    return;
  nb_tab_sync_back (t);
  /* and the page has the focus again: the navigator's grab_focus on the page box gives it to its first focusable
   * child, the URL field, which selects its text */
  if (!t->editing && nb_tab_is_shown (t))
    gtk_window_set_focus (GTK_WINDOW (t->win), t->view ? GTK_WIDGET (t->view) : t->view_holder);
}

NbTab *
nb_tab_new (const char *url, gboolean incognito, gboolean desktop, const char *shortcut_id, WebKitWebView *related)
{
  NbTab *t = g_new0 (NbTab, 1);

  t->url = g_strdup (url);
  t->incognito = incognito;
  t->desktop = desktop;
  t->shortcut_id = g_strdup (shortcut_id);
  t->related = related ? g_object_ref (related) : NULL;
  t->pwa_fullscreen = shortcut_id && nb_store_get_bool (nb->store, "fullscreenShortcuts", FALSE);
  t->win = nb_window_get ();

  t->page = gtk_overlay_new ();
  gtk_widget_add_css_class (t->page, "nb-browser");
  t->column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_overlay_set_child (GTK_OVERLAY (t->page), t->column);
  t->bar = nb_urlbar_new (t);
  gtk_box_append (GTK_BOX (t->column), t->bar);
  gtk_widget_set_visible (t->bar, !t->pwa_fullscreen);
  t->view_holder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (t->view_holder, TRUE);
  gtk_widget_add_css_class (t->view_holder, "nb-view");
  gtk_box_append (GTK_BOX (t->column), t->view_holder);
  t->view_overlay = gtk_overlay_new ();
  gtk_widget_set_vexpand (t->view_overlay, TRUE);
  gtk_box_append (GTK_BOX (t->view_holder), t->view_overlay);
  t->refresh = nb_refresh_new (t);
  gtk_overlay_add_overlay (GTK_OVERLAY (t->view_overlay), t->refresh);
  gtk_overlay_set_clip_overlay (GTK_OVERLAY (t->view_overlay), t->refresh, TRUE);
  /* the focus starts on the page's place, not on the URL field (GTK gives a window shown without one to its first
   * focusable widget: the field, whose focus is editing, and brings the keyboard up); the view takes it once made */
  gtk_widget_set_focusable (t->view_holder, TRUE);
  gtk_overlay_add_overlay (GTK_OVERLAY (t->page), t->dropdown);


  /* The browser has no back button on screen: the window carries an invisible one, in the band the shell's back gesture
   * looks in, sensitive exactly while back has somewhere to go here */
  t->back_probe = gtk_button_new ();
  gtk_widget_add_css_class (t->back_probe, "nb-back-probe");
  gtk_widget_set_focusable (t->back_probe, FALSE);
  gtk_widget_set_can_target (t->back_probe, FALSE);
  gtk_widget_set_halign (t->back_probe, GTK_ALIGN_START);
  gtk_widget_set_valign (t->back_probe, GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (t->back_probe), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Back"), -1);
  g_signal_connect_swapped (t->back_probe, "clicked", G_CALLBACK (step_back), t);
  gtk_overlay_add_overlay (GTK_OVERLAY (t->page), t->back_probe);
  gtk_widget_set_sensitive (t->back_probe, FALSE);

  t->nav = nb_navigator_new ();
  nb_navigator_push (NB_NAVIGATOR (t->nav), t->page);
  g_signal_connect (t->nav, "top-changed", G_CALLBACK (on_top_changed), t);
  /* the tab in the window's stack: its navigator, and the snackbar over whichever of its pages shows (History's
   * "Entry deleted" is shown over History) */
  t->holder = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (t->holder), t->nav);
  t->snackbar = vibeos_snackbar_new ();
  gtk_overlay_add_overlay (GTK_OVERLAY (t->holder), t->snackbar);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (t->holder), t->snackbar, FALSE);
  g_signal_connect (t->holder, "destroy", G_CALLBACK (on_destroy), t);
  gtk_stack_add_child (GTK_STACK (nb->tab_stack), t->holder);

  g_ptr_array_add (nb->tabs, t);
  nb_tab_apply_reach (t);
  /* a popup's view is made now: WebKit needs it back from "create" */
  if (t->related)
    nb_tab_ensure_view (t);
  sync_title (t);
  nb_urlbar_sync (t);
  return t;
}

void
nb_tab_present (NbTab *t)
{
  NbTab *left = nb->last_active;

  if (t->closing)
    return;
  /* the tab left behind keeps a picture of its page for the switcher */
  if (left && left != t && !left->closing && !nb->switcher_open)
    nb_tab_take_thumb (left);
  if (left && left != t && left->menu)
    nb_menu_close (left);
  nb->last_active = t;
  /* no focus while the stacks switch: GtkStack would hand a focus it takes from the child it hides to the first
   * focusable widget of the one it shows, the URL field, which then edits (selects its text, brings the keyboard) */
  gtk_window_set_focus (GTK_WINDOW (t->win), NULL);
  gtk_stack_set_visible_child (GTK_STACK (nb->tab_stack), t->holder);
  nb_switcher_close ();
  nb_tab_ensure_view (t);
  sync_title (t);
  if (!t->editing)
    gtk_window_set_focus (GTK_WINDOW (t->win), t->view ? GTK_WIDGET (t->view) : t->view_holder);
  nb_tab_sync_back (t);
  nb_tabs_changed ();
  nb_save_session ();
  gtk_window_present (GTK_WINDOW (t->win));
}

void
nb_tab_close (NbTab *t)
{
  if (t->closing)
    return;
  gboolean shown = t == nb->last_active;
  if (t->menu)
    nb_menu_close (t);
  t->closing = TRUE;
  nb_tab_forget (t);
  if (t->view) {
    /* its page says nothing more to a tab that is gone */
    g_signal_handlers_disconnect_by_data (t->view, t);
    g_signal_handlers_disconnect_by_data (webkit_web_view_get_find_controller (t->view), t);
    webkit_web_view_terminate_web_process (t->view);
  }
  gtk_stack_remove (GTK_STACK (nb->tab_stack), t->holder);
  if (nb->switcher_open) {
    nb_tabs_changed ();
  } else if (shown && nb->last_active) {
    nb_tab_present (nb->last_active);
  } else if (!nb->tabs->len) {
    /* the last tab closed by its own page: the browser goes */
    gtk_window_close (GTK_WINDOW (nb->win));
  } else {
    nb_tabs_changed ();
  }
}
