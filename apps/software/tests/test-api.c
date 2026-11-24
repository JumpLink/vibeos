/* The store client against a local SoupServer that answers like the store: ETags and 304s, max-age, cursors, the
 * error shape, rate limits, 401s, a proxy's HTML page and a closed port, and which requests carry the source. Also
 * the JSON helpers, dpkg's status file and the device id. */
#include "sw-api.h"
#include "sw-device.h"
#include "sw-dpkg.h"
#include "sw-json.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>

static SoupServer *server;
static char *base_url;
static int hits;                 /* requests that reached /catalog */
static char *last_if_none_match;
static char *last_authorization;
static char *last_body;
static char *catalog_etag;
static char *catalog_body;
static int catalog_max_age;
static int source_hits;          /* requests that reached the server at all */
static char *last_source;        /* the last one's source parameter */

static void
respond_json (SoupServerMessage *msg, guint status, const char *body)
{
  soup_server_message_set_status (msg, status, NULL);
  if (body)
    soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, body, strlen (body));
}

static void
handler (SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  SoupMessageHeaders *in = soup_server_message_get_request_headers (msg);
  SoupMessageHeaders *out = soup_server_message_get_response_headers (msg);
  const char *auth = soup_message_headers_get_one (in, "Authorization");

  g_free (last_authorization);
  last_authorization = g_strdup (auth);
  source_hits++;
  g_free (last_source);
  last_source = g_strdup (query ? g_hash_table_lookup (query, "source") : NULL);

  if (!strcmp (path, "/api/v1/catalog"))
    {
      const char *inm = soup_message_headers_get_one (in, "If-None-Match");
      g_autofree char *cc = g_strdup_printf ("%s, max-age=%d", auth ? "private" : "public", catalog_max_age);

      hits++;
      g_free (last_if_none_match);
      last_if_none_match = g_strdup (inm);
      soup_message_headers_replace (out, "ETag", catalog_etag);
      soup_message_headers_replace (out, "Cache-Control", cc);
      if (inm && !strcmp (inm, catalog_etag))
        {
          soup_server_message_set_status (msg, 304, NULL);
          return;
        }
      if (auth)
        {
          g_autofree char *body = g_strdup_printf ("{\"data\":{\"who\":\"%s\"}}", auth);

          respond_json (msg, 200, body);
          return;
        }
      respond_json (msg, 200, catalog_body);
      return;
    }
  if (!strcmp (path, "/api/v1/apps"))
    {
      const char *cursor = query ? g_hash_table_lookup (query, "cursor") : NULL;

      if (!cursor)
        respond_json (msg, 200, "{\"data\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"next_cursor\":\"eyJvIjoyfQ\","
                                "\"meta\":{\"total\":3}}");
      else if (!strcmp (cursor, "eyJvIjoyfQ"))
        respond_json (msg, 200, "{\"data\":[{\"id\":\"c\"}],\"next_cursor\":null}");
      else
        respond_json (msg, 422, "{\"error\":{\"code\":\"invalid_cursor\",\"message\":\"That page of the list is "
                                "no longer there.\"}}");
      return;
    }
  if (!strcmp (path, "/api/v1/missing"))
    {
      respond_json (msg, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"There is no such app.\"}}");
      return;
    }
  if (!strcmp (path, "/api/v1/busy"))
    {
      soup_message_headers_replace (out, "Retry-After", "42");
      respond_json (msg, 429, "{\"error\":{\"code\":\"rate_limited\",\"message\":\"Too many requests.\","
                              "\"retry_after\":42}}");
      return;
    }
  if (!strcmp (path, "/api/v1/busy-header-only"))
    {
      soup_message_headers_replace (out, "Retry-After", "7");
      respond_json (msg, 429, "{\"error\":{\"code\":\"rate_limited\",\"message\":\"Too many requests.\"}}");
      return;
    }
  if (!strcmp (path, "/api/v1/invalid"))
    {
      respond_json (msg, 422, "{\"error\":{\"code\":\"validation_failed\",\"message\":\"The email field must be a "
                              "valid email address.\",\"fields\":{\"email\":[\"The email field must be a valid "
                              "email address.\"],\"password\":[\"Too short.\",\"Too plain.\"]}}}");
      return;
    }
  if (!strcmp (path, "/api/v1/code"))
    {
      respond_json (msg, 422, "{\"error\":{\"code\":\"code_invalid\",\"message\":\"That code is not right.\","
                              "\"attempts_left\":3}}");
      return;
    }
  if (!strcmp (path, "/api/v1/review"))
    {
      respond_json (msg, 403, "{\"error\":{\"code\":\"review_not_allowed\",\"message\":\"Install the app first.\","
                              "\"reason\":\"not_installed\"}}");
      return;
    }
  if (!strcmp (path, "/api/v1/me"))
    {
      if (auth)
        respond_json (msg, 401, "{\"error\":{\"code\":\"unauthenticated\",\"message\":\"Sign in again.\"}}");
      else
        respond_json (msg, 200, "{\"data\":{}}");
      return;
    }
  if (!strcmp (path, "/api/v1/proxy"))
    {
      const char *html = "<html><body>Bad Gateway</body></html>";

      soup_server_message_set_status (msg, 502, NULL);
      soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_STATIC, html, strlen (html));
      return;
    }
  if (!strcmp (path, "/api/v1/garbage"))
    {
      const char *text = "{not json";

      soup_server_message_set_status (msg, 200, NULL);
      soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, text, strlen (text));
      return;
    }
  if (!strcmp (path, "/api/v1/echo"))
    {
      SoupMessageBody *body = soup_server_message_get_request_body (msg);
      g_autoptr (GBytes) bytes = soup_message_body_flatten (body);
      const char *type = soup_message_headers_get_content_type (in, NULL);

      g_free (last_body);
      last_body = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
      if (g_strcmp0 (type, "application/json"))
        respond_json (msg, 415, "{\"error\":{\"code\":\"bad\",\"message\":\"not JSON\"}}");
      else if (!strcmp (soup_server_message_get_method (msg), "DELETE"))
        soup_server_message_set_status (msg, 204, NULL);
      else
        respond_json (msg, 201, "{\"data\":{\"ok\":true}}");
      return;
    }
  /* the catalogue's other lists and an app's own pages, for the source: an empty answer */
  if (g_str_has_prefix (path, "/api/v1/featured") || g_str_has_prefix (path, "/api/v1/browse/") ||
      g_str_has_prefix (path, "/api/v1/categories") || g_str_has_prefix (path, "/api/v1/collections/") ||
      g_str_has_prefix (path, "/api/v1/search") || g_str_has_prefix (path, "/api/v1/apps/") ||
      !strcmp (path, "/api/v1/config") || !strcmp (path, "/api/v1/me/apps"))
    {
      respond_json (msg, 200, "{\"data\":{}}");
      return;
    }
  respond_json (msg, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"Unknown path.\"}}");
}

