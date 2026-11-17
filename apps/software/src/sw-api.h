/* SwApi: the store's API, version 1 (the server's docs/api.md), over libsoup.
 *
 * Every call is asynchronous and answers through a callback on the main loop:
 *
 *   sw_api_get (api, "/featured", SW_API_CACHED | SW_API_STALE_FIRST, cancellable, on_featured, self);
 *
 * Paths are relative to the API's root (VIBEOS_STORE_URL in the environment, else https://vibeos.top, plus
 * /api/v1), with their query. Every request carries Accept: application/json, gzip (libsoup's decoder) and a
 * User-Agent naming the VibeOS release and architecture; a signed-in account's token goes along as a Bearer token.
 *
 * Catalog reads (SW_API_CACHED) keep each answer and its ETag in memory and on disk (~/.cache/vibeos-software/api).
 * Within the answer's max-age (60 s) the kept copy is the answer and nothing is asked. After it the request goes out
 * with If-None-Match; a 304 means the kept copy is still right, a 200 replaces it. With SW_API_STALE_FIRST an old copy
 * is handed over at once (result->stale), before the request: the callback then runs a second time with the server's
 * answer (result->not_modified when that is the same), or with the error if there was none. SW_API_REVALIDATE asks
 * the server even within max-age (a pull to refresh). What a signed-in request returns is kept apart from what an
 * anonymous one does: reviews carry the account's own flags.
 *
 * The catalogue's lists (featured, browse, categories, apps, collections, search) can be narrowed to the apps of one
 * source: with sw_api_set_source (api, "vibeos") every GET of one of them carries source=vibeos, an app's own page,
 * its reviews and the account's requests never do. The kept answers follow the URLs, so each source's are its own.
 *
 * A cancelled request never calls back: a page cancels its GCancellable when it goes, and its callbacks can rely on
 * the page being there.
 *
 * Errors keep the server's shape (code, message, fields, retry_after, attempts_left, reason). A failure to reach the
 * server at all is SW_API_ERROR_NETWORK with a sentence of our own. A 401 on a request that carried a token also
 * emits "unauthorized": the token no longer works, and the account forgets it. */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
  SW_API_NONE        = 0,
  SW_API_CACHED      = 1 << 0,   /* a catalog read: through the ETag cache */
  SW_API_STALE_FIRST = 1 << 1,   /* hand over an old kept copy at once, then revalidate */
  SW_API_REVALIDATE  = 1 << 2,   /* ask the server even within max-age */
  SW_API_NO_TOKEN    = 1 << 3,   /* never send the token (anonymous install pings) */
} SwApiFlags;

typedef enum {
  SW_API_ERROR_NETWORK,          /* the server could not be reached */
  SW_API_ERROR_HTTP,             /* the server answered with an error */
  SW_API_ERROR_PARSE,            /* the answer was not what the API says it is */
} SwApiErrorKind;

typedef struct {
  SwApiErrorKind kind;
  guint          status;         /* the HTTP status; 0 for a network error */
  char          *code;           /* "validation_failed", "rate_limited", ...; "network" for a network error */
  char          *message;        /* one sentence for the user */
  JsonObject    *fields;         /* validation_failed: field -> [messages]; else NULL */
  int            retry_after;    /* seconds, or -1 */
  int            attempts_left;  /* code_invalid: tries left on the code, or -1 */
  char          *reason;         /* review_not_allowed: why, or NULL */
} SwApiError;

typedef struct {
  guint        status;           /* 200, 201, 202, 204; 304 when a kept copy was confirmed */
  JsonNode    *root;             /* the whole body, NULL for 204 */
  JsonNode    *data;             /* root's "data", NULL when there is none */
  const char  *next_cursor;      /* a list's next page, or NULL on the last one */
  JsonObject  *meta;             /* a list's "meta", or NULL */
  gboolean     from_cache;       /* the kept copy, not a fresh answer */
  gboolean     stale;            /* the kept copy is older than its max-age: the server's answer follows */
  gboolean     not_modified;     /* the server confirmed the copy handed over before */
} SwApiResult;

/* exactly one of result and error is set */
typedef void (*SwApiCallback) (SwApiResult *result, const SwApiError *error, gpointer data);

#define SW_TYPE_API (sw_api_get_type ())
G_DECLARE_FINAL_TYPE (SwApi, sw_api, SW, API, GObject)

/* the process's API, for VIBEOS_STORE_URL or vibeos.top */
SwApi      *sw_api_get_default   (void);
/* an API for `base_url` (the site: "http://127.0.0.1:8000"), keeping its answers in `cache_dir` (NULL: none) */
SwApi      *sw_api_new           (const char *base_url, const char *cache_dir);

const char *sw_api_get_base_url  (SwApi *self);
/* the token sent as Authorization: Bearer (NULL: none) */
void        sw_api_set_token     (SwApi *self, const char *token);
const char *sw_api_get_token     (SwApi *self);
/* the User-Agent's VibeOS release and architecture */
void        sw_api_set_identity  (SwApi *self, const char *vibeos_version, const char *arch);
/* only the apps of `source` ("vibeos" or "debian") in the catalogue's lists from now on, or all of them (NULL);
 * emits "catalog-changed" when that changes */
void        sw_api_set_source    (SwApi *self, const char *source);
const char *sw_api_get_source    (SwApi *self);
/* the max-age a kept answer is good for when the server names none, in seconds (60); tests set 0 */
void        sw_api_set_default_max_age (SwApi *self, int seconds);

void        sw_api_get           (SwApi *self, const char *path, SwApiFlags flags, GCancellable *cancellable,
                                  SwApiCallback callback, gpointer data);
/* POST, PUT, PATCH or DELETE, with `body` (NULL: none) as JSON. Never cached. */
void        sw_api_send          (SwApi *self, const char *method, const char *path, JsonNode *body,
                                  SwApiFlags flags, GCancellable *cancellable, SwApiCallback callback,
                                  gpointer data);

/* "/apps?type=app&sort=popular" with the next page's cursor added */
char       *sw_api_path_with_cursor (const char *path, const char *cursor);
/* a path segment with what is not unreserved percent-encoded (an app id, a query) */
char       *sw_api_escape        (const char *text);

/* forgets every kept answer (signing in or out changes what they hold) */
void        sw_api_clear_cache   (SwApi *self);

/* the errors' sentences for the user: the server's message, or ours for a network failure; a rate limit with the
 * wait in it */
char       *sw_api_error_describe (const SwApiError *error);
/* the first message for `field` in a validation error, or NULL */
const char *sw_api_error_field    (const SwApiError *error, const char *field);

/* Parses an answer the way a request does (for the tests): status, the Retry-After header's value (or NULL) and the
 * body. Returns TRUE for a success, filling `result`; else fills `error`. */
gboolean    sw_api_parse_response (guint status, const char *retry_after, GBytes *body, SwApiResult *result,
                                   SwApiError *error);
void        sw_api_result_clear  (SwApiResult *result);
void        sw_api_error_clear   (SwApiError *error);

/* Signals: "unauthorized" (): a request with the token got 401. "catalog-changed" (): the source changed, and with
 * it every list of apps: what shows them asks again. */

G_END_DECLS
