/* SwApi: see sw-api.h */
#include "sw-api.h"
#include "sw-json.h"

#include <libsoup/soup.h>
#include <string.h>

#define DEFAULT_BASE_URL "https://vibeos.top"
#define API_ROOT         "/api/v1"
#define DEFAULT_MAX_AGE  60

/* one kept answer */
typedef struct {
  char     *etag;
  gint64    fetched;     /* real time, µs */
  int       max_age;     /* s */
  GBytes   *body;
} Entry;

struct _SwApi {
  GObject      parent_instance;
  SoupSession *session;
  char        *base_url;
  char        *root;
  char        *token;
  char        *token_tag;    /* a short hash of the token: its answers are kept apart */
  char        *cache_dir;
  char        *user_agent;
  int          default_max_age;
  GHashTable  *memory;       /* key -> Entry */
};

G_DEFINE_FINAL_TYPE (SwApi, sw_api, G_TYPE_OBJECT)

enum { UNAUTHORIZED, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct {
  SwApi         *api;
  char          *method;
  char          *path;
  char          *url;
  GBytes        *body;
  SwApiFlags     flags;
  GCancellable  *cancellable;
  SwApiCallback  callback;
  gpointer       data;
  char          *key;          /* the cache key (cached GETs) */
  Entry         *entry;        /* a copy of the kept answer, if any */
  gboolean       stale_given;  /* the kept copy was handed over before the request */
  char          *token;        /* the token it carried, or NULL */
  SoupMessage   *message;
} Request;

static void
entry_free (gpointer data)
{
  Entry *entry = data;

  if (!entry)
    return;
  g_free (entry->etag);
  g_clear_pointer (&entry->body, g_bytes_unref);
  g_free (entry);
}

static Entry *
entry_copy (const Entry *entry)
{
  Entry *copy = g_new0 (Entry, 1);

  copy->etag = g_strdup (entry->etag);
  copy->fetched = entry->fetched;
  copy->max_age = entry->max_age;
  copy->body = entry->body ? g_bytes_ref (entry->body) : NULL;
  return copy;
}

static void
request_free (Request *req)
{
  g_object_unref (req->api);
  g_free (req->method);
  g_free (req->path);
  g_free (req->url);
  g_clear_pointer (&req->body, g_bytes_unref);
  g_clear_object (&req->cancellable);
  g_free (req->key);
  g_free (req->token);
  entry_free (req->entry);
  g_clear_object (&req->message);
  g_free (req);
}

/* ---- results and errors ---------------------------------------------------------------------------------------- */

void
sw_api_result_clear (SwApiResult *result)
{
  g_clear_pointer (&result->root, json_node_unref);
  result->data = NULL;
  result->next_cursor = NULL;
  result->meta = NULL;
}

void
sw_api_error_clear (SwApiError *error)
{
  g_clear_pointer (&error->code, g_free);
  g_clear_pointer (&error->message, g_free);
  g_clear_pointer (&error->fields, json_object_unref);
  g_clear_pointer (&error->reason, g_free);
}

static void
error_init (SwApiError *error, SwApiErrorKind kind, guint status, const char *code, const char *message)
{
  memset (error, 0, sizeof *error);
  error->kind = kind;
  error->status = status;
  error->code = g_strdup (code);
  error->message = g_strdup (message);
  error->retry_after = -1;
  error->attempts_left = -1;
}

static int
parse_seconds (const char *text)
{
  char *end;
  gint64 value;

  if (!text)
    return -1;
  value = g_ascii_strtoll (text, &end, 10);
  return end != text && value >= 0 ? (int) MIN (value, G_MAXINT) : -1;
}

/* a sentence for an error answer that is not the API's error shape (a proxy's page, a crash) */
static char *
generic_message (guint status)
{
  if (status >= 500)
    return g_strdup_printf ("The store is not working right now (error %u). Try again later.", status);
  return g_strdup_printf ("The store answered with error %u.", status);
}

gboolean
sw_api_parse_response (guint status, const char *retry_after, GBytes *body, SwApiResult *result,
                       SwApiError *error)
{
  gsize size = 0;
  const char *text = body ? g_bytes_get_data (body, &size) : NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root = NULL;
  gboolean parsed = text && size && json_parser_load_from_data (parser, text, size, NULL);

  if (parsed)
    root = sw_json_node_object (json_parser_get_root (parser));

  if (status >= 200 && status < 300)
    {
      memset (result, 0, sizeof *result);
      result->status = status;
      if (status == 204 || !size)
        return TRUE;
      if (!root)
        {
          error_init (error, SW_API_ERROR_PARSE, status, "parse_error",
                      "The store's answer could not be read. Try again later.");
          return FALSE;
        }
      result->root = json_node_ref (json_parser_get_root (parser));
      result->data = json_object_get_member (root, "data");
      result->next_cursor = sw_json_string (root, "next_cursor");
      result->meta = sw_json_object (root, "meta");
      return TRUE;
    }

  {
    JsonObject *shape = sw_json_object (root, "error");
    const char *code = sw_json_string (shape, "code"), *message = sw_json_string (shape, "message");
    g_autofree char *generic = NULL;

    if (!code)
      code = status >= 500 ? "server_error" : status == 404 ? "not_found" : status == 429 ? "rate_limited"
                                                                                      : "http_error";
    if (!message)
      message = generic = generic_message (status);
    error_init (error, SW_API_ERROR_HTTP, status, code, message);
    if (sw_json_object (shape, "fields"))
      error->fields = json_object_ref (sw_json_object (shape, "fields"));
    error->retry_after = (int) sw_json_int (shape, "retry_after", parse_seconds (retry_after));
    error->attempts_left = (int) sw_json_int (shape, "attempts_left", -1);
    error->reason = g_strdup (sw_json_string (shape, "reason"));
  }
  return FALSE;
}

char *
sw_api_error_describe (const SwApiError *error)
{
  if (!error)
    return g_strdup ("");
  if ((!g_strcmp0 (error->code, "rate_limited") || !g_strcmp0 (error->code, "resend_cooldown")
       || !g_strcmp0 (error->code, "service_unavailable"))
      && error->retry_after > 0 && !strstr (error->message ? error->message : "", "second"))
    {
      int s = error->retry_after;

      if (s >= 120)
        return g_strdup_printf ("%s Try again in %d minutes.", error->message, (s + 59) / 60);
      return g_strdup_printf ("%s Try again in %d second%s.", error->message, s, s == 1 ? "" : "s");
    }
  return g_strdup (error->message ? error->message : "Something went wrong.");
}

const char *
sw_api_error_field (const SwApiError *error, const char *field)
{
  JsonArray *messages = error ? sw_json_array (error->fields, field) : NULL;
  JsonNode *first = messages && json_array_get_length (messages) ? json_array_get_element (messages, 0) : NULL;

  return first && JSON_NODE_HOLDS_VALUE (first) && json_node_get_value_type (first) == G_TYPE_STRING
         ? json_node_get_string (first) : NULL;
}

/* ---- the cache ------------------------------------------------------------------------------------------------- */

static char *
cache_key (SwApi *self, const char *url, gboolean with_token)
{
  return g_strconcat (with_token && self->token_tag ? self->token_tag : "-", " ", url, NULL);
}

static char *
cache_file (SwApi *self, const char *key)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, key, -1);
  g_autofree char *name = g_strconcat (hash, ".json", NULL);

  return self->cache_dir ? g_build_filename (self->cache_dir, name, NULL) : NULL;
}

