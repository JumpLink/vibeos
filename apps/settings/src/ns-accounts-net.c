/* The checks GNOME Online Accounts' 3.58 provider dialogs make before they add an account (see ns-accounts-net.h),
 * written again from GOA's sources: goautils.c (URLs, addresses, error messages), goadavclient.c and
 * goawebdavprovider.c (WebDAV), goamailclient.c, goaimapauthlogin.c and goasmtpauth.c (IMAP, SMTP, autoconfig),
 * goaewsclient.c (Exchange) and goaowncloudprovider.c (Nextcloud). Where GOA's code does something odd the same thing
 * is done here, and said so, since what matters is that the account comes out as GOA's would.
 *
 * The WebDAV and mail checks are sequences of blocking requests, run in a worker thread with a session of their own;
 * Exchange's two requests at once and Nextcloud's requests are asynchronous, on the main context. */
#include "ns-accounts-net.h"

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

/* libsoup adds its own name after the trailing space */
#define USER_AGENT "vibeos-settings/" NS_VERSION " "
#define DAV_TIMEOUT 30
#define MAIL_TIMEOUT 30
#define AUTOCONFIG_TIMEOUT 15
#define WELL_KNOWN_CALDAV "/.well-known/caldav"
#define WELL_KNOWN_CARDDAV "/.well-known/carddav"

/* ---- goautils.c ---- */

char *
ns_goa_normalize_url (const char *base, const char *ref, char **server)
{
  const GUriFlags flags = G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED;
  int std_port;

  if (!base || !*base)
    return NULL;
  g_autofree char *text = g_str_has_suffix (base, "/") ? g_strdup (base) : g_strconcat (base, "/", NULL);
  /* dav(s) is what DNS-SD and GVfs call http(s) */
  const char *scheme = g_uri_peek_scheme (base);
  /* "nas.local:5006" is a host and its port; g_uri_peek_scheme takes "nas.local" for a scheme (and GOA refuses it as
   * one it does not know, so its form never let such a server through) */
  if (scheme && g_ascii_isdigit (base[strlen (scheme) + 1]))
    scheme = NULL;
  if (!scheme) {
    char *with = g_strconcat ("https://", text, NULL);
    g_free (text);
    text = with;
    scheme = "https";
    std_port = 443;
  } else if (g_str_equal (scheme, "https") || g_str_equal (scheme, "davs")) {
    scheme = "https";
    std_port = 443;
  } else if (g_str_equal (scheme, "http") || g_str_equal (scheme, "dav")) {
    scheme = "http";
    std_port = 80;
  } else {
    return NULL;
  }

  g_autoptr (GUri) uri = g_uri_parse (text, flags, NULL);
  if (!uri || !g_uri_get_host (uri) || !*g_uri_get_host (uri))
    return NULL;
  if (ref) {
    GUri *resolved = g_uri_parse_relative (uri, ref, flags, NULL);
    if (!resolved)
      return NULL;
    g_uri_unref (uri);
    uri = resolved;
  }
  const char *path = g_uri_get_path (uri);
  g_autofree char *slashed = g_str_has_suffix (path, "/") ? NULL : g_strconcat (path, "/", NULL);
  g_autoptr (GUri) out = g_uri_build (g_uri_get_flags (uri), scheme, g_uri_get_userinfo (uri), g_uri_get_host (uri),
                                      g_uri_get_port (uri), slashed ? slashed : path, g_uri_get_query (uri),
                                      g_uri_get_fragment (uri));
  if (server) {
    int port = g_uri_get_port (out);
    const char *out_path = g_uri_get_path (out);
    g_autofree char *pretty = g_strndup (out_path, strlen (out_path) - 1);
    g_autofree char *port_text = g_strdup_printf (":%d", port);
    *server = g_strconcat (g_uri_get_host (uri), (port == std_port || port == -1) ? "" : port_text, pretty, NULL);
  }
  return g_uri_to_string (out);
}

gboolean
ns_goa_parse_email (const char *email, char **local, char **domain)
{
  if (!email || !*email)
    return FALSE;
  const char *at = strchr (email, '@');
  if (!at || at == email || !at[1])
    return FALSE;
  if (local)
    *local = g_strndup (email, at - email);
  if (domain)
    *domain = g_strdup (at + 1);
  return TRUE;
}

/* goa_utils_set_error_ssl: one message for the certificate's error, a general one for several; it takes the place
 * of the TLS layer's own error, when there is one */
static void
set_error_ssl (GError **error, GTlsCertificateFlags flags)
{
  const char *message;
  g_clear_error (error);
  switch (flags) {
  case G_TLS_CERTIFICATE_UNKNOWN_CA:
    message = "The signing certificate authority is not known.";
    break;
  case G_TLS_CERTIFICATE_BAD_IDENTITY:
    message = "The certificate does not match the expected identity of the site that it was retrieved from.";
    break;
  case G_TLS_CERTIFICATE_NOT_ACTIVATED:
    message = "The certificate’s activation time is still in the future.";
    break;
  case G_TLS_CERTIFICATE_EXPIRED:
    message = "The certificate has expired.";
    break;
  case G_TLS_CERTIFICATE_REVOKED:
    message = "The certificate has been revoked.";
    break;
  case G_TLS_CERTIFICATE_INSECURE:
    message = "The certificate’s algorithm is considered insecure.";
    break;
  default:
    message = "Invalid certificate.";
    break;
  }
  g_clear_error (error);
  g_set_error_literal (error, GOA_ERROR, GOA_ERROR_SSL, message);
}

/* goa_utils_set_error_soup: an HTTP status that is not an answer */
static void
set_error_status (GError **error, guint status)
{
  g_clear_error (error);
  switch (status) {
  case SOUP_STATUS_METHOD_NOT_ALLOWED:
  case SOUP_STATUS_INTERNAL_SERVER_ERROR:
  case SOUP_STATUS_NOT_IMPLEMENTED:
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Not supported");
    break;
  case SOUP_STATUS_NOT_FOUND:
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Not found");
    break;
  case SOUP_STATUS_UNAUTHORIZED:
  case SOUP_STATUS_FORBIDDEN:
  case SOUP_STATUS_PROXY_AUTHENTICATION_REQUIRED:
  case SOUP_STATUS_PRECONDITION_FAILED:
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_AUTHORIZED, "Authentication failed");
    break;
  default:
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Code: %u — Unexpected response from server", status);
    break;
  }
}

/* ---- HTTP: the sign-in and the certificate check of each request ---- */

typedef struct {
  char                *user, *password;
  gboolean             accept_ssl_errors;
  GTlsCertificateFlags refused;  /* the errors of a certificate refused, 0 while none was */
} Http;

static void
http_clear (Http *h)
{
  g_free (h->user);
  g_free (h->password);
}

static gboolean
http_authenticate (SoupMessage *msg, SoupAuth *auth, gboolean retrying, gpointer data)
{
  Http *h = data;
  /* a second ask is a wrong password: the 401 is the answer */
  if (retrying || !h->user)
    return FALSE;
  soup_auth_authenticate (auth, h->user, h->password ? h->password : "");
  return TRUE;
}

static gboolean
http_accept_certificate (SoupMessage *msg, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer data)
{
  Http *h = data;
  if (h->accept_ssl_errors || errors == 0)
    return TRUE;
  if (!h->refused)
    h->refused = errors;
  return FALSE;
}

static void
http_watch (Http *h, SoupMessage *msg)
{
  g_signal_connect (msg, "authenticate", G_CALLBACK (http_authenticate), h);
  g_signal_connect (msg, "accept-certificate", G_CALLBACK (http_accept_certificate), h);
}