/* ---- a request, waited for ------------------------------------------------------------------------------------- */

typedef struct {
  int          calls;
  SwApiResult  results[2];
  char        *data[2];      /* the data member as JSON text */
  SwApiError   error;
  gboolean     failed;
  gboolean     done;
} Outcome;

static void
outcome_clear (Outcome *o)
{
  for (int i = 0; i < 2; i++)
    g_free (o->data[i]);
  sw_api_error_clear (&o->error);
  memset (o, 0, sizeof *o);
}

static void
on_answer (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Outcome *o = data;
  int i = MIN (o->calls, 1);

  o->calls++;
  if (error)
    {
      o->failed = TRUE;
      o->error = *error;
      o->error.code = g_strdup (error->code);
      o->error.message = g_strdup (error->message);
      o->error.reason = g_strdup (error->reason);
      o->error.fields = error->fields ? json_object_ref (error->fields) : NULL;
      o->done = TRUE;
      return;
    }
  o->results[i] = *result;
  o->results[i].root = NULL;
  o->results[i].data = NULL;
  o->results[i].meta = NULL;
  o->results[i].next_cursor = NULL;
  o->data[i] = result->data ? json_to_string (result->data, FALSE) : NULL;
  if (result->next_cursor)
    o->results[i].next_cursor = g_intern_string (result->next_cursor);
  /* a stale copy is followed by the server's answer */
  if (!result->stale)
    o->done = TRUE;
}

static void
wait_for (Outcome *o)
{
  gint64 until = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;

  while (!o->done && g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (o->done);
}

static void
get (SwApi *api, const char *path, SwApiFlags flags, Outcome *o)
{
  memset (o, 0, sizeof *o);
  sw_api_get (api, path, flags, NULL, on_answer, o);
  wait_for (o);
}

static void
spin (int ms)
{
  gint64 until = g_get_monotonic_time () + ms * 1000;

  while (g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, FALSE);
}

static char *
temp_dir (void)
{
  return g_dir_make_tmp ("sw-test-XXXXXX", NULL);
}

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *child = g_build_filename (path, name, NULL);

      if (g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_tree (child);
      else
        g_unlink (child);
    }
  g_rmdir (path);
}

static void
reset_catalog (const char *etag, const char *body, int max_age)
{
  g_free (catalog_etag);
  g_free (catalog_body);
  catalog_etag = g_strdup (etag);
  catalog_body = g_strdup (body);
  catalog_max_age = max_age;
  hits = 0;
  g_clear_pointer (&last_if_none_match, g_free);
}