/* "ETAG\tFETCHED\tMAX-AGE\n" then the body */
static Entry *
entry_from_file (const char *contents, gsize length)
{
  const char *newline = memchr (contents, '\n', length);
  g_auto (GStrv) head = NULL;
  Entry *entry;

  if (!newline)
    return NULL;
  {
    g_autofree char *line = g_strndup (contents, newline - contents);

    head = g_strsplit (line, "\t", 3);
  }
  if (g_strv_length (head) != 3)
    return NULL;
  entry = g_new0 (Entry, 1);
  entry->etag = *head[0] ? g_strdup (head[0]) : NULL;
  entry->fetched = g_ascii_strtoll (head[1], NULL, 10);
  entry->max_age = (int) g_ascii_strtoll (head[2], NULL, 10);
  entry->body = g_bytes_new (newline + 1, length - (newline + 1 - contents));
  return entry;
}

static void
on_cache_written (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), res, NULL, &error))
    g_debug ("could not keep an answer: %s", error->message);
}

static void
entry_save (SwApi *self, const char *key, const Entry *entry)
{
  g_autofree char *path = cache_file (self, key);
  g_autoptr (GFile) file = NULL;
  g_autoptr (GBytes) bytes = NULL;
  GString *contents;
  gsize size = 0;
  const char *body;

  if (!path || !entry->body)
    return;
  g_mkdir_with_parents (self->cache_dir, 0700);
  body = g_bytes_get_data (entry->body, &size);
  contents = g_string_new (NULL);
  g_string_append_printf (contents, "%s\t%" G_GINT64_FORMAT "\t%d\n", entry->etag ? entry->etag : "",
                          entry->fetched, entry->max_age);
  g_string_append_len (contents, body, size);
  bytes = g_string_free_to_bytes (contents);
  file = g_file_new_for_path (path);
  g_file_replace_contents_bytes_async (file, bytes, NULL, FALSE,
                                       G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                                       NULL, on_cache_written, NULL);
}