/* a request's own error, or GOA's certificate error when the certificate was refused */
static void
http_error (Http *h, GError **error)
{
  if (h->refused && !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    set_error_ssl (error, h->refused);
}

static SoupSession *
http_session (guint timeout)
{
  SoupSession *s = soup_session_new ();
  soup_session_set_timeout (s, timeout);
  soup_session_set_user_agent (s, USER_AGENT);
  return s;
}

/* ---- WebDAV (goadavclient.c), in a worker thread ---- */

typedef enum { DAV_WEBDAV, DAV_CALDAV, DAV_CARDDAV } DavService;

typedef struct {
  DavService service;
  char      *uri;
} DavConfig;

static DavConfig *
dav_config_new (DavService service, const char *uri)
{
  DavConfig *c = g_new0 (DavConfig, 1);
  c->service = service;
  c->uri = g_strdup (uri);
  return c;
}

static void
dav_config_free (DavConfig *c)
{
  g_free (c->uri);
  g_free (c);
}

/* the DAV header's features: 1 files, calendar-access calendars, addressbook contacts */
enum { DAV_FILES = 1, DAV_CALENDAR = 2, DAV_CONTACTS = 4 };

static guint
dav_features (const char *header)
{
  if (!header || !*header)
    return 0;
  g_auto (GStrv) parts = g_strsplit_set (header, ", ", -1);
  guint f = 0;
  if (g_strv_contains ((const char *const *) parts, "1"))
    f |= DAV_FILES;
  if (g_strv_contains ((const char *const *) parts, "calendar-access"))
    f |= DAV_CALENDAR;
  if (g_strv_contains ((const char *const *) parts, "addressbook"))
    f |= DAV_CONTACTS;
  return f;
}

typedef struct {
  SoupSession  *session;
  Http          http;
  GCancellable *cancellable;
} Dav;

typedef struct {
  guint  status;
  char  *final_uri;  /* where the request ended, after redirects */
  char  *dav;        /* the DAV header */
  char  *path;       /* final_uri's path */
} DavAnswer;

static void
dav_answer_clear (DavAnswer *a)
{
  g_free (a->final_uri);
  g_free (a->dav);
  g_free (a->path);
  memset (a, 0, sizeof *a);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (DavAnswer, dav_answer_clear)

/* an OPTIONS request with the account's sign-in: FALSE with `error` when no answer came */
static gboolean
dav_options (Dav *d, const char *uri, DavAnswer *a, GError **error)
{
  dav_answer_clear (a);
  g_autoptr (SoupMessage) msg = soup_message_new (SOUP_METHOD_OPTIONS, uri);
  if (!msg) {
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", uri);
    return FALSE;
  }
  http_watch (&d->http, msg);
  g_autoptr (GBytes) body = soup_session_send_and_read (d->session, msg, d->cancellable, error);
  if (!body) {
    http_error (&d->http, error);
    return FALSE;
  }
  GUri *final = soup_message_get_uri (msg);
  a->status = soup_message_get_status (msg);
  a->final_uri = g_uri_to_string (final);
  a->path = g_strdup (g_uri_get_path (final));
  a->dav = g_strdup (soup_message_headers_get_list (soup_message_get_response_headers (msg), "DAV"));
  return TRUE;
}

static gboolean
status_is_answer (guint status)
{
  return status == SOUP_STATUS_OK || status == SOUP_STATUS_NON_AUTHORITATIVE || status == SOUP_STATUS_NO_CONTENT ||
         status == SOUP_STATUS_MULTI_STATUS;
}

static gboolean
status_is_fallback (guint status)
{
  return status == SOUP_STATUS_NOT_FOUND || status == SOUP_STATUS_METHOD_NOT_ALLOWED ||
         status == SOUP_STATUS_INTERNAL_SERVER_ERROR || status == SOUP_STATUS_NOT_IMPLEMENTED;
}

static char *
well_known (DavService service, const char *uri)
{
  if (service == DAV_CALDAV)
    return g_uri_resolve_relative (uri, WELL_KNOWN_CALDAV, G_URI_FLAGS_NONE, NULL);
  if (service == DAV_CARDDAV)
    return g_uri_resolve_relative (uri, WELL_KNOWN_CARDDAV, G_URI_FLAGS_NONE, NULL);
  return NULL;
}

/* goa_dav_client_check. As in GOA, any DAV feature passes for any service, and a 404 (405, 500, 501) that the
 * well-known address answers the same way is not an error. */
static gboolean
dav_check_sync (Dav *d, DavService service, const char *uri, GError **error)
{
  g_auto (DavAnswer) a = { 0 };
  g_autofree char *current = g_strdup (uri);
  gboolean fell_back = FALSE;
  g_autoptr (GError) failed = NULL;

  for (;;) {
    if (!dav_options (d, current, &a, error))
      return FALSE;
    g_clear_error (&failed);
    if (status_is_answer (a.status)) {
      if (dav_features (a.dav))
        return TRUE;
      g_set_error_literal (&failed, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "DAV not supported");
    } else if (!status_is_fallback (a.status)) {
      set_error_status (error, a.status);
      return FALSE;
    }
    g_autofree char *next = fell_back ? NULL : well_known (service, uri);
    if (!next)
      break;
    fell_back = TRUE;
    g_free (current);
    current = g_steal_pointer (&next);
  }
  if (failed) {
    g_propagate_error (error, g_steal_pointer (&failed));
    return FALSE;
  }
  return TRUE;
}

/* a URI built from `uri`'s parts with another path: the port left out when it is the scheme's own */
static char *
uri_with_path (GUri *uri, const char *path)
{
  int port = g_uri_get_port (uri);
  const char *scheme = g_uri_get_scheme (uri);
  if ((g_strcmp0 (scheme, "https") == 0 && port == 443) || (g_strcmp0 (scheme, "http") == 0 && port == 80))
    port = -1;
  return g_uri_join_with_user (G_URI_FLAGS_PARSE_RELAXED, scheme, g_uri_get_user (uri), g_uri_get_password (uri),
                               g_uri_get_auth_params (uri), g_uri_get_host (uri), port, path, g_uri_get_query (uri),
                               g_uri_get_fragment (uri));
}

/* dav_client_discover_postconfig_nexcloud: a Nextcloud (ownCloud) server's own addresses, from where the answer came
 * from or its DAV header */
static gboolean
dav_nextcloud (const DavAnswer *a, GPtrArray *services)
{
  const char *root = g_strrstr (a->path, "/remote.php/dav");
  if (!root)
    root = g_strrstr (a->path, "/remote.php/webdav");
  if (!root && a->dav && g_strrstr (a->dav, "nextcloud-checksum-update")) {
    root = g_strrstr (a->path, WELL_KNOWN_CALDAV);
    if (!root)
      root = g_strrstr (a->path, WELL_KNOWN_CARDDAV);
  }
  if (!root)
    return FALSE;
  g_autoptr (GUri) uri = g_uri_parse (a->final_uri, SOUP_HTTP_URI_FLAGS, NULL);
  if (!uri)
    return FALSE;
  g_autofree char *base = g_strndup (a->path, root - a->path);
  g_autofree char *dav_path = g_build_path ("/", base, "/remote.php/dav", NULL);
  g_autofree char *webdav_path = g_build_path ("/", base, "/remote.php/webdav", NULL);
  g_autofree char *webdav = uri_with_path (uri, webdav_path);
  g_autofree char *dav = uri_with_path (uri, dav_path);
  g_ptr_array_add (services, dav_config_new (DAV_WEBDAV, webdav));
  g_ptr_array_add (services, dav_config_new (DAV_CALDAV, dav));
  g_ptr_array_add (services, dav_config_new (DAV_CARDDAV, dav));
  return TRUE;
}

/* dav_client_discover_postconfig_sogo: a SOGo server's address for the user */
static gboolean
dav_sogo (const DavAnswer *a, const char *user, GPtrArray *services)
{
  const char *root = g_strrstr (a->path, "/SOGo/dav");
  if (!root)
    return FALSE;
  g_autofree char *suffix = g_strdup_printf ("/%s/", user);
  if (g_str_has_suffix (a->path, suffix) || g_str_has_suffix (a->path, user))
    return FALSE;
  g_autoptr (GUri) uri = g_uri_parse (a->final_uri, SOUP_HTTP_URI_FLAGS, NULL);
  if (!uri)
    return FALSE;
  g_autofree char *base = g_strndup (a->path, root - a->path);
  g_autofree char *path = g_build_path ("/", base, "/SOGo/dav", user, "/", NULL);
  g_autofree char *dav = uri_with_path (uri, path);
  g_ptr_array_add (services, dav_config_new (DAV_CALDAV, dav));
  g_ptr_array_add (services, dav_config_new (DAV_CARDDAV, dav));
  return TRUE;
}

/* goa_dav_client_srv_lookup: `service`'s SRV record for `domain` (or its base domain) as a URI with the well-known
 * path, or NULL. (GOA asks for the TXT record's "path" too but with its key unset, so it never finds one: the
 * well-known path is what it uses, and what is used here.) */
static char *
dav_srv (const char *service, const char *domain, GCancellable *cancellable)
{
  g_autoptr (GResolver) resolver = g_resolver_get_default ();
  g_autofree char *folded = g_utf8_casefold (domain, -1);
  const char *base = soup_tld_get_base_domain (folded, NULL);
  const char *domains[] = { domain, base && g_strcmp0 (base, folded) != 0 ? base : NULL };

  for (guint i = 0; i < G_N_ELEMENTS (domains) && domains[i]; i++) {
    GList *targets = g_resolver_lookup_service (resolver, service, "tcp", domains[i], cancellable, NULL);
    if (!targets)
      continue;
    targets = g_srv_target_list_sort (targets);
    const char *host = g_srv_target_get_hostname (targets->data);
    char *uri = NULL;
    if (host && *host) {
      const char *scheme = g_str_has_suffix (service, "s") ? "https" : "http";
      const char *path = g_str_has_prefix (service, "caldav") ? WELL_KNOWN_CALDAV : WELL_KNOWN_CARDDAV;
      g_autoptr (GUri) u = g_uri_build (SOUP_HTTP_URI_FLAGS, scheme, NULL, host,
                                        g_srv_target_get_port (targets->data), path, NULL, NULL);
      uri = u ? g_uri_to_string (u) : NULL;
    }
    g_resolver_free_targets (targets);
    return uri;
  }
  return NULL;
}

/* dav_client_discover_preconfig: the providers GOA knows the addresses of */
static gboolean
dav_preconfig (GUri *uri, GQueue *candidates)
{
  static const struct {
    const char *host, *caldav, *carddav, *webdav;
  } known[] = {
    { "fastmail.com", "https://caldav.fastmail.com/.well-known/caldav",
      "https://carddav.fastmail.com/.well-known/carddav", "https://myfiles.fastmail.com" },
    { "mailbox.org", "https://dav.mailbox.org/caldav", "https://dav.mailbox.org/carddav",
      "https://dav.mailbox.org/servlet/webdav.infostore/" },
    { "mail.ru", "https://calendar.mail.ru", NULL, "https://webdav.cloud.mail.ru" },
  };
  if (!g_uri_get_host (uri))
    return FALSE;
  g_autofree char *host = g_utf8_casefold (g_uri_get_host (uri), -1);
  const char *base = soup_tld_get_base_domain (host, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (known); i++) {
    if (g_strcmp0 (host, known[i].host) != 0 && g_strcmp0 (base, known[i].host) != 0)
      continue;
    g_queue_push_tail (candidates, dav_config_new (DAV_CALDAV, known[i].caldav));
    if (known[i].carddav)
      g_queue_push_tail (candidates, dav_config_new (DAV_CARDDAV, known[i].carddav));
    g_queue_push_tail (candidates, dav_config_new (DAV_WEBDAV, known[i].webdav));
    return TRUE;
  }
  return FALSE;
}

static gboolean
service_matches (DavService service, guint features)
{
  return (service == DAV_CALDAV && (features & DAV_CALENDAR)) ||
         (service == DAV_CARDDAV && (features & DAV_CONTACTS)) || (service == DAV_WEBDAV && (features & DAV_FILES));
}

/* goa_dav_client_discover: the services found from `uri`, in the order found (GOA's sort compares each entry with
 * itself and so keeps that order), or NULL with `error`. Only a certificate error or a cancel stops it early; an
 * address that does not answer is passed over. */
static GPtrArray *
dav_discover_sync (Dav *d, const char *uri_text, GError **error)
{
  g_autoptr (GUri) uri = g_uri_parse (uri_text, SOUP_HTTP_URI_FLAGS, NULL);
  if (!uri) {
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", uri_text);
    return NULL;
  }
  g_autofree char *base = g_uri_to_string (uri);
  g_autoptr (GPtrArray) services = g_ptr_array_new_with_free_func ((GDestroyNotify) dav_config_free);
  GQueue candidates = G_QUEUE_INIT;
  gboolean auth_error = FALSE, uri_fallback = FALSE;

  if (!dav_preconfig (uri, &candidates)) {
    static const char *const srv[] = { "caldavs", "caldav", "carddavs", "carddav" };
    for (guint i = 0; i < G_N_ELEMENTS (srv) && g_uri_get_host (uri); i++) {
      g_autofree char *found = dav_srv (srv[i], g_uri_get_host (uri), d->cancellable);
      if (found)
        g_queue_push_tail (&candidates, dav_config_new (i < 2 ? DAV_CALDAV : DAV_CARDDAV, found));
    }
    if (g_cancellable_set_error_if_cancelled (d->cancellable, error)) {
      g_queue_clear_full (&candidates, (GDestroyNotify) dav_config_free);
      return NULL;
    }
  }

  for (;;) {
    DavConfig *c = g_queue_pop_head (&candidates);
    if (!c) {
      if (services->len > 0)
        break;
      if (!uri_fallback) {
        /* nothing found: the address itself as each service, and Nextcloud's own path */
        g_autofree char *nc = g_uri_resolve_relative (base, "remote.php/dav", G_URI_FLAGS_NONE, NULL);
        g_queue_push_tail (&candidates, dav_config_new (DAV_CALDAV, base));
        g_queue_push_tail (&candidates, dav_config_new (DAV_CARDDAV, base));
        g_queue_push_tail (&candidates, dav_config_new (DAV_WEBDAV, base));
        if (nc)
          g_queue_push_tail (&candidates, dav_config_new (DAV_WEBDAV, nc));
        uri_fallback = TRUE;
        continue;
      }
      if (auth_error)
        g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_AUTHORIZED, "Authentication failed");
      else
        g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "Cannot find WebDAV endpoint");
      return NULL;
    }

    g_autofree char *current = g_strdup (c->uri);
    gboolean fell_back = FALSE, taken = FALSE, stop = FALSE;
    while (!taken && !stop) {
      g_auto (DavAnswer) a = { 0 };
      g_autoptr (GError) e = NULL;
      gboolean fallback = FALSE;
      if (!dav_options (d, current, &a, &e)) {
        if (g_error_matches (e, GOA_ERROR, GOA_ERROR_SSL) || g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          g_propagate_error (error, g_steal_pointer (&e));
          dav_config_free (c);
          g_queue_clear_full (&candidates, (GDestroyNotify) dav_config_free);
          return NULL;
        }
        break;
      }
      if (status_is_answer (a.status)) {
        if (dav_nextcloud (&a, services) || dav_sogo (&a, d->http.user, services)) {
          g_queue_clear_full (&candidates, (GDestroyNotify) dav_config_free);
          break;
        }
        guint features = dav_features (a.dav);
        if (!features)
          break;
        if (service_matches (c->service, features)) {
          /* GVfs does not follow redirects: files get the address the answer came from. The others keep their
           * own, even when it was their well-known address that answered. */
          if (c->service == DAV_WEBDAV) {
            g_free (c->uri);
            c->uri = g_strdup (a.final_uri);
          }
          g_ptr_array_add (services, g_steal_pointer (&c));
          taken = TRUE;
          break;
        }
        fallback = TRUE;
      } else if (status_is_fallback (a.status)) {
        fallback = TRUE;
      } else if (a.status == SOUP_STATUS_UNAUTHORIZED || a.status == SOUP_STATUS_FORBIDDEN ||
                 a.status == SOUP_STATUS_PRECONDITION_FAILED) {
        /* passed over, and reported only when nothing is found: a password may be good for some services only */
        auth_error = TRUE;
        break;
      } else {
        break;
      }
      if (fallback && !fell_back) {
        g_autofree char *next = well_known (c->service, c->uri);
        if (next && g_strcmp0 (next, c->uri) != 0) {
          fell_back = TRUE;
          g_free (current);
          current = g_steal_pointer (&next);
          continue;
        }
      }
      stop = TRUE;
    }
    if (c)
      dav_config_free (c);
  }
  g_queue_clear_full (&candidates, (GDestroyNotify) dav_config_free);
  return g_steal_pointer (&services);
}