/* ---- the cache ------------------------------------------------------------------------------------------------- */

static void
test_fresh_within_max_age (void)
{
  g_autofree char *dir = temp_dir ();
  g_autoptr (SwApi) api = sw_api_new (base_url, dir);
  Outcome o;

  reset_catalog ("W/\"one\"", "{\"data\":{\"n\":1}}", 60);
  get (api, "/catalog", SW_API_CACHED, &o);
  g_assert_false (o.failed);
  g_assert_cmpint (o.results[0].status, ==, 200);
  g_assert_false (o.results[0].from_cache);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":1}");
  g_assert_cmpint (hits, ==, 1);
  outcome_clear (&o);

  /* within max-age: the kept copy, nothing asked */
  get (api, "/catalog", SW_API_CACHED | SW_API_STALE_FIRST, &o);
  g_assert_cmpint (o.calls, ==, 1);
  g_assert_true (o.results[0].from_cache);
  g_assert_false (o.results[0].stale);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":1}");
  g_assert_cmpint (hits, ==, 1);
  outcome_clear (&o);

  /* a pull to refresh asks even so: 304, the copy stands */
  get (api, "/catalog", SW_API_CACHED | SW_API_REVALIDATE, &o);
  g_assert_cmpint (hits, ==, 2);
  g_assert_cmpstr (last_if_none_match, ==, "W/\"one\"");
  g_assert_cmpint (o.results[0].status, ==, 304);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":1}");
  outcome_clear (&o);
  remove_tree (dir);
}

static void
test_stale_then_304 (void)
{
  g_autofree char *dir = temp_dir ();
  g_autoptr (SwApi) api = sw_api_new (base_url, dir);
  Outcome o;

  reset_catalog ("W/\"one\"", "{\"data\":{\"n\":1}}", 0);
  get (api, "/catalog", SW_API_CACHED, &o);
  g_assert_null (last_if_none_match);
  outcome_clear (&o);

  /* max-age 0: the stale copy at once, then the server confirms it */
  get (api, "/catalog", SW_API_CACHED | SW_API_STALE_FIRST, &o);
  g_assert_cmpint (o.calls, ==, 2);
  g_assert_true (o.results[0].stale);
  g_assert_true (o.results[0].from_cache);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":1}");
  g_assert_cmpint (o.results[1].status, ==, 304);
  g_assert_true (o.results[1].not_modified);
  g_assert_cmpstr (o.data[1], ==, "{\"n\":1}");
  g_assert_cmpstr (last_if_none_match, ==, "W/\"one\"");
  outcome_clear (&o);

  /* the catalog changed: the stale copy, then the new one */
  g_free (catalog_etag);
  catalog_etag = g_strdup ("W/\"two\"");
  g_free (catalog_body);
  catalog_body = g_strdup ("{\"data\":{\"n\":2}}");
  get (api, "/catalog", SW_API_CACHED | SW_API_STALE_FIRST, &o);
  g_assert_cmpint (o.calls, ==, 2);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":1}");
  g_assert_cmpint (o.results[1].status, ==, 200);
  g_assert_false (o.results[1].not_modified);
  g_assert_cmpstr (o.data[1], ==, "{\"n\":2}");
  outcome_clear (&o);

  /* without STALE_FIRST only the answer comes */
  get (api, "/catalog", SW_API_CACHED, &o);
  g_assert_cmpint (o.calls, ==, 1);
  g_assert_cmpint (o.results[0].status, ==, 304);
  g_assert_cmpstr (o.data[0], ==, "{\"n\":2}");
  outcome_clear (&o);
  remove_tree (dir);
}

static void
test_disk_cache (void)
{
  g_autofree char *dir = temp_dir ();
  Outcome o;

  reset_catalog ("W/\"disk\"", "{\"data\":{\"kept\":true}}", 0);
  {
    g_autoptr (SwApi) api = sw_api_new (base_url, dir);

    get (api, "/catalog", SW_API_CACHED, &o);
    outcome_clear (&o);
    /* the copy is written in the background */
    spin (200);
  }
  {
    /* a new start: the copy from disk at once, then revalidated */
    g_autoptr (SwApi) api = sw_api_new (base_url, dir);

    get (api, "/catalog", SW_API_CACHED | SW_API_STALE_FIRST, &o);
    g_assert_cmpint (o.calls, ==, 2);
    g_assert_true (o.results[0].from_cache);
    g_assert_cmpstr (o.data[0], ==, "{\"kept\":true}");
    g_assert_cmpstr (last_if_none_match, ==, "W/\"disk\"");
    g_assert_true (o.results[1].not_modified);
    outcome_clear (&o);
  }
  {
    /* the server cannot be reached: the old copy is still handed over, then the error */
    g_autoptr (SwApi) api = sw_api_new ("http://127.0.0.1:9", dir);

    get (api, "/catalog", SW_API_CACHED | SW_API_STALE_FIRST, &o);
    /* a different server is a different cache key: nothing kept for it */
    g_assert_cmpint (o.calls, ==, 1);
    g_assert_true (o.failed);
    g_assert_cmpstr (o.error.code, ==, "network");
    outcome_clear (&o);
  }
  remove_tree (dir);
}