static void
entry_store (SwApi *self, const char *key, Entry *entry)
{
  g_hash_table_replace (self->memory, g_strdup (key), entry);
}

static gboolean
entry_is_fresh (const Entry *entry)
{
  return g_get_real_time () - entry->fetched < (gint64) entry->max_age * G_USEC_PER_SEC;
}

static int
parse_max_age (const char *cache_control, int fallback)
{
  const char *at = cache_control ? strstr (cache_control, "max-age=") : NULL;

  if (!at)
    return fallback;
  return MAX (0, parse_seconds (at + strlen ("max-age=")));
}

void
sw_api_clear_cache (SwApi *self)
{
  g_hash_table_remove_all (self->memory);
}

/* ---- delivery -------------------------------------------------------------------------------------------------- */

/* status: 200 for a kept copy given as it is, 304 when the server just confirmed it */
static void
deliver_entry (Request *req, const Entry *entry, guint status, gboolean stale, gboolean not_modified)
{
  SwApiResult result;
  SwApiError error;

  if (g_cancellable_is_cancelled (req->cancellable))
    return;
  if (!sw_api_parse_response (200, NULL, entry->body, &result, &error))
    {
      /* a kept copy that does not parse is no copy: say nothing, the request decides */
      sw_api_error_clear (&error);
      return;
    }
  result.status = status;
  result.from_cache = TRUE;
  result.stale = stale;
  result.not_modified = not_modified;
  req->callback (&result, NULL, req->data);
  sw_api_result_clear (&result);
}

static void
deliver_error (Request *req, SwApiError *error)
{
  if (!g_cancellable_is_cancelled (req->cancellable))
    req->callback (NULL, error, req->data);
}

/* ---- the request ----------------------------------------------------------------------------------------------- */

static void
on_sent (GObject *source, GAsyncResult *res, gpointer data)
{
  Request *req = data;
  SwApi *self = req->api;
  g_autoptr (GError) gerror = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &gerror);
  SoupMessageHeaders *headers;
  guint status;
  SwApiResult result;
  SwApiError error;

  if (g_error_matches (gerror, G_IO_ERROR, G_IO_ERROR_CANCELLED) || g_cancellable_is_cancelled (req->cancellable))
    {
      request_free (req);
      return;
    }
  if (!body)
    {
      g_debug ("%s %s: %s", req->method, req->url, gerror ? gerror->message : "no answer");
      error_init (&error, SW_API_ERROR_NETWORK, 0, "network",
                  "Software cannot reach the store. Check the internet connection and try again.");
      deliver_error (req, &error);
      sw_api_error_clear (&error);
      request_free (req);
      return;
    }

  status = soup_message_get_status (req->message);
  headers = soup_message_get_response_headers (req->message);

  /* the kept copy is still right */
  if (status == SOUP_STATUS_NOT_MODIFIED && req->entry)
    {
      Entry *entry = entry_copy (req->entry);

      entry->fetched = g_get_real_time ();
      entry->max_age = parse_max_age (soup_message_headers_get_one (headers, "Cache-Control"), entry->max_age);
      entry_store (self, req->key, entry);
      entry_save (self, req->key, entry);
      deliver_entry (req, entry, SOUP_STATUS_NOT_MODIFIED, FALSE, req->stale_given);
      request_free (req);
      return;
    }

  if (sw_api_parse_response (status, soup_message_headers_get_one (headers, "Retry-After"), body, &result, &error))
    {
      if (req->key && status == 200)
        {
          Entry *entry = g_new0 (Entry, 1);

          entry->etag = g_strdup (soup_message_headers_get_one (headers, "ETag"));
          entry->fetched = g_get_real_time ();
          entry->max_age = parse_max_age (soup_message_headers_get_one (headers, "Cache-Control"),
                                          self->default_max_age);
          entry->body = g_bytes_ref (body);
          entry_store (self, req->key, entry);
          entry_save (self, req->key, entry);
        }
      if (!g_cancellable_is_cancelled (req->cancellable))
        req->callback (&result, NULL, req->data);
      sw_api_result_clear (&result);
    }
  else
    {
      g_debug ("%s %s: %u %s", req->method, req->url, status, error.code);
      /* only while that token is still the account's: one signed out meanwhile is gone already */
      if (status == SOUP_STATUS_UNAUTHORIZED && req->token && !g_strcmp0 (req->token, self->token))
        g_signal_emit (self, signals[UNAUTHORIZED], 0);
      deliver_error (req, &error);
      sw_api_error_clear (&error);
    }
  request_free (req);
}