static const char *
dav_first (GPtrArray *services, DavService service)
{
  for (guint i = 0; services && i < services->len; i++) {
    DavConfig *c = g_ptr_array_index (services, i);
    if (c->service == service)
      return c->uri;
  }
  return NULL;
}

void
ns_dav_found_free (NsDavFound *found)
{
  if (!found)
    return;
  g_free (found->webdav);
  g_free (found->caldav);
  g_free (found->carddav);
  g_free (found);
}

typedef struct {
  char    *uri, *files, *caldav, *carddav, *service;
  Http     http;
} DavJob;

static void
dav_job_free (DavJob *j)
{
  g_free (j->uri);
  g_free (j->files);
  g_free (j->caldav);
  g_free (j->carddav);
  g_free (j->service);
  http_clear (&j->http);
  g_free (j);
}

static void
dav_setup (Dav *d, DavJob *j, GCancellable *cancellable)
{
  d->session = http_session (DAV_TIMEOUT);
  d->http = j->http;
  d->cancellable = cancellable;
}

/* add_account_handle_response's stages: the discovery, then the addresses the user typed, each normalized against
 * the server address as GOA's dialog does */
static void
dav_add_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  DavJob *j = data;
  Dav d;
  GError *error = NULL;
  dav_setup (&d, j, cancellable);
  g_autoptr (NsDavFound) found = g_new0 (NsDavFound, 1);

  g_autoptr (GPtrArray) services = dav_discover_sync (&d, j->uri, &error);
  if (!services)
    goto out;
  found->webdav = g_strdup (dav_first (services, DAV_WEBDAV));
  found->caldav = g_strdup (dav_first (services, DAV_CALDAV));
  found->carddav = g_strdup (dav_first (services, DAV_CARDDAV));

  const char *typed[] = { j->files, j->caldav, j->carddav };
  for (guint i = 0; i < G_N_ELEMENTS (typed); i++) {
    if (!typed[i] || !*typed[i])
      continue;
    g_autofree char *uri = ns_goa_normalize_url (j->uri, typed[i], NULL);
    if (!uri) {
      error = g_error_new (GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", typed[i]);
      goto out;
    }
    if (i == 0) {
      /* files: a discovery of its own, for the address it redirects to; what it finds may be nothing */
      g_autoptr (GPtrArray) own = dav_discover_sync (&d, uri, &error);
      if (!own)
        goto out;
      g_free (found->webdav);
      found->webdav = g_strdup (dav_first (own, DAV_WEBDAV));
    } else {
      /* calendars, contacts: checked, then taken even when the server does not say it has them */
      if (!dav_check_sync (&d, i == 1 ? DAV_CALDAV : DAV_CARDDAV, uri, &error))
        goto out;
      char **slot = i == 1 ? &found->caldav : &found->carddav;
      g_free (*slot);
      *slot = g_steal_pointer (&uri);
    }
  }

out:
  g_object_unref (d.session);
  /* the strings are the job's */
  if (error) {
    g_task_return_error (task, error);
  } else {
    g_task_return_pointer (task, g_steal_pointer (&found), (GDestroyNotify) ns_dav_found_free);
  }
}