static void
test_token_kept_apart (void)
{
  g_autofree char *dir = temp_dir ();
  g_autoptr (SwApi) api = sw_api_new (base_url, dir);
  Outcome o;

  reset_catalog ("W/\"t\"", "{\"data\":{\"who\":\"nobody\"}}", 60);
  get (api, "/catalog", SW_API_CACHED, &o);
  g_assert_cmpstr (o.data[0], ==, "{\"who\":\"nobody\"}");
  g_assert_null (last_authorization);
  outcome_clear (&o);

  sw_api_set_token (api, "secret-1");
  get (api, "/catalog", SW_API_CACHED, &o);
  /* not the anonymous copy */
  g_assert_false (o.results[0].from_cache);
  g_assert_cmpstr (o.data[0], ==, "{\"who\":\"Bearer secret-1\"}");
  outcome_clear (&o);

  /* NO_TOKEN leaves it out */
  get (api, "/catalog", SW_API_CACHED | SW_API_NO_TOKEN, &o);
  g_assert_true (o.results[0].from_cache);
  g_assert_cmpstr (o.data[0], ==, "{\"who\":\"nobody\"}");
  outcome_clear (&o);

  sw_api_set_token (api, NULL);
  get (api, "/catalog", SW_API_CACHED, &o);
  g_assert_true (o.results[0].from_cache);
  g_assert_cmpstr (o.data[0], ==, "{\"who\":\"nobody\"}");
  outcome_clear (&o);
  remove_tree (dir);
}

/* ---- lists and cursors ----------------------------------------------------------------------------------------- */

static void
test_cursors (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  g_autofree char *next = NULL;
  g_autofree char *again = NULL;
  Outcome o;

  get (api, "/apps?type=app", SW_API_CACHED, &o);
  g_assert_cmpstr (o.data[0], ==, "[{\"id\":\"a\"},{\"id\":\"b\"}]");
  g_assert_cmpstr (o.results[0].next_cursor, ==, "eyJvIjoyfQ");
  next = sw_api_path_with_cursor ("/apps?type=app", o.results[0].next_cursor);
  g_assert_cmpstr (next, ==, "/apps?type=app&cursor=eyJvIjoyfQ");
  outcome_clear (&o);

  get (api, next, SW_API_NONE, &o);
  g_assert_cmpstr (o.data[0], ==, "[{\"id\":\"c\"}]");
  g_assert_null (o.results[0].next_cursor);
  outcome_clear (&o);

  again = sw_api_path_with_cursor ("/apps", "bogus");
  g_assert_cmpstr (again, ==, "/apps?cursor=bogus");
  get (api, again, SW_API_NONE, &o);
  g_assert_true (o.failed);
  g_assert_cmpint (o.error.status, ==, 422);
  g_assert_cmpstr (o.error.code, ==, "invalid_cursor");
  outcome_clear (&o);

  {
    g_autofree char *plain = sw_api_path_with_cursor ("/x", NULL);

    g_assert_cmpstr (plain, ==, "/x");
  }
}

/* ---- the source ------------------------------------------------------------------------------------------------ */

static void
on_catalog_changed (SwApi *api, int *count)
{
  (*count)++;
}

/* the source parameter that reached the server for a catalog read of `path`, or NULL; it must have been asked (no
 * kept copy answered) */
static const char *
source_sent (SwApi *api, const char *path, SwApiFlags flags)
{
  Outcome o;

  g_clear_pointer (&last_source, g_free);
  source_hits = 0;
  get (api, path, flags, &o);
  g_assert_false (o.failed);
  g_assert_cmpint (source_hits, ==, 1);
  outcome_clear (&o);
  return last_source;
}

