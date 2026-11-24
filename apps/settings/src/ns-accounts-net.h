/* The checks GNOME Online Accounts' 3.58 provider dialogs make before they add an account, done the same way for
 * Settings' own account pages (ns-accounts.c), so that an account added there is the account GOA's dialog would have
 * added: the WebDAV discovery, the IMAP and SMTP sign-in, the mail autoconfig lookup, Exchange's autodiscover, and
 * GOA's URL and address helpers. Nothing here touches GTK. Errors are GOA's (GOA_ERROR, GOA's messages): a
 * certificate error is GOA_ERROR_SSL, which the pages offer to ignore, as GOA's dialog does. */
#pragma once

#include <gio/gio.h>

/* goa_utils_normalize_url: `base` as an http(s) URL with a trailing '/' (https when it has no scheme, dav(s) taken as
 * http(s)), `ref` (or NULL) resolved against it; `server` (or NULL) gets the host, the port when it is not the
 * scheme's own, and the path without its trailing '/'. NULL when `base` is not such a URL. */
char *ns_goa_normalize_url (const char *base, const char *ref, char **server);

/* goa_utils_parse_email_address: "local@domain", both parts non-empty */
gboolean ns_goa_parse_email (const char *email, char **local, char **domain);

/* ---- WebDAV (goadavclient.c, goawebdavprovider.c) ---- */

/* the addresses an account's details get: NULL where nothing was found */
typedef struct {
  char *webdav;   /* "Uri": files */
  char *caldav;   /* "CalDavUri" */
  char *carddav;  /* "CardDavUri" */
} NsDavFound;

void ns_dav_found_free (NsDavFound *found);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NsDavFound, ns_dav_found_free)

/* the WebDAV dialog's check: discovery from `uri` (normalized already), then the addresses the user typed (or NULL):
 * `files` found by a discovery of its own, `caldav` and `carddav` checked and taken as they are */
void        ns_dav_add_check (const char *uri, const char *user, const char *password, const char *files,
                              const char *caldav, const char *carddav, gboolean accept_ssl_errors,
                              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
NsDavFound *ns_dav_add_check_finish (GAsyncResult *result, GError **error);

/* the check GOA makes of an account's own address (its EnsureCredentials and its "Sign in again"): an OPTIONS
 * request to `uri`, with the well-known fallback for CalDAV and CardDAV. `service` is "webdav", "caldav" or
 * "carddav". */
void     ns_dav_check (const char *service, const char *uri, const char *user, const char *password,
                       gboolean accept_ssl_errors, GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer data);
gboolean ns_dav_check_finish (GAsyncResult *result, GError **error);

/* ---- mail (goamailclient.c, goaimapauthlogin.c, goasmtpauth.c) ---- */

/* GoaTlsType: GOA offers these two; an autoconfig "plain" server is taken as SSL, as GOA's dialog does */
typedef enum {
  NS_TLS_STARTTLS = 1,
  NS_TLS_SSL = 2,
} NsTls;

/* what the autoconfig lookup found for an address: NULL hosts where it found nothing */
typedef struct {
  char  *imap_host, *imap_user;
  NsTls  imap_tls;
  char  *smtp_host, *smtp_user;
  NsTls  smtp_tls;
} NsMailFound;

void ns_mail_found_free (NsMailFound *found);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NsMailFound, ns_mail_found_free)

/* the IMAP and SMTP dialog's lookup of an address's servers: the domain's autoconfig files, the ISPDB, then the same
 * for the domain's mail exchanger */
void         ns_mail_autoconfig (const char *email, GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer data);
NsMailFound *ns_mail_autoconfig_finish (GAsyncResult *result, GError **error);

/* how the SMTP server takes a sign-in, from its EHLO (SmtpUseAuth, SmtpAuthLogin, SmtpAuthPlain) */
typedef struct {
  gboolean use_auth, auth_login, auth_plain;
} NsSmtpAuth;

/* GOA's sign-in check of a mail server: IMAP LOGIN, or SMTP AUTH as its EHLO offers it (none when it offers none).
 * `host` may carry a ":port"; otherwise the encryption's own port (993/143, 465/587). `domain` is the address's
 * domain, for SMTP's EHLO. `smtp_auth` (SMTP only, or NULL) gets what the server's EHLO said. */
void     ns_mail_check (gboolean smtp, const char *host, NsTls tls, gboolean accept_ssl_errors, const char *user,
                        const char *password, const char *domain, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer data);
gboolean ns_mail_check_finish (GAsyncResult *result, NsSmtpAuth *smtp_auth, GError **error);

/* ---- Exchange (goaewsclient.c) ---- */

/* the Exchange dialog's check: EWS autodiscover at https://`server`/ and https://autodiscover.`server`/ at once, the
 * first good answer winning */
void     ns_ews_autodiscover (const char *email, const char *password, const char *user, const char *server,
                              gboolean accept_ssl_errors, GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer data);
gboolean ns_ews_autodiscover_finish (GAsyncResult *result, GError **error);

/* ---- Nextcloud (goaowncloudprovider.c): Login Flow v2 ---- */

/* the flow's start: a POST to `uri` (normalized) + index.php/login/v2; `login` gets the page to open in the browser,
 * `poll` and `token` what to poll with */
void     ns_nextcloud_start (const char *uri, gboolean accept_ssl_errors, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer data);
gboolean ns_nextcloud_start_finish (GAsyncResult *result, char **login, char **poll, char **token, GError **error);

/* one poll: TRUE with the server's address, the login name and the app password once the user has signed in in the
 * browser; TRUE with all three NULL while the server still waits for them (it answers 404 until then) */
void     ns_nextcloud_poll (const char *poll, const char *token, gboolean accept_ssl_errors,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
gboolean ns_nextcloud_poll_finish (GAsyncResult *result, char **server, char **login, char **app_password,
                                   GError **error);