static DavJob *
dav_job_new (const char *user, const char *password, gboolean accept_ssl_errors)
{
  DavJob *j = g_new0 (DavJob, 1);
  j->http.user = g_strdup (user);
  j->http.password = g_strdup (password);
  j->http.accept_ssl_errors = accept_ssl_errors;
  return j;
}

void
ns_dav_add_check (const char *uri, const char *user, const char *password, const char *files, const char *caldav,
                  const char *carddav, gboolean accept_ssl_errors, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  DavJob *j = dav_job_new (user, password, accept_ssl_errors);
  j->uri = g_strdup (uri);
  j->files = g_strdup (files);
  j->caldav = g_strdup (caldav);
  j->carddav = g_strdup (carddav);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, j, (GDestroyNotify) dav_job_free);
  g_task_run_in_thread (task, dav_add_thread);
}

NsDavFound *
ns_dav_add_check_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
dav_check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  DavJob *j = data;
  Dav d;
  GError *error = NULL;
  dav_setup (&d, j, cancellable);
  DavService service = g_str_equal (j->service, "caldav")    ? DAV_CALDAV
                       : g_str_equal (j->service, "carddav") ? DAV_CARDDAV
                                                             : DAV_WEBDAV;
  gboolean ok = dav_check_sync (&d, service, j->uri, &error);
  g_object_unref (d.session);
  if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

void
ns_dav_check (const char *service, const char *uri, const char *user, const char *password,
              gboolean accept_ssl_errors, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  DavJob *j = dav_job_new (user, password, accept_ssl_errors);
  j->uri = g_strdup (uri);
  j->service = g_strdup (service);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, j, (GDestroyNotify) dav_job_free);
  g_task_run_in_thread (task, dav_check_thread);
}

gboolean
ns_dav_check_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---- mail: a line protocol over TCP and TLS, in a worker thread ---- */

typedef struct {
  GSocketConnection   *socket;
  GIOStream           *stream;   /* the socket's, or the TLS connection over it */
  GDataInputStream    *in;
  GOutputStream       *out;
  GCancellable        *cancellable;
  GSocketConnectable  *identity; /* the server, for the certificate */
  gboolean             accept_ssl_errors;
  GTlsCertificateFlags refused;
} Line;

static void
line_close (Line *l)
{
  g_clear_object (&l->in);
  g_clear_object (&l->stream);
  g_clear_object (&l->socket);
  g_clear_object (&l->identity);
}

static gboolean
line_accept_certificate (GTlsConnection *conn, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer data)
{
  Line *l = data;
  if (l->accept_ssl_errors)
    return TRUE;
  l->refused = errors;
  return FALSE;
}

/* the streams over `stream` */
static void
line_use (Line *l, GIOStream *stream)
{
  g_clear_object (&l->in);
  if (l->stream != stream) {
    g_clear_object (&l->stream);
    l->stream = g_object_ref (stream);
  }
  l->in = g_data_input_stream_new (g_io_stream_get_input_stream (stream));
  g_data_input_stream_set_newline_type (l->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (l->in), FALSE);
  l->out = g_io_stream_get_output_stream (stream);
}

/* TLS over the connection as it is now; a refused certificate is GOA's certificate error */
static gboolean
line_tls (Line *l, GError **error)
{
  g_autoptr (GIOStream) tls = g_tls_client_connection_new (G_IO_STREAM (l->socket), l->identity, error);
  if (!tls)
    return FALSE;
  g_signal_connect (tls, "accept-certificate", G_CALLBACK (line_accept_certificate), l);
  if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls), l->cancellable, error)) {
    if (l->refused && !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      set_error_ssl (error, l->refused);
    return FALSE;
  }
  line_use (l, tls);
  return TRUE;
}

static gboolean
line_open (Line *l, const char *host, guint16 port, gboolean ssl, GError **error)
{
  l->identity = g_network_address_parse (host, port, error);
  if (!l->identity)
    return FALSE;
  g_autoptr (GSocketClient) client = g_socket_client_new ();
  g_socket_client_set_timeout (client, MAIL_TIMEOUT);
  l->socket = g_socket_client_connect (client, l->identity, l->cancellable, error);
  if (!l->socket)
    return FALSE;
  line_use (l, G_IO_STREAM (l->socket));
  return ssl ? line_tls (l, error) : TRUE;
}

/* a line without its CR LF (goa_utils_data_input_stream_read_line) */
static char *
line_read (Line *l, GError **error)
{
  g_autoptr (GError) e = NULL;
  char *s = g_data_input_stream_read_line (l->in, NULL, l->cancellable, &e);
  if (!s) {
    if (e)
      g_propagate_error (error, g_steal_pointer (&e));
    else
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Connection closed");
    return NULL;
  }
  g_strchomp (s);
  return s;
}