static void
test_source (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  static const char *const lists[] = { "/featured", "/browse/apps", "/browse/games", "/categories?type=apps",
                                       "/apps?type=app&sort=popular", "/apps?category=games", "/collections/new",
                                       "/collections/new/apps", "/search?q=chess&type=game", "/search/suggest?q=ch" };
  static const char *const others[] = { "/apps/org.gnome.Chess", "/apps/org.gnome.Chess/reviews?sort=newest",
                                        "/config", "/me/apps", "/featuredx", "/collections/new/apps/more" };
  g_autofree char *next = sw_api_path_with_cursor ("/apps?type=app", "eyJvIjoyfQ");
  int changed = 0;
  Outcome o;

  g_signal_connect (api, "catalog-changed", G_CALLBACK (on_catalog_changed), &changed);
  /* every app: the paths as they are */
  g_assert_null (sw_api_get_source (api));
  for (guint i = 0; i < G_N_ELEMENTS (lists); i++)
    g_assert_null (source_sent (api, lists[i], SW_API_CACHED));

  sw_api_set_source (api, "vibeos");
  g_assert_cmpint (changed, ==, 1);
  sw_api_set_source (api, "vibeos");
  g_assert_cmpint (changed, ==, 1);
  g_assert_cmpstr (sw_api_get_source (api), ==, "vibeos");
  /* the lists carry it, and what was kept for them without it does not answer for them */
  for (guint i = 0; i < G_N_ELEMENTS (lists); i++)
    g_assert_cmpstr (source_sent (api, lists[i], SW_API_CACHED), ==, "vibeos");
  for (guint i = 0; i < G_N_ELEMENTS (others); i++)
    g_assert_null (source_sent (api, others[i], SW_API_CACHED));
  /* a list's next page keeps it */
  g_assert_cmpstr (source_sent (api, next, SW_API_NONE), ==, "vibeos");
  get (api, next, SW_API_NONE, &o);
  g_assert_cmpstr (o.data[0], ==, "[{\"id\":\"c\"}]");
  outcome_clear (&o);
  /* never on a write */
  g_clear_pointer (&last_source, g_free);
  memset (&o, 0, sizeof o);
  sw_api_send (api, "POST", "/echo", NULL, SW_API_NONE, NULL, on_answer, &o);
  wait_for (&o);
  g_assert_null (last_source);
  outcome_clear (&o);

  /* back to every app, which an empty source also means */
  sw_api_set_source (api, NULL);
  g_assert_cmpint (changed, ==, 2);
  sw_api_set_source (api, "");
  g_assert_cmpint (changed, ==, 2);
  g_assert_null (source_sent (api, "/apps?category=games", SW_API_REVALIDATE | SW_API_CACHED));
}

/* ---- errors ---------------------------------------------------------------------------------------------------- */

static void
test_errors (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  g_autofree char *text = NULL;
  Outcome o;

  get (api, "/missing", SW_API_CACHED, &o);
  g_assert_true (o.failed);
  g_assert_cmpint (o.error.kind, ==, SW_API_ERROR_HTTP);
  g_assert_cmpint (o.error.status, ==, 404);
  g_assert_cmpstr (o.error.code, ==, "not_found");
  g_assert_cmpstr (o.error.message, ==, "There is no such app.");
  outcome_clear (&o);

  get (api, "/busy", SW_API_NONE, &o);
  g_assert_cmpstr (o.error.code, ==, "rate_limited");
  g_assert_cmpint (o.error.retry_after, ==, 42);
  text = sw_api_error_describe (&o.error);
  g_assert_cmpstr (text, ==, "Too many requests. Try again in 42 seconds.");
  g_clear_pointer (&text, g_free);
  outcome_clear (&o);

  /* only the header names the wait */
  get (api, "/busy-header-only", SW_API_NONE, &o);
  g_assert_cmpint (o.error.retry_after, ==, 7);
  outcome_clear (&o);

  get (api, "/invalid", SW_API_NONE, &o);
  g_assert_cmpstr (o.error.code, ==, "validation_failed");
  g_assert_cmpstr (sw_api_error_field (&o.error, "email"), ==, "The email field must be a valid email address.");
  g_assert_cmpstr (sw_api_error_field (&o.error, "password"), ==, "Too short.");
  g_assert_null (sw_api_error_field (&o.error, "name"));
  outcome_clear (&o);

  get (api, "/code", SW_API_NONE, &o);
  g_assert_cmpstr (o.error.code, ==, "code_invalid");
  g_assert_cmpint (o.error.attempts_left, ==, 3);
  outcome_clear (&o);

  get (api, "/review", SW_API_NONE, &o);
  g_assert_cmpint (o.error.status, ==, 403);
  g_assert_cmpstr (o.error.reason, ==, "not_installed");
  g_assert_cmpstr (o.error.message, ==, "Install the app first.");
  outcome_clear (&o);

  /* a proxy's page in place of the API's error */
  get (api, "/proxy", SW_API_CACHED, &o);
  g_assert_cmpint (o.error.status, ==, 502);
  g_assert_cmpstr (o.error.code, ==, "server_error");
  g_assert_cmpstr (o.error.message, ==, "The store is not working right now (error 502). Try again later.");
  outcome_clear (&o);

  get (api, "/garbage", SW_API_NONE, &o);
  g_assert_cmpint (o.error.kind, ==, SW_API_ERROR_PARSE);
  outcome_clear (&o);
}