static void
send_request (Request *req)
{
  SwApi *self = req->api;
  SoupMessageHeaders *headers;

  req->message = soup_message_new (req->method, req->url);
  if (!req->message)
    {
      SwApiError error;

      error_init (&error, SW_API_ERROR_NETWORK, 0, "network", "The store's address is not valid.");
      deliver_error (req, &error);
      sw_api_error_clear (&error);
      request_free (req);
      return;
    }
  headers = soup_message_get_request_headers (req->message);
  soup_message_headers_replace (headers, "Accept", "application/json");
  soup_message_headers_replace (headers, "User-Agent", self->user_agent);
  if (req->token)
    {
      g_autofree char *bearer = g_strconcat ("Bearer ", req->token, NULL);

      soup_message_headers_replace (headers, "Authorization", bearer);
    }
  if (req->entry && req->entry->etag)
    soup_message_headers_replace (headers, "If-None-Match", req->entry->etag);
  if (req->body)
    soup_message_set_request_body_from_bytes (req->message, "application/json", req->body);
  soup_session_send_and_read_async (self->session, req->message, G_PRIORITY_DEFAULT, req->cancellable, on_sent,
                                    req);
}

/* the kept copy is known (or known to be missing): answer from it, or ask */
static void
continue_cached (Request *req)
{
  if (req->entry && !(req->flags & SW_API_REVALIDATE) && entry_is_fresh (req->entry))
    {
      deliver_entry (req, req->entry, 200, FALSE, FALSE);
      request_free (req);
      return;
    }
  if (req->entry && (req->flags & SW_API_STALE_FIRST))
    {
      deliver_entry (req, req->entry, 200, TRUE, FALSE);
      req->stale_given = TRUE;
    }
  if (g_cancellable_is_cancelled (req->cancellable))
    {
      request_free (req);
      return;
    }
  send_request (req);
}

static void
on_cache_read (GObject *source, GAsyncResult *res, gpointer data)
{
  Request *req = data;
  g_autofree char *contents = NULL;
  gsize length = 0;

  if (g_file_load_contents_finish (G_FILE (source), res, &contents, &length, NULL, NULL))
    {
      Entry *entry = entry_from_file (contents, length);

      if (entry)
        {
          /* another request may have filled it meanwhile: the newer one stays */
          Entry *now = g_hash_table_lookup (req->api->memory, req->key);

          if (!now || now->fetched < entry->fetched)
            entry_store (req->api, req->key, entry_copy (entry));
          req->entry = now && now->fetched >= entry->fetched ? entry_copy (now) : entry_copy (entry);
          entry_free (entry);
        }
    }
  continue_cached (req);
}

static gboolean
continue_later (gpointer data)
{
  continue_cached (data);
  return G_SOURCE_REMOVE;
}

static Request *
request_new (SwApi *self, const char *method, const char *path, SwApiFlags flags, GCancellable *cancellable,
             SwApiCallback callback, gpointer data)
{
  Request *req = g_new0 (Request, 1);

  req->api = g_object_ref (self);
  req->method = g_strdup (method);
  req->path = g_strdup (path);
  req->url = g_strconcat (self->root, path, NULL);
  req->flags = flags;
  req->cancellable = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
  req->callback = callback;
  req->data = data;
  req->token = !(flags & SW_API_NO_TOKEN) ? g_strdup (self->token) : NULL;
  return req;
}

void
sw_api_get (SwApi *self, const char *path, SwApiFlags flags, GCancellable *cancellable, SwApiCallback callback,
            gpointer data)
{
  Request *req = request_new (self, "GET", path, flags, cancellable, callback, data);
  Entry *entry;

  if (!(flags & SW_API_CACHED))
    {
      send_request (req);
      return;
    }
  req->key = cache_key (self, req->url, req->token != NULL);
  entry = g_hash_table_lookup (self->memory, req->key);
  if (entry || !self->cache_dir)
    {
      req->entry = entry ? entry_copy (entry) : NULL;
      /* never back before the call returns */
      g_idle_add (continue_later, req);
      return;
    }
  {
    g_autofree char *path_name = cache_file (self, req->key);
    g_autoptr (GFile) file = g_file_new_for_path (path_name);

    g_file_load_contents_async (file, req->cancellable, on_cache_read, req);
  }
}