static gboolean
line_send (Line *l, const char *text, GError **error)
{
  return g_output_stream_write_all (l->out, text, strlen (text), NULL, l->cancellable, error);
}

/* ---- IMAP (goaimapauthlogin.c) ---- */

#define IMAP_TAG "A001"

static gboolean
imap_bye (const char *r, GError **error)
{
  if (!g_str_has_prefix (r, "* BYE"))
    return FALSE;
  g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Service not available");
  return TRUE;
}

static gboolean
imap_not_ok (const char *r, gboolean tagged, GError **error)
{
  if (g_str_has_prefix (r, tagged ? IMAP_TAG " OK" : "* OK"))
    return FALSE;
  g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Unexpected response `%s' while doing LOGIN authentication", r);
  return TRUE;
}

/* the lines up to the tagged answer, which must be OK */
static gboolean
imap_read_response (Line *l, GError **error)
{
  for (;;) {
    g_autofree char *r = line_read (l, error);
    if (!r)
      return FALSE;
    if (g_str_has_prefix (r, IMAP_TAG)) {
      if (g_str_has_prefix (r, IMAP_TAG " NO")) {
        g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Authentication failed");
        return FALSE;
      }
      return !imap_not_ok (r, TRUE, error);
    }
    if (g_str_has_prefix (r, "* ")) {
      if (imap_bye (r, error))
        return FALSE;
      continue;
    }
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Unexpected response `%s' while doing LOGIN authentication", r);
    return FALSE;
  }
}

static char *
imap_quote (const char *s)
{
  GString *q = g_string_new (NULL);
  for (; s && *s; s++) {
    if (*s == '\\' || *s == '"')
      g_string_append_c (q, '\\');
    g_string_append_c (q, *s);
  }
  return g_string_free (q, FALSE);
}

static gboolean
imap_check (Line *l, NsTls tls, const char *user, const char *password, GError **error)
{
  /* the greeting */
  g_autofree char *r = line_read (l, error);
  if (!r || imap_bye (r, error) || imap_not_ok (r, FALSE, error))
    return FALSE;
  if (tls == NS_TLS_STARTTLS) {
    if (!line_send (l, IMAP_TAG " CAPABILITY\r\n", error))
      return FALSE;
    g_autofree char *caps = line_read (l, error);
    if (!caps)
      return FALSE;
    if (!strstr (caps, "STARTTLS")) {
      g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "Server does not support STARTTLS");
      return FALSE;
    }
    if (!imap_read_response (l, error) || !line_send (l, IMAP_TAG " STARTTLS\r\n", error) ||
        !imap_read_response (l, error) || !line_tls (l, error))
      return FALSE;
  }
  g_autofree char *u = imap_quote (user);
  g_autofree char *p = imap_quote (password);
  g_autofree char *login = g_strdup_printf (IMAP_TAG " LOGIN \"%s\" \"%s\"\r\n", u, p);
  return line_send (l, login, error) && imap_read_response (l, error);
}

/* ---- SMTP (goasmtpauth.c) ---- */

static gboolean
smtp_unexpected (const char *r, const char *code, GError **error)
{
  if (g_str_has_prefix (r, "421")) {
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Service not available");
    return TRUE;
  }
  if (g_str_has_prefix (r, code) && strlen (r) >= 4)
    return FALSE;
  g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Unexpected response `%s'", r);
  return TRUE;
}

static gboolean
smtp_greeting (Line *l, GError **error)
{
  for (;;) {
    g_autofree char *r = line_read (l, error);
    if (!r || smtp_unexpected (r, "220", error))
      return FALSE;
    if (r[3] != '-')
      return TRUE;
  }
}

/* EHLO: whether STARTTLS is offered, and the AUTH line's mechanisms ("PLAIN" counts when both are listed) */
static gboolean
smtp_ehlo (Line *l, const char *domain, gboolean *starttls, NsSmtpAuth *auth, GError **error)
{
  g_autofree char *ehlo = g_strdup_printf ("EHLO %s\r\n", domain ? domain : "");
  if (!line_send (l, ehlo, error))
    return FALSE;
  for (;;) {
    g_autofree char *r = line_read (l, error);
    if (!r || smtp_unexpected (r, "250", error))
      return FALSE;
    if (starttls && g_str_has_prefix (r + 4, "STARTTLS"))
      *starttls = TRUE;
    if (auth && g_str_has_prefix (r + 4, "AUTH")) {
      auth->use_auth = TRUE;
      if (strstr (r, "PLAIN"))
        auth->auth_plain = TRUE;
      else if (strstr (r, "LOGIN"))
        auth->auth_login = TRUE;
    }
    if (r[3] != '-')
      return TRUE;
  }
}

static gboolean
smtp_check (Line *l, NsTls tls, const char *user, const char *password, const char *domain, NsSmtpAuth *auth,
            GError **error)
{
  if (!smtp_greeting (l, error))
    return FALSE;
  if (tls == NS_TLS_STARTTLS) {
    gboolean starttls = FALSE;
    if (!smtp_ehlo (l, domain, &starttls, NULL, error))
      return FALSE;
    if (!starttls) {
      g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "Server does not support STARTTLS");
      return FALSE;
    }
    if (!line_send (l, "STARTTLS\r\n", error))
      return FALSE;
    g_autofree char *r = line_read (l, error);
    if (!r)
      return FALSE;
    if (g_str_has_prefix (r, "454")) {
      g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "TLS not available");
      return FALSE;
    }
    if (smtp_unexpected (r, "220", error) || !line_tls (l, error))
      return FALSE;
  }
  /* after STARTTLS there is no greeting */
  if (!smtp_ehlo (l, domain, NULL, auth, error))
    return FALSE;
  if (!auth->use_auth)
    return TRUE;
  if (!auth->auth_login && !auth->auth_plain) {
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "Unknown authentication mechanism");
    return FALSE;
  }
  g_autofree char *request = NULL;
  if (auth->auth_plain) {
    /* the user twice: as the identity to act as and as the one signing in */
    gsize ul = strlen (user), pl = strlen (password);
    g_autofree guchar *arg = g_malloc (2 * ul + pl + 2);
    memcpy (arg, user, ul);
    arg[ul] = '\0';
    memcpy (arg + ul + 1, user, ul);
    arg[2 * ul + 1] = '\0';
    memcpy (arg + 2 * ul + 2, password, pl);
    g_autofree char *b64 = g_base64_encode (arg, 2 * ul + pl + 2);
    request = g_strdup_printf ("AUTH PLAIN %s\r\n", b64);
    if (!line_send (l, request, error))
      return FALSE;
  } else {
    g_autofree char *b64 = g_base64_encode ((const guchar *) user, strlen (user));
    request = g_strdup_printf ("AUTH LOGIN %s\r\n", b64);
    if (!line_send (l, request, error))
      return FALSE;
    g_autofree char *r = line_read (l, error);
    if (!r)
      return FALSE;
    if (!g_str_has_prefix (r, "334 UGFzc3dvcmQ6")) {
      g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Unexpected response `%s'", r);
      return FALSE;
    }
    g_autofree char *pb64 = g_base64_encode ((const guchar *) password, strlen (password));
    g_autofree char *line = g_strdup_printf ("%s\r\n", pb64);
    if (!line_send (l, line, error))
      return FALSE;
  }
  g_autofree char *r = line_read (l, error);
  if (!r)
    return FALSE;
  if (!g_str_has_prefix (r, "235")) {
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Authentication failed");
    return FALSE;
  }
  return TRUE;
}

typedef struct {
  gboolean   smtp;
  char      *host, *user, *password, *domain;
  NsTls      tls;
  gboolean   accept_ssl_errors;
  NsSmtpAuth auth;
} MailJob;

static void
mail_job_free (MailJob *j)
{
  g_free (j->host);
  g_free (j->user);
  g_free (j->password);
  g_free (j->domain);
  g_free (j);
}

static void
mail_check_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  MailJob *j = data;
  Line l = { .cancellable = cancellable, .accept_ssl_errors = j->accept_ssl_errors };
  GError *error = NULL;
  guint16 port = j->smtp ? (j->tls == NS_TLS_SSL ? 465 : 587) : (j->tls == NS_TLS_SSL ? 993 : 143);
  gboolean ok = line_open (&l, j->host, port, j->tls == NS_TLS_SSL, &error) &&
                (j->smtp ? smtp_check (&l, j->tls, j->user, j->password, j->domain, &j->auth, &error)
                         : imap_check (&l, j->tls, j->user, j->password, &error));
  line_close (&l);
  if (ok)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