static void
test_network_error (void)
{
  /* port 9 (discard) is closed here */
  g_autoptr (SwApi) api = sw_api_new ("http://127.0.0.1:9/", NULL);
  g_autofree char *text = NULL;
  Outcome o;

  g_assert_cmpstr (sw_api_get_base_url (api), ==, "http://127.0.0.1:9");
  get (api, "/featured", SW_API_CACHED | SW_API_STALE_FIRST, &o);
  g_assert_true (o.failed);
  g_assert_cmpint (o.error.kind, ==, SW_API_ERROR_NETWORK);
  g_assert_cmpint (o.error.status, ==, 0);
  text = sw_api_error_describe (&o.error);
  g_assert_cmpstr (text, ==, "Software cannot reach the store. Check the internet connection and try again.");
  outcome_clear (&o);
}

static int unauthorized;

static void
on_unauthorized (SwApi *api, gpointer data)
{
  unauthorized++;
}

static void
test_unauthorized (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  Outcome o;

  g_signal_connect (api, "unauthorized", G_CALLBACK (on_unauthorized), NULL);
  unauthorized = 0;
  get (api, "/me", SW_API_NONE, &o);
  g_assert_false (o.failed);
  g_assert_cmpint (unauthorized, ==, 0);
  outcome_clear (&o);

  sw_api_set_token (api, "revoked");
  get (api, "/me", SW_API_NONE, &o);
  g_assert_true (o.failed);
  g_assert_cmpstr (o.error.code, ==, "unauthenticated");
  g_assert_cmpint (unauthorized, ==, 1);
  outcome_clear (&o);

  /* signed out while the request was out: no second sign-out */
  memset (&o, 0, sizeof o);
  sw_api_get (api, "/me", SW_API_NONE, NULL, on_answer, &o);
  sw_api_set_token (api, NULL);
  wait_for (&o);
  g_assert_cmpint (unauthorized, ==, 1);
  outcome_clear (&o);
}

static void
on_never (SwApiResult *result, const SwApiError *error, gpointer data)
{
  g_assert_not_reached ();
}

static void
test_cancel (void)
{
  g_autofree char *dir = temp_dir ();
  g_autoptr (SwApi) api = sw_api_new (base_url, dir);
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  Outcome o;

  reset_catalog ("W/\"c\"", "{\"data\":{}}", 60);
  sw_api_get (api, "/catalog", SW_API_CACHED, cancellable, on_never, NULL);
  g_cancellable_cancel (cancellable);
  spin (300);

  /* cancelled while a kept copy was on its way */
  get (api, "/catalog", SW_API_CACHED, &o);
  outcome_clear (&o);
  g_object_unref (cancellable);
  cancellable = g_cancellable_new ();
  sw_api_get (api, "/catalog", SW_API_CACHED, cancellable, on_never, NULL);
  g_cancellable_cancel (cancellable);
  sw_api_send (api, "POST", "/echo", NULL, SW_API_NONE, cancellable, on_never, NULL);
  spin (300);
  remove_tree (dir);
}

static void
test_send (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  Outcome o;

  json_parser_load_from_data (parser, "{\"rating\":5,\"text\":\"Grüße\"}", -1, NULL);
  sw_api_set_token (api, "tok");
  memset (&o, 0, sizeof o);
  sw_api_send (api, "PUT", "/echo", json_parser_get_root (parser), SW_API_NONE, NULL, on_answer, &o);
  wait_for (&o);
  g_assert_false (o.failed);
  g_assert_cmpint (o.results[0].status, ==, 201);
  g_assert_cmpstr (last_body, ==, "{\"rating\":5,\"text\":\"Grüße\"}");
  g_assert_cmpstr (last_authorization, ==, "Bearer tok");
  outcome_clear (&o);

  memset (&o, 0, sizeof o);
  sw_api_send (api, "DELETE", "/echo", json_parser_get_root (parser), SW_API_NO_TOKEN, NULL, on_answer, &o);
  wait_for (&o);
  g_assert_cmpint (o.results[0].status, ==, 204);
  g_assert_null (o.data[0]);
  g_assert_null (last_authorization);
  outcome_clear (&o);
}

/* ---- parsing without a server ---------------------------------------------------------------------------------- */

