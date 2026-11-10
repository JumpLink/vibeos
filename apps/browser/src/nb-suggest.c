/* nb-suggest: see nb-suggest.h. The URLs and the parsing are SuggestionProvider.kt's. */
#include "nb-suggest.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

const char *const nb_suggest_ids[] = { "BAIDU", "BING", "BRAVE", "DUCK", "GOOGLE", "YAHOO", "HISTORY", "NONE", NULL };
const char *const nb_suggest_names[] = { "Baidu", "Bing", "Brave", "DuckDuckGo", "Google", "Yahoo", "History",
                                         "None", NULL };

void
nb_suggestion_free (NbSuggestion *s)
{
  g_free (s->title);
  g_free (s->url);
  g_free (s);
}

static NbSuggestion *
suggestion_new (const char *title, const char *url)
{
  NbSuggestion *s = g_new0 (NbSuggestion, 1);

  s->title = g_strdup (title);
  s->url = g_strdup (url);
  return s;
}

/* Locale.getDefault ().language, "en" without one */
static char *
language (void)
{
  const char *const *names = g_get_language_names ();
  const char *lang = names && names[0] && !g_str_equal (names[0], "C") ? names[0] : "en";
  char *out = g_strdup (lang);

  out[strcspn (out, "_.@")] = '\0';
  return *out ? out : (g_free (out), g_strdup ("en"));
}

static char *
query_url (const char *provider, const char *query)
{
  g_autofree char *q = g_uri_escape_string (query, NULL, TRUE);
  g_autofree char *lang = language ();

  if (g_str_equal (provider, "BAIDU"))
    return g_strdup_printf ("https://suggestion.baidu.com/su?ie=UTF-8&wd=%s&action=opensearch", q);
  if (g_str_equal (provider, "BING"))
    return g_strdup_printf ("https://api.bing.com/osjson.aspx?query=%s&language=%s", q, lang);
  if (g_str_equal (provider, "BRAVE"))
    return g_strdup_printf ("https://search.brave.com/api/suggest?q=%s", q);
  if (g_str_equal (provider, "DUCK"))
    return g_strdup_printf ("https://duckduckgo.com/ac/?q=%s", q);
  if (g_str_equal (provider, "YAHOO"))
    return g_strdup_printf ("https://search.yahoo.com/sugg/chrome?output=fxjson&command=%s", q);
  /* GOOGLE, and anything unknown */
  return g_strdup_printf ("https://www.google.com/complete/search?client=android&oe=utf8&ie=utf8&cp=4&xssi=t"
                          "&gs_pcrt=undefined&hl=%s&q=%s", lang, q);
}

static GPtrArray *
parse (const char *provider, const char *text, gsize len)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) nb_suggestion_free);
  g_autoptr (JsonParser) parser = json_parser_new ();
  const char *body = text;

  /* Google's XSSI guard */
  if (len >= 4 && strncmp (body, ")]}'", 4) == 0) {
    body += 4;
    len -= 4;
  }
  if (!json_parser_load_from_data (parser, body, len, NULL))
    return out;
  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY (root))
    return out;
  JsonArray *array = json_node_get_array (root);
  if (g_str_equal (provider, "DUCK")) {
    for (guint i = 0; i < json_array_get_length (array) && out->len < NB_SUGGEST_LIMIT; i++) {
      JsonNode *n = json_array_get_element (array, i);
      if (JSON_NODE_HOLDS_OBJECT (n)) {
        const char *phrase = json_object_get_string_member_with_default (json_node_get_object (n), "phrase", NULL);
        if (phrase)
          g_ptr_array_add (out, suggestion_new (phrase, NULL));
      }
    }
    return out;
  }
  /* [query, [suggestions], …] */
  if (json_array_get_length (array) < 2)
    return out;
  JsonNode *list = json_array_get_element (array, 1);
  if (!JSON_NODE_HOLDS_ARRAY (list))
    return out;
  JsonArray *items = json_node_get_array (list);
  for (guint i = 0; i < json_array_get_length (items) && out->len < NB_SUGGEST_LIMIT; i++) {
    JsonNode *n = json_array_get_element (items, i);
    if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
      g_ptr_array_add (out, suggestion_new (json_node_get_string (n), NULL));
  }
  return out;
}

typedef struct {
  char          *provider;
  GCancellable  *cancel;
  NbSuggestDone  done;
  gpointer       data;
} Fetch;

static void
fetch_free (Fetch *f)
{
  g_free (f->provider);
  g_clear_object (&f->cancel);
  g_free (f);
}

static SoupSession *
session (void)
{
  static SoupSession *s;

  if (!s)
    s = soup_session_new_with_options ("timeout", 10, NULL);
  return s;
}

static void
on_fetched (GObject *src, GAsyncResult *res, gpointer data)
{
  Fetch *f = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (g_cancellable_is_cancelled (f->cancel)) {
    fetch_free (f);
    return;
  }
  GPtrArray *out;
  if (bytes) {
    gsize len;
    const char *text = g_bytes_get_data (bytes, &len);
    out = parse (f->provider, text ? text : "", len);
  } else {
    out = g_ptr_array_new_with_free_func ((GDestroyNotify) nb_suggestion_free);
  }
  f->done (out, f->data);
  g_ptr_array_unref (out);
  fetch_free (f);
}

static gboolean
contains_fold (const char *hay, const char *needle_lower)
{
  g_autofree char *lower = g_utf8_strdown (hay ? hay : "", -1);
  return strstr (lower, needle_lower) != NULL;
}

void
nb_suggest_fetch (NbStore *store, const char *provider, const char *query, GCancellable *cancel,
                  NbSuggestDone done, gpointer data)
{
  g_autofree char *q = g_utf8_strdown (query, -1);

  g_strstrip (q);
  if (!*q || g_str_equal (provider, "NONE")) {
    g_autoptr (GPtrArray) none = g_ptr_array_new ();
    done (none, data);
    return;
  }
  if (g_str_equal (provider, "HISTORY")) {
    /* HistoryDao.search: title or URL LIKE %q%, newest first, LIMIT 5 */
    g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func ((GDestroyNotify) nb_suggestion_free);
    for (guint i = 0; i < store->history->len && out->len < NB_SUGGEST_LIMIT; i++) {
      NbEntry *e = store->history->pdata[i];
      if (contains_fold (e->title, q) || contains_fold (e->url, q))
        g_ptr_array_add (out, suggestion_new (e->title, e->url));
    }
    done (out, data);
    return;
  }
  g_autofree char *url = query_url (provider, q);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  if (!msg)
    return;
  soup_message_headers_append (soup_message_get_request_headers (msg), "Cache-Control",
                               "max-age=86400, max-stale=86400");
  soup_message_headers_append (soup_message_get_request_headers (msg), "Accept-Charset", "UTF-8");
  Fetch *f = g_new0 (Fetch, 1);
  f->provider = g_strdup (provider);
  f->cancel = cancel ? g_object_ref (cancel) : g_cancellable_new ();
  f->done = done;
  f->data = data;
  soup_session_send_and_read_async (session (), msg, G_PRIORITY_DEFAULT, f->cancel, on_fetched, f);
}