void
ns_mail_check (gboolean smtp, const char *host, NsTls tls, gboolean accept_ssl_errors, const char *user,
               const char *password, const char *domain, GCancellable *cancellable, GAsyncReadyCallback callback,
               gpointer data)
{
  MailJob *j = g_new0 (MailJob, 1);
  j->smtp = smtp;
  j->host = g_strdup (host);
  j->user = g_strdup (user ? user : "");
  j->password = g_strdup (password ? password : "");
  j->domain = g_strdup (domain);
  j->tls = tls;
  j->accept_ssl_errors = accept_ssl_errors;
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, j, (GDestroyNotify) mail_job_free);
  g_task_run_in_thread (task, mail_check_thread);
}

gboolean
ns_mail_check_finish (GAsyncResult *result, NsSmtpAuth *smtp_auth, GError **error)
{
  MailJob *j = g_task_get_task_data (G_TASK (result));
  gboolean ok = g_task_propagate_boolean (G_TASK (result), error);
  if (ok && smtp_auth)
    *smtp_auth = j->auth;
  return ok;
}

/* ---- the mail autoconfig lookup (goamailclient.c), in a worker thread ---- */

void
ns_mail_found_free (NsMailFound *found)
{
  if (!found)
    return;
  g_free (found->imap_host);
  g_free (found->imap_user);
  g_free (found->smtp_host);
  g_free (found->smtp_user);
  g_free (found);
}

typedef struct {
  const char  *email, *local, *domain;
  NsMailFound *found;
  GPtrArray   *path;       /* the open elements */
  int          server;     /* 1 an IMAP incomingServer, 2 an SMTP outgoingServer, -1 another server, 0 none */
  gboolean     has_imap, has_smtp;
  GString     *text;
  char        *hostname, *username, *socket_type;
} Autoconfig;

/* an element's name without its namespace prefix */
static const char *
local_name (const char *name)
{
  const char *colon = strrchr (name, ':');
  return colon ? colon + 1 : name;
}

static void
ac_start (GMarkupParseContext *ctx, const char *name, const char **attrs, const char **values, gpointer data,
          GError **error)
{
  Autoconfig *ac = data;
  name = local_name (name);
  guint depth = ac->path->len;
  if (depth == 0 && !g_str_equal (name, "clientConfig")) {
    g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "Failed to parse Autoconfig XML");
    return;
  }
  if (depth == 2 && g_str_equal (g_ptr_array_index (ac->path, 1), "emailProvider") &&
      (g_str_equal (name, "incomingServer") || g_str_equal (name, "outgoingServer"))) {
    const char *type = NULL;
    for (guint i = 0; attrs[i]; i++)
      if (g_str_equal (attrs[i], "type"))
        type = values[i];
    if (g_str_equal (name, "incomingServer") && g_strcmp0 (type, "imap") == 0 && !ac->has_imap)
      ac->server = 1;
    else if (g_str_equal (name, "outgoingServer") && g_strcmp0 (type, "smtp") == 0 && !ac->has_smtp)
      ac->server = 2;
    else
      ac->server = -1;
  }
  g_string_truncate (ac->text, 0);
  g_ptr_array_add (ac->path, g_strdup (name));
}

static void
ac_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Autoconfig *ac = data;
  g_string_append_len (ac->text, text, len);
}

/* the user name's placeholders */
static char *
ac_username (Autoconfig *ac, const char *value)
{
  if (g_ascii_strcasecmp (value, "%EMAILADDRESS%") == 0)
    return g_strdup (ac->email);
  if (g_ascii_strcasecmp (value, "%EMAILLOCALPART%") == 0)
    return g_strdup (ac->local);
  if (g_ascii_strcasecmp (value, "%EMAILDOMAIN%") == 0)
    return g_strdup (ac->domain);
  /* GOA leaves any other user name out */
  return NULL;
}

static void
ac_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Autoconfig *ac = data;
  name = local_name (name);
  guint depth = ac->path->len;
  if (ac->server > 0 && depth == 4) {
    g_autofree char *value = g_strstrip (g_strdup (ac->text->str));
    if (g_str_equal (name, "hostname"))
      g_set_str (&ac->hostname, value);
    else if (g_str_equal (name, "username"))
      g_set_str (&ac->username, value);
    else if (g_str_equal (name, "socketType"))
      g_set_str (&ac->socket_type, value);
  }
  if (ac->server != 0 && depth == 3) {
    if (ac->server > 0) {
      /* "plain" is shown as SSL, as GOA's dialog shows it */
      NsTls tls = ac->socket_type && g_ascii_strcasecmp (ac->socket_type, "STARTTLS") == 0 ? NS_TLS_STARTTLS
                                                                                            : NS_TLS_SSL;
      char *user = ac->username ? ac_username (ac, ac->username) : NULL;
      if (ac->server == 1) {
        ac->found->imap_host = g_strdup (ac->hostname);
        ac->found->imap_user = user;
        ac->found->imap_tls = tls;
        ac->has_imap = TRUE;
      } else {
        ac->found->smtp_host = g_strdup (ac->hostname);
        ac->found->smtp_user = user;
        ac->found->smtp_tls = tls;
        ac->has_smtp = TRUE;
      }
    }
    ac->server = 0;
    g_clear_pointer (&ac->hostname, g_free);
    g_clear_pointer (&ac->username, g_free);
    g_clear_pointer (&ac->socket_type, g_free);
  }
  g_ptr_array_set_size (ac->path, depth - 1);
  g_string_truncate (ac->text, 0);
}

/* mail_client_parse_autoconfig_xml: the first IMAP and the first SMTP server of the file's emailProvider */
static NsMailFound *
ac_parse (GBytes *body, const char *email)
{
  g_autofree char *local = NULL, *domain = NULL;
  ns_goa_parse_email (email, &local, &domain);
  Autoconfig ac = { .email = email, .local = local, .domain = domain };
  ac.found = g_new0 (NsMailFound, 1);
  ac.path = g_ptr_array_new_with_free_func (g_free);
  ac.text = g_string_new (NULL);
  static const GMarkupParser parser = { ac_start, ac_end, ac_text, NULL, NULL };
  g_autoptr (GMarkupParseContext) ctx = g_markup_parse_context_new (&parser, 0, &ac, NULL);
  gsize len;
  const char *xml = g_bytes_get_data (body, &len);
  gboolean ok = g_markup_parse_context_parse (ctx, xml, len, NULL) && g_markup_parse_context_end_parse (ctx, NULL);
  g_ptr_array_unref (ac.path);
  g_string_free (ac.text, TRUE);
  g_free (ac.hostname);
  g_free (ac.username);
  g_free (ac.socket_type);
  if (!ok) {
    ns_mail_found_free (ac.found);
    return NULL;
  }
  return ac.found;
}

/* the first file that answers 200 and parses, of `uris` */
static NsMailFound *
ac_try (SoupSession *s, GPtrArray *uris, const char *email, GCancellable *cancellable, GError **error)
{
  for (guint i = 0; i < uris->len; i++) {
    g_autoptr (SoupMessage) msg = soup_message_new (SOUP_METHOD_GET, g_ptr_array_index (uris, i));
    if (!msg)
      continue;
    g_autoptr (GError) e = NULL;
    g_autoptr (GBytes) body = soup_session_send_and_read (s, msg, cancellable, &e);
    if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_propagate_error (error, g_steal_pointer (&e));
      return NULL;
    }
    if (!body || soup_message_get_status (msg) != SOUP_STATUS_OK)
      continue;
    NsMailFound *found = ac_parse (body, email);
    if (found)
      return found;
  }
  return NULL;
}

static gint
mx_compare (gconstpointer a, gconstpointer b)
{
  guint16 pa, pb;
  g_variant_get ((GVariant *) a, "(q&s)", &pa, NULL);
  g_variant_get ((GVariant *) b, "(q&s)", &pb, NULL);
  return (pa > pb) - (pa < pb);
}