static void
test_parse (void)
{
  SwApiResult result;
  SwApiError error;
  static const char text_429[] = "{\"error\":{\"code\":\"resend_cooldown\",\"message\":\"Wait a little.\","
                                 "\"retry_after\":150}}";
  g_autoptr (GBytes) body = g_bytes_new_static (text_429, strlen (text_429));
  g_autofree char *text = NULL;

  g_assert_false (sw_api_parse_response (429, NULL, body, &result, &error));
  g_assert_cmpint (error.retry_after, ==, 150);
  text = sw_api_error_describe (&error);
  g_assert_cmpstr (text, ==, "Wait a little. Try again in 3 minutes.");
  sw_api_error_clear (&error);

  g_assert_true (sw_api_parse_response (204, NULL, NULL, &result, &error));
  g_assert_null (result.data);
  sw_api_result_clear (&result);

  /* an empty 500 */
  g_assert_false (sw_api_parse_response (500, NULL, NULL, &result, &error));
  g_assert_cmpstr (error.code, ==, "server_error");
  sw_api_error_clear (&error);
}

static void
test_json_helpers (void)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *app;
  g_autofree char *size = NULL, *date = NULL, *rating = NULL, *count = NULL, *mask = NULL, *digits = NULL;
  g_autofree char *one = NULL, *none = NULL;

  g_assert_true (json_parser_load_from_data (parser,
    "{\"name\":\"Chess\",\"rating\":4.55,\"count\":3,\"nothing\":null,\"flag\":true,"
    "\"icon\":{\"96\":\"https://x/96.webp\",\"192\":\"https://x/192.webp\"},"
    "\"packages\":[{\"arch\":\"all\",\"size\":10},{\"arch\":\"arm64\",\"size\":20}]}", -1, NULL));
  app = json_node_get_object (json_parser_get_root (parser));
  g_assert_cmpstr (sw_json_string (app, "name"), ==, "Chess");
  g_assert_null (sw_json_string (app, "nothing"));
  g_assert_null (sw_json_string (app, "count"));
  g_assert_null (sw_json_string (NULL, "name"));
  g_assert_cmpint (sw_json_int (app, "count", -1), ==, 3);
  g_assert_cmpint (sw_json_int (app, "nothing", -1), ==, -1);
  g_assert_cmpfloat (sw_json_double (app, "rating"), ==, 4.55);
  g_assert_true (isnan (sw_json_double (app, "nothing")));
  g_assert_true (sw_json_bool (app, "flag", FALSE));
  g_assert_cmpstr (sw_app_icon_url (app, 64), ==, "https://x/96.webp");
  g_assert_cmpstr (sw_app_icon_url (app, 288), ==, "https://x/192.webp");
  g_assert_cmpint (sw_json_int (sw_app_package_for_arch (app, "arm64"), "size", 0), ==, 20);
  g_assert_cmpint (sw_json_int (sw_app_package_for_arch (app, "riscv64"), "size", 0), ==, 10);

  size = sw_format_size (5600000);
  /* a no-break space keeps the number and its unit on one line */
  g_assert_cmpstr (size, ==, "5.6\u00a0MB");
  /* in the user's language and its region's order: British English here */
  g_setenv ("LANG", "en_GB.UTF-8", TRUE);
  g_unsetenv ("LANGUAGE");
  g_unsetenv ("LC_ALL");
  g_unsetenv ("LC_MESSAGES");
  g_unsetenv ("LC_TIME");
  date = sw_format_date ("2025-03-14T09:30:00Z");
  g_assert_cmpstr (date, ==, "14 Mar 2025");
  g_assert_null (sw_format_date ("yesterday"));
  rating = sw_format_rating (4.55);
  g_assert_true (!strcmp (rating, "4.5") || !strcmp (rating, "4.6"));
  count = sw_format_ratings_count (132);
  g_assert_cmpstr (count, ==, "132 ratings");
  one = sw_format_ratings_count (1);
  g_assert_cmpstr (one, ==, "1 rating");
  none = sw_format_ratings_count (0);
  g_assert_cmpstr (none, ==, "No ratings yet");
  g_assert_cmpint (sw_text_length ("  Grüße  "), ==, 5);
  mask = sw_mask_email ("anna@example.org");
  g_assert_cmpstr (mask, ==, "a•••@example.org");
  digits = sw_digits_only ("12 34-5678", 6);
  g_assert_cmpstr (digits, ==, "123456");
}

/* ---- dpkg ------------------------------------------------------------------------------------------------------ */