void
sw_api_send (SwApi *self, const char *method, const char *path, JsonNode *body, SwApiFlags flags,
             GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  Request *req = request_new (self, method, path, flags & ~(SW_API_CACHED | SW_API_STALE_FIRST), cancellable,
                              callback, data);

  if (body)
    {
      char *text = json_to_string (body, FALSE);
      gsize length = strlen (text);

      req->body = g_bytes_new_take (text, length);
    }
  send_request (req);
}

/* ---- paths ----------------------------------------------------------------------------------------------------- */

char *
sw_api_path_with_cursor (const char *path, const char *cursor)
{
  g_autofree char *escaped = NULL;

  if (!cursor)
    return g_strdup (path);
  escaped = g_uri_escape_string (cursor, NULL, FALSE);
  return g_strconcat (path, strchr (path, '?') ? "&cursor=" : "?cursor=", escaped, NULL);
}

char *
sw_api_escape (const char *text)
{
  return g_uri_escape_string (text ? text : "", NULL, FALSE);
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
update_user_agent (SwApi *self, const char *version, const char *arch)
{
  g_free (self->user_agent);
  self->user_agent = g_strdup_printf ("VibeOSSoftware/%s (VibeOS %s; %s)", SW_VERSION,
                                      version && *version ? version : "unknown", arch && *arch ? arch : "unknown");
}

static void
sw_api_finalize (GObject *object)
{
  SwApi *self = SW_API (object);

  g_clear_object (&self->session);
  g_free (self->base_url);
  g_free (self->root);
  g_free (self->token);
  g_free (self->token_tag);
  g_free (self->cache_dir);
  g_free (self->user_agent);
  g_hash_table_unref (self->memory);
  G_OBJECT_CLASS (sw_api_parent_class)->finalize (object);
}

static void
sw_api_class_init (SwApiClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sw_api_finalize;
  signals[UNAUTHORIZED] = g_signal_new ("unauthorized", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                        NULL, NULL, G_TYPE_NONE, 0);
}

static void
sw_api_init (SwApi *self)
{
  /* libsoup's content decoder asks for gzip and inflates it */
  self->session = soup_session_new_with_options ("timeout", 30, "idle-timeout", 60, "max-conns-per-host", 6, NULL);
  self->memory = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, entry_free);
  self->default_max_age = DEFAULT_MAX_AGE;
  update_user_agent (self, NULL, NULL);
}

SwApi *
sw_api_new (const char *base_url, const char *cache_dir)
{
  SwApi *self = g_object_new (SW_TYPE_API, NULL);
  g_autofree char *base = g_strdup (base_url && *base_url ? base_url : DEFAULT_BASE_URL);

  /* "https://vibeos.top/" and "https://vibeos.top" are the same server */
  while (g_str_has_suffix (base, "/"))
    base[strlen (base) - 1] = '\0';
  self->base_url = g_steal_pointer (&base);
  self->root = g_strconcat (self->base_url, API_ROOT, NULL);
  self->cache_dir = g_strdup (cache_dir);
  return self;
}

SwApi *
sw_api_get_default (void)
{
  static SwApi *api;

  if (!api)
    {
      g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "vibeos-software", "api", NULL);

      api = sw_api_new (g_getenv ("VIBEOS_STORE_URL"), dir);
    }
  return api;
}

const char *
sw_api_get_base_url (SwApi *self)
{
  return self->base_url;
}

void
sw_api_set_token (SwApi *self, const char *token)
{
  if (!g_strcmp0 (self->token, token))
    return;
  g_free (self->token);
  g_clear_pointer (&self->token_tag, g_free);
  self->token = token && *token ? g_strdup (token) : NULL;
  if (self->token)
    {
      g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, self->token, -1);

      self->token_tag = g_strndup (hash, 16);
    }
}

const char *
sw_api_get_token (SwApi *self)
{
  return self->token;
}

void
sw_api_set_identity (SwApi *self, const char *vibeos_version, const char *arch)
{
  update_user_agent (self, vibeos_version, arch);
}

void
sw_api_set_default_max_age (SwApi *self, int seconds)
{
  self->default_max_age = MAX (0, seconds);
}