static void
autoconfig_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  const char *email = data;
  g_autofree char *domain = NULL;
  GError *error = NULL;
  ns_goa_parse_email (email, NULL, &domain);
  g_autoptr (SoupSession) s = http_session (AUTOCONFIG_TIMEOUT);
  g_autoptr (GPtrArray) uris = g_ptr_array_new_with_free_func (g_free);

  /* the address's domain: the provider's own files, then the ISPDB (GOA puts the address in as it is) */
  g_ptr_array_add (uris, g_strdup_printf ("https://autoconfig.%s/.well-known/mail-v1.xml?emailaddress=%s", domain,
                                          email));
  g_ptr_array_add (uris, g_strdup_printf ("https://autoconfig.%s/mail/config-v1.1.xml?emailaddress=%s", domain,
                                          email));
  g_ptr_array_add (uris, g_strdup_printf ("https://%s/.well-known/autoconfig/mail/config-v1.1.xml", domain));
  g_ptr_array_add (uris, g_strdup_printf ("http://autoconfig.%s/mail/config-v1.1.xml", domain));
  g_ptr_array_add (uris, g_strdup_printf ("https://v1.ispdb.net/%s", domain));
  NsMailFound *found = ac_try (s, uris, email, cancellable, &error);

  /* then the same for the domain's mail exchanger: the one first in preference, and its base domain */
  if (!found && !error) {
    g_autoptr (GResolver) resolver = g_resolver_get_default ();
    GList *mx = g_resolver_lookup_records (resolver, domain, G_RESOLVER_RECORD_MX, cancellable, NULL);
    if (mx) {
      mx = g_list_sort (mx, mx_compare);
      const char *raw;
      g_variant_get (mx->data, "(q&s)", NULL, &raw);
      g_autofree char *full = g_utf8_casefold (raw, -1);
      const char *base = soup_tld_get_base_domain (full, NULL);
      gboolean other = base && g_strcmp0 (base, full) != 0;
      g_ptr_array_set_size (uris, 0);
      g_ptr_array_add (uris, g_strdup_printf ("https://autoconfig.%s/.well-known/mail-v1.xml?emailaddress=%s", full,
                                              email));
      if (other)
        g_ptr_array_add (uris, g_strdup_printf ("https://autoconfig.%s/.well-known/mail-v1.xml?emailaddress=%s",
                                                base, email));
      g_ptr_array_add (uris, g_strdup_printf ("https://v1.ispdb.net/%s", full));
      if (other)
        g_ptr_array_add (uris, g_strdup_printf ("https://v1.ispdb.net/%s", base));
      g_list_free_full (mx, (GDestroyNotify) g_variant_unref);
      found = ac_try (s, uris, email, cancellable, &error);
    }
    if (!found && !error)
      g_cancellable_set_error_if_cancelled (cancellable, &error);
  }
  if (found)
    g_task_return_pointer (task, found, (GDestroyNotify) ns_mail_found_free);
  else if (error)
    g_task_return_error (task, error);
  else
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Failed to autoconfig '%s'", email);
}

void
ns_mail_autoconfig (const char *email, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, g_strdup (email), g_free);
  g_task_run_in_thread (task, autoconfig_thread);
}

NsMailFound *
ns_mail_autoconfig_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- Exchange (goaewsclient.c) ---- */

typedef struct {
  SoupSession  *session;
  Http          http;
  GBytes       *request;
  int           pending;
  gboolean      done;
  GError       *error;     /* the first request's error, while no answer was good */
  GCancellable *cancel;    /* the requests', cancelled once one answered */
} Ews;

static void
ews_free (Ews *e)
{
  g_clear_object (&e->session);
  http_clear (&e->http);
  g_bytes_unref (e->request);
  g_clear_error (&e->error);
  g_clear_object (&e->cancel);
  g_free (e);
}

typedef struct {
  GPtrArray *path;
  guint      reached;  /* how many of Autodiscover, Response, Account, Protocol, ASUrl were found in that order */
} EwsParse;

/* Autodiscover/Response/Account/Protocol/ASUrl: the answer GOA takes as good */
static void
ews_start (GMarkupParseContext *ctx, const char *name, const char **attrs, const char **values, gpointer data,
           GError **error)
{
  static const char *const want[] = { "Autodiscover", "Response", "Account", "Protocol", "ASUrl" };
  EwsParse *p = data;
  name = local_name (name);
  guint depth = p->path->len;
  if (depth == 0 && !g_str_equal (name, "Autodiscover")) {
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Failed to find “%s” element", "Autodiscover");
    return;
  }
  gboolean on_path = depth < G_N_ELEMENTS (want) && g_str_equal (name, want[depth]);
  for (guint i = 0; on_path && i < depth; i++)
    on_path = g_str_equal (g_ptr_array_index (p->path, i), want[i]);
  if (on_path && depth + 1 > p->reached)
    p->reached = depth + 1;
  g_ptr_array_add (p->path, g_strdup (name));
}

static void
ews_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  EwsParse *p = data;
  g_ptr_array_set_size (p->path, p->path->len - 1);
}

static gboolean
ews_parse (GBytes *body, GError **error)
{
  EwsParse p = { g_ptr_array_new_with_free_func (g_free), 0 };
  static const GMarkupParser parser = { ews_start, ews_end, NULL, NULL, NULL };
  g_autoptr (GMarkupParseContext) ctx = g_markup_parse_context_new (&parser, 0, &p, NULL);
  gsize len;
  const char *xml = g_bytes_get_data (body, &len);
  g_autoptr (GError) e = NULL;
  gboolean ok = g_markup_parse_context_parse (ctx, xml, len, &e) && g_markup_parse_context_end_parse (ctx, &e);
  g_ptr_array_unref (p.path);
  if (!ok) {
    if (e && e->domain == GOA_ERROR)
      g_propagate_error (error, g_steal_pointer (&e));
    else
      g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Failed to parse autodiscover response XML");
    return FALSE;
  }
  /* GOA's messages for what is missing */
  if (p.reached < 2) {
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Failed to find “%s” element", "Response");
    return FALSE;
  }
  if (p.reached < 3) {
    g_set_error (error, GOA_ERROR, GOA_ERROR_FAILED, "Failed to find “%s” element", "Account");
    return FALSE;
  }
  if (p.reached < 5) {
    g_set_error_literal (error, GOA_ERROR, GOA_ERROR_FAILED, "Failed to find ASUrl in autodiscover response");
    return FALSE;
  }
  return TRUE;
}

/* a redirect makes libsoup's POST a GET: back to a POST with the request */
static void
ews_restarted (SoupMessage *msg, gpointer data)
{
  Ews *e = data;
  soup_message_set_method (msg, SOUP_METHOD_POST);
  soup_message_set_request_body_from_bytes (msg, "text/xml; charset=utf-8", e->request);
}

static void
ews_answered (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Ews *e = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  e->pending--;
  if (e->done)
    return;
  if (!body) {
    http_error (&e->http, &error);
  } else if (soup_message_get_status (msg) != SOUP_STATUS_OK) {
    set_error_status (&error, soup_message_get_status (msg));
  } else if (ews_parse (body, &error)) {
    e->done = TRUE;
    g_cancellable_cancel (e->cancel);
    g_task_return_boolean (task, TRUE);
    return;
  }
  /* a certificate error or a cancel is what is shown; otherwise the first error */
  if (!e->error || g_error_matches (error, GOA_ERROR, GOA_ERROR_SSL))
    g_set_error_literal (&e->error, error->domain, error->code, error->message);
  if (e->pending == 0) {
    e->done = TRUE;
    g_task_return_error (task, g_steal_pointer (&e->error));
  }
}

static void
ews_cancelled (GCancellable *cancellable, gpointer data)
{
  g_cancellable_cancel (data);
}