static const char status_text[] =
  "Package: gnome-chess\n"
  "Status: install ok installed\n"
  "Priority: optional\n"
  "Version: 1:48.0-1\n"
  "Description: simple chess game\n"
  " A long description\n"
  " Version: not a field\n"
  "\n"
  "Package: removed-thing\n"
  "Status: deinstall ok config-files\n"
  "Version: 2.0\n"
  "\n"
  "Package: half\n"
  "Status: install reinstreq half-installed\n"
  "Version: 3\n"
  "\n"
  "Package: libfoo1\n"
  "Architecture: arm64\n"
  "Status: install ok installed\n"
  "Version: 1.2-3\n"
  "\n"
  "Package: libfoo1\n"
  "Architecture: armhf\n"
  "Status: install ok installed\n"
  "Version: 1.2-3\n"
  "\n"
  "Package: last-one\n"
  "Version: 0.9\n"
  "Status: hold ok installed";

static void
test_dpkg (void)
{
  g_autoptr (GHashTable) installed = sw_dpkg_parse_status (status_text, strlen (status_text));

  g_assert_cmpuint (g_hash_table_size (installed), ==, 3);
  g_assert_cmpstr (g_hash_table_lookup (installed, "gnome-chess"), ==, "1:48.0-1");
  g_assert_cmpstr (g_hash_table_lookup (installed, "libfoo1"), ==, "1.2-3");
  g_assert_cmpstr (g_hash_table_lookup (installed, "last-one"), ==, "0.9");
  g_assert_false (g_hash_table_contains (installed, "removed-thing"));
  g_assert_false (g_hash_table_contains (installed, "half"));
}

static void
on_dpkg_read (GObject *source, GAsyncResult *res, gpointer data)
{
  GHashTable **out = data;

  *out = sw_dpkg_read_finish (res, NULL);
}

static void
test_dpkg_file (void)
{
  g_autofree char *dir = temp_dir ();
  g_autofree char *path = g_build_filename (dir, "status", NULL);
  GHashTable *installed = NULL;
  gint64 until = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;

  g_assert_true (g_file_set_contents (path, status_text, -1, NULL));
  sw_dpkg_read_async (path, NULL, on_dpkg_read, &installed);
  while (!installed && g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, TRUE);
  g_assert_nonnull (installed);
  g_assert_cmpuint (g_hash_table_size (installed), ==, 3);
  g_hash_table_unref (installed);
  remove_tree (dir);
}

/* ---- the device ------------------------------------------------------------------------------------------------ */

static void
test_device (void)
{
  g_autofree char *dir = temp_dir ();
  g_autofree char *first = sw_device_id_in (dir);
  g_autofree char *again = sw_device_id_in (dir);
  g_autofree char *path = g_build_filename (dir, "device-id", NULL);
  g_autofree char *model = sw_device_clean_model ("Xiaomi POCO X3 NFC\0\0", 20);
  g_autofree char *spaced = sw_device_clean_model ("  Samsung Galaxy S10e \n", 23);
  GStatBuf st;

  g_assert_true (g_uuid_string_is_valid (first));
  g_assert_cmpstr (first, ==, again);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_assert_cmpstr (model, ==, "Xiaomi POCO X3 NFC");
  g_assert_cmpstr (spaced, ==, "Samsung Galaxy S10e");

  /* a broken file is replaced */
  g_assert_true (g_file_set_contents (path, "not a uuid", -1, NULL));
  {
    g_autofree char *fresh = sw_device_id_in (dir);

    g_assert_true (g_uuid_string_is_valid (fresh));
    g_assert_cmpstr (fresh, !=, first);
  }
  g_assert_nonnull (sw_device_built_arch ());
  remove_tree (dir);
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  GSList *uris;

  g_test_init (&argc, &argv, NULL);
  server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, handler, NULL, NULL);
  if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
    g_error ("could not listen: %s", error->message);
  uris = soup_server_get_uris (server);
  base_url = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  g_test_add_func ("/api/fresh-within-max-age", test_fresh_within_max_age);
  g_test_add_func ("/api/stale-then-304", test_stale_then_304);
  g_test_add_func ("/api/disk-cache", test_disk_cache);
  g_test_add_func ("/api/token-kept-apart", test_token_kept_apart);
  g_test_add_func ("/api/cursors", test_cursors);
  g_test_add_func ("/api/source", test_source);
  g_test_add_func ("/api/errors", test_errors);
  g_test_add_func ("/api/network-error", test_network_error);
  g_test_add_func ("/api/unauthorized", test_unauthorized);
  g_test_add_func ("/api/cancel", test_cancel);
  g_test_add_func ("/api/send", test_send);
  g_test_add_func ("/api/parse", test_parse);
  g_test_add_func ("/json/helpers", test_json_helpers);
  g_test_add_func ("/dpkg/parse", test_dpkg);
  g_test_add_func ("/dpkg/file", test_dpkg_file);
  g_test_add_func ("/device/id", test_device);
  return g_test_run ();
}