void
ns_ews_autodiscover (const char *email, const char *password, const char *user, const char *server,
                     gboolean accept_ssl_errors, GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer data)
{
  Ews *e = g_new0 (Ews, 1);
  e->http.user = g_strdup (user);
  e->http.password = g_strdup (password);
  e->http.accept_ssl_errors = accept_ssl_errors;
  e->session = soup_session_new ();
  soup_session_add_feature_by_type (e->session, SOUP_TYPE_AUTH_NTLM);
  e->cancel = g_cancellable_new ();
  if (cancellable)
    g_cancellable_connect (cancellable, G_CALLBACK (ews_cancelled), g_object_ref (e->cancel), g_object_unref);
  g_autofree char *escaped = g_markup_escape_text (email, -1);
  g_autofree char *xml = g_strdup_printf (
    "<?xml version=\"1.0\"?>\n"
    "<Autodiscover xmlns=\"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006\">"
    "<Request><EMailAddress>%s</EMailAddress>"
    "<AcceptableResponseSchema>http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a"
    "</AcceptableResponseSchema></Request></Autodiscover>",
    escaped);
  gsize xml_len = strlen (xml);
  e->request = g_bytes_new_take (g_steal_pointer (&xml), xml_len);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, e, (GDestroyNotify) ews_free);

  /* GOA asks both at once: some firewalls drop the first instead of refusing it, which takes long to time out */
  g_autofree char *url1 = g_strdup_printf ("https://%s/autodiscover/autodiscover.xml", server);
  g_autofree char *url2 = g_strdup_printf ("https://autodiscover.%s/autodiscover/autodiscover.xml", server);
  const char *urls[] = { url1, url2 };
  for (guint i = 0; i < G_N_ELEMENTS (urls); i++) {
    g_autoptr (SoupMessage) msg = soup_message_new (SOUP_METHOD_POST, urls[i]);
    if (!msg)
      continue;
    soup_message_headers_append (soup_message_get_request_headers (msg), "User-Agent", "libews/0.1");
    soup_message_set_request_body_from_bytes (msg, "text/xml; charset=utf-8", e->request);
    http_watch (&e->http, msg);
    g_signal_connect (msg, "restarted", G_CALLBACK (ews_restarted), e);
    e->pending++;
    soup_session_send_and_read_async (e->session, msg, G_PRIORITY_DEFAULT, e->cancel, ews_answered,
                                      g_object_ref (task));
  }
  if (e->pending == 0)
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", server);
}

gboolean
ns_ews_autodiscover_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---- Nextcloud: Login Flow v2 (goaowncloudprovider.c) ---- */

#define LOGINFLOW2_PATH "index.php/login/v2"

typedef struct {
  SoupSession *session;
  Http         http;
} Flow;

static void
flow_free (Flow *f)
{
  g_clear_object (&f->session);
  http_clear (&f->http);
  g_free (f);
}

static GTask *
flow_task (gboolean accept_ssl_errors, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  Flow *f = g_new0 (Flow, 1);
  f->session = http_session (DAV_TIMEOUT);
  f->http.accept_ssl_errors = accept_ssl_errors;
  GTask *task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_task_data (task, f, (GDestroyNotify) flow_free);
  return task;
}

static JsonObject *
flow_json (GBytes *body, JsonParser *parser)
{
  gsize len;
  const char *data = g_bytes_get_data (body, &len);
  if (!json_parser_load_from_data (parser, data, len, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  return root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

static const char *
json_string (JsonObject *o, const char *member)
{
  JsonNode *n = o ? json_object_get_member (o, member) : NULL;
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n)
                                                                                         : NULL;
}

typedef struct {
  char *a, *b, *c;
} Three;

static void
three_free (Three *t)
{
  g_free (t->a);
  g_free (t->b);
  g_free (t->c);
  g_free (t);
}

static void
flow_started (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Flow *f = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (!body) {
    http_error (&f->http, &error);
    g_task_return_error (task, error);
    return;
  }
  /* GOA's own words for a server without the flow */
  if (soup_message_get_status (msg) != SOUP_STATUS_OK) {
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_NOT_SUPPORTED, "Not supported");
    return;
  }
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *o = flow_json (body, parser);
  JsonObject *poll = o && json_object_has_member (o, "poll") &&
                         JSON_NODE_HOLDS_OBJECT (json_object_get_member (o, "poll"))
                       ? json_object_get_object_member (o, "poll")
                       : NULL;
  const char *login = json_string (o, "login"), *endpoint = json_string (poll, "endpoint");
  const char *token = json_string (poll, "token");
  if (!login || !endpoint || !token) {
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Could not parse response");
    return;
  }
  Three *t = g_new0 (Three, 1);
  t->a = g_strdup (login);
  t->b = g_strdup (endpoint);
  t->c = g_strdup (token);
  g_task_return_pointer (task, t, (GDestroyNotify) three_free);
}

void
ns_nextcloud_start (const char *uri, gboolean accept_ssl_errors, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = flow_task (accept_ssl_errors, cancellable, callback, data);
  Flow *f = g_task_get_task_data (task);
  g_autofree char *target = g_strrstr (uri, LOGINFLOW2_PATH) ? g_strdup (uri)
                                                            : g_uri_resolve_relative (uri, LOGINFLOW2_PATH,
                                                                                      G_URI_FLAGS_PARSE_RELAXED |
                                                                                        G_URI_FLAGS_ENCODED,
                                                                                      NULL);
  g_autoptr (SoupMessage) msg = target ? soup_message_new (SOUP_METHOD_POST, target) : NULL;
  if (!msg) {
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", uri);
    return;
  }
  http_watch (&f->http, msg);
  soup_session_send_and_read_async (f->session, msg, G_PRIORITY_DEFAULT, cancellable, flow_started,
                                    g_object_ref (task));
}

gboolean
ns_nextcloud_start_finish (GAsyncResult *result, char **login, char **poll, char **token, GError **error)
{
  Three *t = g_task_propagate_pointer (G_TASK (result), error);
  if (!t)
    return FALSE;
  *login = g_steal_pointer (&t->a);
  *poll = g_steal_pointer (&t->b);
  *token = g_steal_pointer (&t->c);
  three_free (t);
  return TRUE;
}

static void
flow_polled (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Flow *f = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  if (!body) {
    http_error (&f->http, &error);
    g_task_return_error (task, error);
    return;
  }
  guint status = soup_message_get_status (msg);
  /* the server answers 404 until the user has signed in */
  if (status == SOUP_STATUS_NOT_FOUND) {
    g_task_return_pointer (task, g_new0 (Three, 1), (GDestroyNotify) three_free);
    return;
  }
  if (status != SOUP_STATUS_OK) {
    set_error_status (&error, status);
    g_task_return_error (task, error);
    return;
  }
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *o = flow_json (body, parser);
  const char *server = json_string (o, "server"), *login = json_string (o, "loginName");
  const char *password = json_string (o, "appPassword");
  if (!server || !login || !password) {
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Could not parse response");
    return;
  }
  Three *t = g_new0 (Three, 1);
  t->a = g_strdup (server);
  t->b = g_strdup (login);
  t->c = g_strdup (password);
  g_task_return_pointer (task, t, (GDestroyNotify) three_free);
}

void
ns_nextcloud_poll (const char *poll, const char *token, gboolean accept_ssl_errors, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = flow_task (accept_ssl_errors, cancellable, callback, data);
  Flow *f = g_task_get_task_data (task);
  g_autofree char *form = g_strdup_printf ("token=%s", token);
  g_autoptr (SoupMessage) msg = soup_message_new_from_encoded_form (SOUP_METHOD_POST, poll, g_steal_pointer (&form));
  if (!msg) {
    g_task_return_new_error (task, GOA_ERROR, GOA_ERROR_FAILED, "Invalid URI: %s", poll);
    return;
  }
  http_watch (&f->http, msg);
  soup_session_send_and_read_async (f->session, msg, G_PRIORITY_DEFAULT, cancellable, flow_polled,
                                    g_object_ref (task));
}

gboolean
ns_nextcloud_poll_finish (GAsyncResult *result, char **server, char **login, char **app_password, GError **error)
{
  Three *t = g_task_propagate_pointer (G_TASK (result), error);
  if (!t)
    return FALSE;
  *server = g_steal_pointer (&t->a);
  *login = g_steal_pointer (&t->b);
  *app_password = g_steal_pointer (&t->c);
  three_free (t);
  return TRUE;
}
