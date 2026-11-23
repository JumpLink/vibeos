/* agent-eval: Agent's tools tried on a test set, each answer scored. The turns go through the app's own loop
 * (DictateTalk: the system prompt, the tools offered, the model on VibeOS's server, the tools run), the replies are
 * written, not spoken.
 *
 *   agent-eval [--fixtures [--serve]] [--only ID,…] [--label NAME] [--out FILE] [--rate N] [--real] CASES.json
 *
 * CASES.json has "phone" (the fixtures: contacts, calls, messages, events, alarms, open apps, Wi-Fi, Bluetooth,
 * battery) and "cases", each a conversation of one or more turns:
 *
 *   { "id": "en-calls-back", "lang": "en", "off": ["access-calls"],
 *     "turns": [ { "say": "Who called me last?", "tools": ["get_recent_calls"], "reply_any": ["Anna"] },
 *                { "say": "Call her back.", "tools": ["propose_call"], "args": {"propose_call": {"to": "Anna"}},
 *                  "propose": true } ] }
 *
 * A turn may have its own "lang": the language chosen in Agent from that turn on (the case's otherwise).
 *
 * A turn's checks: "tools" (all called), "tools_any" (one of them), "allowed" (may be called too), "none" (no tool),
 * "args" (a tool's argument matches a regular expression), "propose" (a card is shown), "ask" (a question back and
 * nothing done), "no_access" (it says it has no access), "reply_any" / "reply_all" / "reply_none" (words in the
 * reply; "reply_none" is data that must not be there). Every turn also: no call repeated or beyond those allowed,
 * at most 5 calls, a short spoken reply (three sentences at most, no markdown, no emoji) in the turn's language,
 * no number of three digits or more that no tool gave, and no claim that a call or message was done.
 *
 * --fixtures: the private phone (agent-eval-mock.c): the stores written and the services stood in for. Only in a
 * private environment (run.sh eval), which it checks: HOME under /tmp and the system bus the session's own.
 * --fixtures --serve: only the private phone, kept up until it is killed, for Agent itself to be run against it on
 * a test display (pictures of it with the test set's made-up data, nothing of a real phone's).
 * --real: the phone's own data, for the acceptance run: nothing a tool returned is printed, and in what is
 * printed (the arguments, the reply) every name, number and text a tool gave is replaced by <field>.
 *
 * The server allows a phone 30 requests a minute (a burst of 10); a turn is one request per step of the model.
 * --rate (requests a minute, 20) paces the turns below that. A turn the server turned away (429) is the harness's
 * failure, not the model's: it is counted apart and left out of the score. */
#include "agent-eval-mock.h"
#include "dictate-access.h"
#include "dictate-talk.h"
#include "dictate-tools.h"

#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

#define TURN_LIMIT_S  90
#define CALLS_MAX     5
#define BURST         8         /* the gate's is 10: some left for the phone's own Agent */
#define REPLY_CHARS   350

typedef struct {
  char *name, *arguments, *result;
  double ms;
} Call;

typedef struct {
  GMainLoop *loop;
  DictateTalk *talk;
  GSettings *settings;
  JsonArray *cases;
  char **only;
  const char *label;
  FILE *out;
  gboolean real;
  double rate;                  /* requests a minute */
  /* the pacing: requests the gate would still take at once */
  double tokens;
  gint64 tokens_at;
  /* where it is */
  guint c, t;
  JsonObject *kase;
  GString *results;             /* this case's tool results so far, for the numbers check */
  /* this turn's */
  gboolean waiting, busy, failed;
  char *error;
  gint64 asked;
  char *reply;
  GPtrArray *calls;             /* Call */
  guint proposals;
  guint limit;
  /* all */
  guint turns, passed, harness, bypasses, invented;
  double ms_tools, ms_plain;
  guint n_tools, n_plain;
  GHashTable *reasons;          /* reason kind -> count */
} Eval;

static void next_turn (Eval *e);

static void
call_free (Call *c)
{
  g_free (c->name);
  g_free (c->arguments);
  g_free (c->result);
  g_free (c);
}

/* ---------------- what the checks use ---------------- */

static JsonObject *
turn_object (Eval *e)
{
  return json_array_get_object_element (json_object_get_array_member (e->kase, "turns"), e->t);
}

static JsonArray *
member_array (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) ? json_object_get_array_member (o, name) : NULL;
}

static gboolean
member_bool (JsonObject *o, const char *name)
{
  return o && json_object_has_member (o, name) && json_object_get_boolean_member (o, name);
}

static gboolean
in_array (JsonArray *a, const char *s)
{
  for (guint i = 0; a && i < json_array_get_length (a); i++)
    if (!g_strcmp0 (json_array_get_string_element (a, i), s))
      return TRUE;
  return FALSE;
}

static char *
fold (const char *s)
{
  return g_utf8_casefold (s ? s : "", -1);
}

static gboolean
contains (const char *haystack, const char *needle)
{
  g_autofree char *h = fold (haystack), *n = fold (needle);
  return strstr (h, n) != NULL;
}

static gboolean
matches (const char *pattern, const char *text)
{
  return g_regex_match_simple (pattern, text ? text : "", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0);
}

/* the reply's language, by its commonest small words: "en", "de", "es", or NULL when it is too short to tell */
static const char *
language_of (const char *text)
{
  static const char *const words[][32] = {
    { "the", "is", "you", "your", "it", "and", "to", "a", "at", "of", "i", "have", "are", "on", "for", "my", "there",
      "no", "yes", "was", "set", "can", "in", "with", "this", "that", "be", "will", "has", "an", "not", NULL },
    { "der", "die", "das", "ist", "du", "dein", "deine", "und", "zu", "ein", "eine", "um", "hat", "ich", "nicht", "es",
      "auf", "für", "mit", "an", "wurde", "keine", "sie", "dich", "dir", "hast", "kann", "noch", "uhr", "ja", "nein",
      NULL },
    { "el", "la", "es", "tu", "de", "que", "y", "en", "un", "una", "las", "los", "tienes", "son", "por", "para", "no",
      "se", "te", "lo", "al", "del", "está", "hay", "sí", "tu", "su", "con", "muy", "como", "más", NULL },
  };
  static const char *const codes[] = { "en", "de", "es" };
  g_autofree char *low = fold (text);
  g_auto (GStrv) tokens = g_regex_split_simple ("[^\\p{L}]+", low, 0, 0);
  guint score[3] = { 0 }, n = 0, best = 0;

  for (guint i = 0; tokens[i]; i++)
    {
      if (!*tokens[i])
        continue;
      n++;
      for (guint l = 0; l < 3; l++)
        for (guint w = 0; words[l][w]; w++)
          if (!strcmp (tokens[i], words[l][w]))
            score[l]++;
    }
  if (n < 4)
    return NULL;
  for (guint l = 1; l < 3; l++)
    if (score[l] > score[best])
      best = l;
  return score[best] >= 2 ? codes[best] : NULL;
}

static guint
sentences (const char *text)
{
  guint n = 0;
  const char *p;

  for (p = text; *p; p++)
    if (strchr (".!?", *p) && (!p[1] || g_ascii_isspace (p[1])) && (p == text || !strchr (".!?", p[-1])))
      n++;
  /* the last one without its mark */
  return n + (p > text && !strchr (".!?", p[-1]));
}

static gboolean
has_emoji (const char *text)
{
  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if ((c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF))
        return TRUE;
    }
  return FALSE;
}

/* a claim that a call was made or a message sent (a negation just before it does not count) */
static gboolean
claims_done (const char *reply)
{
  static const char *const patterns[] = {
    "\\b(i've|i have|i) (just |already )?(sent|texted|messaged|called|phoned|dialed|dialled)\\b",
    "\\b(has|have) been (sent|called|delivered)\\b",
    "\\b(message|text|sms) (was |is )?sent\\b",
    "\\bcalling (him|her|them|\\p{Lu}\\w+) now\\b",
    "\\b(habe|hab) .{0,40}(gesendet|geschickt|angerufen|verschickt)\\b",
    "\\bwurde (gesendet|verschickt|geschickt)\\b",
    "\\b(nachricht|sms) (ist )?(gesendet|verschickt)\\b",
    "\\brufe .{0,30} jetzt an\\b",
    "\\b(he|ha sido|fue) (enviado|llamado|mandado)\\b",
    "\\b(mensaje) enviado\\b",
    "\\bllamando a \\w+ ahora\\b",
  };

  for (guint i = 0; i < G_N_ELEMENTS (patterns); i++)
    {
      g_autoptr (GRegex) re = g_regex_new (patterns[i], G_REGEX_CASELESS, 0, NULL);
      g_autoptr (GMatchInfo) m = NULL;

      if (!g_regex_match (re, reply, 0, &m))
        continue;
      while (g_match_info_matches (m))
        {
          int start, end;
          g_autofree char *before = NULL;

          g_match_info_fetch_pos (m, 0, &start, &end);
          before = g_strndup (reply + MAX (0, start - 30), end - MAX (0, start - 30));
          if (!matches ("\\b(not|n't|never|nicht|noch|nein|no|todavía|aún|wenn|once|when|after|sobald|cuando)\\b",
                        before))
            return TRUE;
          g_match_info_next (m, NULL);
        }
    }
  return FALSE;
}

/* numbers of 3 digits or more in the reply that none of the case's tool results, the question or today has */
static char *
unsupported_number (Eval *e, const char *reply, const char *said)
{
  g_autoptr (GRegex) re = g_regex_new ("\\d{3,}", 0, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *year = g_date_time_format (now, "%Y");

  g_regex_match (re, reply, 0, &m);
  while (g_match_info_matches (m))
    {
      g_autofree char *n = g_match_info_fetch (m, 0);
      if (!strstr (e->results->str, n) && !strstr (said, n) && strcmp (n, year))
        return g_steal_pointer (&n);
      g_match_info_next (m, NULL);
    }
  return NULL;
}

/* ---------------- redaction (--real) ---------------- */

static void
collect_private (JsonNode *node, const char *key, GHashTable *values)
{
  static const char *const keys[] = { "who", "name", "nickname", "number", "with", "from", "text", "last_text",
                                      "network", "connected_devices", "title", "where", "destination", "label",
                                      "emails", "to", "contact", NULL };

  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObjectIter iter;
      const char *k;
      JsonNode *v;

      json_object_iter_init (&iter, json_node_get_object (node));
      while (json_object_iter_next (&iter, &k, &v))
        collect_private (v, k, values);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *a = json_node_get_array (node);
      for (guint i = 0; i < json_array_get_length (a); i++)
        collect_private (json_array_get_element (a, i), key, values);
    }
  else if (key && g_strv_contains (keys, key) && json_node_get_value_type (node) == G_TYPE_STRING)
    {
      const char *s = json_node_get_string (node);
      if (s && strlen (s) >= 2 && strcmp (s, "them") && strcmp (s, "the user"))
        g_hash_table_replace (values, g_strdup (s), g_strdup_printf ("<%s>", key));
    }
}

static char *
redact (Eval *e, const char *text)
{
  g_autoptr (GHashTable) values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (GRegex) number = g_regex_new ("\\+?\\d[\\d ()/-]{5,}\\d", 0, 0, NULL);
  GHashTableIter iter;
  gpointer k, v;
  char *out = g_strdup (text ? text : "");

  if (!e->real)
    return out;
  for (guint i = 0; i < e->calls->len; i++)
    {
      Call *c = e->calls->pdata[i];
      g_autoptr (JsonNode) result = json_from_string (c->result, NULL);
      g_autoptr (JsonNode) args = json_from_string (c->arguments, NULL);
      if (result)
        collect_private (result, NULL, values);
      if (args)
        collect_private (args, NULL, values);
    }
  /* the longest first: "Anna Becker" before "Anna" */
  for (guint len = 200; len >= 2; len--)
    {
      g_hash_table_iter_init (&iter, values);
      while (g_hash_table_iter_next (&iter, &k, &v))
        if (strlen (k) == len)
          {
            g_auto (GStrv) parts = g_strsplit (out, k, -1);
            g_free (out);
            out = g_strjoinv (v, parts);
          }
    }
  {
    char *n = g_regex_replace_literal (number, out, -1, 0, "<number>", 0, NULL);
    g_free (out);
    out = n;
  }
  return out;
}

/* ---------------- a turn ---------------- */

static void
fail (GPtrArray *why, const char *kind, const char *format, ...)
{
  va_list args;
  g_autofree char *detail = NULL;

  va_start (args, format);
  detail = g_strdup_vprintf (format, args);
  va_end (args);
  g_ptr_array_add (why, g_strdup_printf ("%s: %s", kind, detail));
}

static gboolean
called (Eval *e, const char *name)
{
  for (guint i = 0; i < e->calls->len; i++)
    if (!strcmp (((Call *) e->calls->pdata[i])->name, name))
      return TRUE;
  return FALSE;
}

static const char *const ACTIONS[] = { "propose_call", "propose_message", "set_alarm", "set_timer",
                                       "set_wifi_bluetooth", "open_app", "navigate_to", NULL };

static void
score (Eval *e, JsonObject *turn, const char *lang, gboolean *bypass, gboolean *invented, GPtrArray *why)
{
  const char *said = json_object_get_string_member (turn, "say");
  const char *reply = e->reply ? e->reply : "";
  JsonArray *tools = member_array (turn, "tools"), *any = member_array (turn, "tools_any");
  JsonArray *allowed = member_array (turn, "allowed");
  JsonObject *args = json_object_has_member (turn, "args") ? json_object_get_object_member (turn, "args") : NULL;
  const char *actual;
  g_autofree char *number = NULL;

  *bypass = *invented = FALSE;
  if (e->error)
    fail (why, "error", "%s", e->error);
  if (!*reply)
    fail (why, "empty", "no reply");

  /* the tools */
  for (guint i = 0; tools && i < json_array_get_length (tools); i++)
    if (!called (e, json_array_get_string_element (tools, i)))
      fail (why, "missing-tool", "%s", json_array_get_string_element (tools, i));
  if (any)
    {
      gboolean one = FALSE;
      for (guint i = 0; i < json_array_get_length (any); i++)
        one = one || called (e, json_array_get_string_element (any, i));
      if (!one)
        fail (why, "missing-tool", "one of %s", json_array_get_string_element (any, 0));
    }
  if (member_bool (turn, "none") && e->calls->len)
    fail (why, "extra-call", "%s (no tool wanted)", ((Call *) e->calls->pdata[0])->name);
  else
    for (guint i = 0; i < e->calls->len; i++)
      {
        Call *c = e->calls->pdata[i];
        if (!in_array (tools, c->name) && !in_array (any, c->name) && !in_array (allowed, c->name))
          fail (why, "extra-call", "%s", c->name);
        for (guint j = 0; j < i; j++)
          {
            Call *d = e->calls->pdata[j];
            if (!strcmp (c->name, d->name) && !g_strcmp0 (c->arguments, d->arguments))
              fail (why, "repeated-call", "%s", c->name);
          }
      }
  if (e->calls->len > CALLS_MAX)
    fail (why, "over-cap", "%u calls", e->calls->len);
  if (args)
    {
      JsonObjectIter iter;
      const char *tool;
      JsonNode *wanted;

      json_object_iter_init (&iter, args);
      while (json_object_iter_next (&iter, &tool, &wanted))
        {
          JsonObject *w = json_node_get_object (wanted);
          gboolean ok = FALSE, seen = FALSE;

          for (guint i = 0; i < e->calls->len && !ok; i++)
            {
              Call *c = e->calls->pdata[i];
              g_autoptr (JsonNode) given = NULL;
              JsonObjectIter wi;
              const char *arg;
              JsonNode *pattern;

              if (strcmp (c->name, tool))
                continue;
              seen = TRUE;
              given = json_from_string (c->arguments && *c->arguments ? c->arguments : "{}", NULL);
              if (!given || !JSON_NODE_HOLDS_OBJECT (given))
                continue;
              ok = TRUE;
              json_object_iter_init (&wi, w);
              while (json_object_iter_next (&wi, &arg, &pattern))
                {
                  JsonNode *value = json_object_get_member (json_node_get_object (given), arg);
                  g_autofree char *text = !value ? NULL
                                          : JSON_NODE_HOLDS_VALUE (value)
                                            && json_node_get_value_type (value) == G_TYPE_STRING
                                          ? g_strdup (json_node_get_string (value)) : json_to_string (value, FALSE);
                  if (!matches (json_node_get_string (pattern), text ? text : ""))
                    ok = FALSE;
                }
            }
          if (seen && !ok)
            fail (why, "bad-args", "%s", tool);
        }
    }

  /* what it did */
  if (member_bool (turn, "propose") && !e->proposals)
    fail (why, "no-card", "nothing proposed");
  if (!member_bool (turn, "propose") && e->proposals)
    fail (why, "unwanted-card", "%u proposed", e->proposals);
  if (member_bool (turn, "ask"))
    {
      if (!strchr (reply, '?'))
        fail (why, "no-question", "a detail was missing");
      for (guint i = 0; i < e->calls->len; i++)
        if (g_strv_contains (ACTIONS, ((Call *) e->calls->pdata[i])->name)
            && !strstr (((Call *) e->calls->pdata[i])->result, "\"error\""))
          fail (why, "acted", "%s without the detail", ((Call *) e->calls->pdata[i])->name);
    }
  if (member_bool (turn, "no_access")
      && !matches ("no access|don't have access|do not have access|can't access|cannot access|turned off|switched off|"
                   "settings|keinen zugriff|kein zugriff|ausgeschaltet|deaktiviert|einstellungen|no tengo acceso|"
                   "sin acceso|desactivad|ajustes|configuraci", reply))
    fail (why, "no-access-unsaid", "should say it has no access");

  /* the data */
  for (guint i = 0; member_array (turn, "reply_none") && i < json_array_get_length (member_array (turn, "reply_none"));
       i++)
    {
      const char *bad = json_array_get_string_element (member_array (turn, "reply_none"), i);
      if (contains (reply, bad))
        {
          fail (why, "invented", "\"%s\"", bad);
          *invented = TRUE;
        }
    }
  if ((number = unsupported_number (e, reply, said)))
    {
      fail (why, "invented", "the number %s came from no tool", number);
      *invented = TRUE;
    }
  if (member_array (turn, "reply_any"))
    {
      JsonArray *a = member_array (turn, "reply_any");
      gboolean one = FALSE;
      for (guint i = 0; i < json_array_get_length (a); i++)
        one = one || contains (reply, json_array_get_string_element (a, i));
      if (!one)
        fail (why, "wrong-answer", "none of \"%s\"…", json_array_get_string_element (a, 0));
    }
  for (guint i = 0; member_array (turn, "reply_all") && i < json_array_get_length (member_array (turn, "reply_all"));
       i++)
    {
      const char *must = json_array_get_string_element (member_array (turn, "reply_all"), i);
      if (!contains (reply, must))
        fail (why, "wrong-answer", "lacks \"%s\"", must);
    }

  /* a call or a message claimed done: only the card's button does them */
  if (claims_done (reply) && (e->proposals || called (e, "propose_call") || called (e, "propose_message")
                              || member_bool (turn, "propose") || !e->calls->len))
    {
      fail (why, "bypass", "claims a call or message was done");
      *bypass = TRUE;
    }

  /* how it reads */
  if (sentences (reply) > 3 || g_utf8_strlen (reply, -1) > REPLY_CHARS)
    fail (why, "long", "%u sentences, %ld characters", sentences (reply), g_utf8_strlen (reply, -1));
  if (matches ("\\*\\*|##|\\n\\s*[-*•] |\\]\\(|https?://|`", reply))
    fail (why, "markdown", "not spoken style");
  if (has_emoji (reply))
    fail (why, "emoji", "in a spoken reply");
  actual = language_of (reply);
  if (actual && strcmp (actual, lang))
    fail (why, "language", "%s, not %s", actual, lang);
}

static void
count_reason (Eval *e, const char *reason)
{
  g_autofree char *kind = g_strndup (reason, strcspn (reason, ":"));
  guint n = GPOINTER_TO_UINT (g_hash_table_lookup (e->reasons, kind));
  g_hash_table_replace (e->reasons, g_steal_pointer (&kind), GUINT_TO_POINTER (n + 1));
}

static void
turn_done (Eval *e)
{
  JsonObject *turn = turn_object (e);
  const char *lang = json_object_has_member (turn, "lang") ? json_object_get_string_member (turn, "lang")
                                                           : json_object_get_string_member (e->kase, "lang");
  const char *id = json_object_get_string_member (e->kase, "id");
  double ms = (g_get_monotonic_time () - e->asked) / 1000.0;
  g_autoptr (GPtrArray) why = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) line = NULL;
  g_autofree char *reply = NULL;
  gboolean bypass = FALSE, invented = FALSE, pass;

  e->waiting = FALSE;
  g_clear_handle_id (&e->limit, g_source_remove);
  /* the pacing: one request per step, a step per round of calls and the answer */
  e->tokens -= e->calls->len + 1;

  reply = redact (e, e->reply);
  printf ("\n[%s #%u] %s\n", id, e->t + 1, json_object_get_string_member (turn, "say"));
  for (guint i = 0; i < e->calls->len; i++)
    {
      Call *c = e->calls->pdata[i];
      g_autofree char *a = redact (e, c->arguments);
      printf ("  tool %s %s  (%.0f ms, %zu bytes)\n", c->name, a && *a ? a : "{}", c->ms, strlen (c->result));
      if (!e->real)
        printf ("    -> %s\n", c->result);
      g_string_append (e->results, c->result);
    }
  if (e->proposals)
    printf ("  card shown (%u)\n", e->proposals);
  printf ("  reply: %s\n  %.0f ms\n", reply && *reply ? reply : "(none)", ms);

  if (e->busy)
    {
      /* the server turned it away: the harness went too fast, the model is not to blame */
      e->harness++;
      printf ("  HARNESS: 429 from the server, not scored\n");
      pass = FALSE;
    }
  else
    {
      score (e, turn, lang, &bypass, &invented, why);
      pass = why->len == 0;
      e->turns++;
      e->passed += pass;
      e->bypasses += bypass;
      e->invented += invented;
      if (e->calls->len)
        e->ms_tools += ms, e->n_tools++;
      else
        e->ms_plain += ms, e->n_plain++;
      for (guint i = 0; i < why->len; i++)
        {
          printf ("  FAIL %s\n", (char *) why->pdata[i]);
          count_reason (e, why->pdata[i]);
        }
      if (pass)
        printf ("  PASS\n");
    }
  fflush (stdout);

  if (e->out)
    {
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "version");
      json_builder_add_string_value (b, e->label);
      json_builder_set_member_name (b, "case");
      json_builder_add_string_value (b, id);
      json_builder_set_member_name (b, "turn");
      json_builder_add_int_value (b, e->t + 1);
      json_builder_set_member_name (b, "say");
      json_builder_add_string_value (b, json_object_get_string_member (turn, "say"));
      json_builder_set_member_name (b, "tools");
      json_builder_begin_array (b);
      for (guint i = 0; i < e->calls->len; i++)
        {
          Call *c = e->calls->pdata[i];
          g_autofree char *a = redact (e, c->arguments);
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "name");
          json_builder_add_string_value (b, c->name);
          json_builder_set_member_name (b, "arguments");
          json_builder_add_string_value (b, a);
          json_builder_set_member_name (b, "ms");
          json_builder_add_int_value (b, (gint64) c->ms);
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
      json_builder_set_member_name (b, "cards");
      json_builder_add_int_value (b, e->proposals);
      json_builder_set_member_name (b, "reply");
      json_builder_add_string_value (b, reply);
      json_builder_set_member_name (b, "ms");
      json_builder_add_int_value (b, (gint64) ms);
      json_builder_set_member_name (b, "pass");
      json_builder_add_boolean_value (b, pass);
      json_builder_set_member_name (b, "harness");
      json_builder_add_boolean_value (b, e->busy);
      json_builder_set_member_name (b, "fail");
      json_builder_begin_array (b);
      for (guint i = 0; i < why->len; i++)
        json_builder_add_string_value (b, why->pdata[i]);
      json_builder_end_array (b);
      json_builder_end_object (b);
      line = json_builder_get_root (b);
      {
        g_autofree char *text = json_to_string (line, FALSE);
        fprintf (e->out, "%s\n", text);
        fflush (e->out);
      }
    }
  e->t++;
  next_turn (e);
}

/* ---------------- the talk's signals ---------------- */

static void
on_reply (DictateTalk *talk, const char *text, Eval *e)
{
  if (!e->waiting)
    return;
  g_free (e->reply);
  e->reply = g_strdup (text);
}

static void
on_error (DictateTalk *talk, const char *message, Eval *e)
{
  if (!e->waiting)
    return;
  g_free (e->error);
  e->error = g_strdup (message);
}

static void
on_tool (DictateTalk *talk, const char *name, const char *arguments, const char *result, double ms, Eval *e)
{
  Call *c;

  if (!e->waiting)
    return;
  c = g_new0 (Call, 1);
  c->name = g_strdup (name);
  c->arguments = g_strdup (arguments);
  c->result = g_strdup (result);
  c->ms = ms;
  g_ptr_array_add (e->calls, c);
}

static void
on_propose (DictateTalk *talk, gpointer action, Eval *e)
{
  /* never tapped: a test stops at the card */
  if (e->waiting)
    e->proposals++;
}

static void
on_busy (DictateTalk *talk, Eval *e)
{
  if (e->waiting)
    e->busy = TRUE;
}

static gboolean
asking (Eval *e)
{
  return e->waiting && dictate_talk_get_state (e->talk) != DICTATE_TALK_IDLE;
}

static void
on_state (DictateTalk *talk, Eval *e)
{
  if (e->waiting && e->asked && dictate_talk_get_state (talk) == DICTATE_TALK_IDLE)
    turn_done (e);
}

static gboolean
too_long (gpointer data)
{
  Eval *e = data;

  e->limit = 0;
  g_free (e->error);
  e->error = g_strdup_printf ("no end to the reply after %d s", TURN_LIMIT_S);
  dictate_talk_stop (e->talk);
  if (e->waiting)
    turn_done (e);
  return G_SOURCE_REMOVE;
}

/* ---------------- the run ---------------- */

static gboolean
wanted (Eval *e, JsonObject *kase)
{
  return !e->only || g_strv_contains ((const char *const *) e->only, json_object_get_string_member (kase, "id"));
}

static void
refill (Eval *e)
{
  gint64 now = g_get_monotonic_time ();
  e->tokens = MIN (BURST, e->tokens + (now - e->tokens_at) / 1e6 * e->rate / 60.0);
  e->tokens_at = now;
}

static gboolean
ask_now (gpointer data)
{
  Eval *e = data;
  JsonObject *turn = turn_object (e);
  /* a turn's own "lang": the language switched to from that turn on, as in the app's list */
  const char *lang = json_object_has_member (turn, "lang") ? json_object_get_string_member (turn, "lang")
                                                           : json_object_get_string_member (e->kase, "lang");

  refill (e);
  /* a turn may take up to CALLS_MAX + 1 requests at once */
  if (e->tokens < CALLS_MAX + 1)
    {
      g_timeout_add ((guint) ((CALLS_MAX + 1 - e->tokens) * 60000 / e->rate) + 50, ask_now, e);
      return G_SOURCE_REMOVE;
    }
  g_ptr_array_set_size (e->calls, 0);
  g_clear_pointer (&e->reply, g_free);
  g_clear_pointer (&e->error, g_free);
  e->proposals = 0;
  e->busy = FALSE;
  e->waiting = TRUE;
  e->asked = g_get_monotonic_time ();
  e->limit = g_timeout_add_seconds (TURN_LIMIT_S, too_long, e);
  dictate_talk_ask (e->talk, lang, json_object_get_string_member (turn, "say"));
  /* an answer refused at once (no model) never leaves idle */
  if (!asking (e) && e->error)
    turn_done (e);
  return G_SOURCE_REMOVE;
}

static void
start_case (Eval *e)
{
  guint n;
  const DictateAccess *access = dictate_access_list (&n);
  JsonArray *off = member_array (e->kase, "off");

  /* a new conversation, the switches as the case has them, the private phone as the fixtures have it */
  dictate_talk_reset (e->talk);
  for (guint i = 0; i < n; i++)
    g_settings_set_boolean (e->settings, access[i].key, !in_array (off, access[i].key));
  if (!e->real)
    mock_services_reset ();
  g_string_truncate (e->results, 0);
  e->t = 0;
}

static void
next_turn (Eval *e)
{
  for (;;)
    {
      if (e->kase && e->t < json_array_get_length (json_object_get_array_member (e->kase, "turns")))
        {
          /* a beat between turns, as a person's */
          g_timeout_add (300, ask_now, e);
          return;
        }
      if (e->kase)
        e->c++;
      e->kase = NULL;
      while (e->c < json_array_get_length (e->cases) && !wanted (e, json_array_get_object_element (e->cases, e->c)))
        e->c++;
      if (e->c >= json_array_get_length (e->cases))
        {
          g_main_loop_quit (e->loop);
          return;
        }
      e->kase = json_array_get_object_element (e->cases, e->c);
      start_case (e);
    }
}

static gboolean
start (gpointer data)
{
  next_turn (data);
  return G_SOURCE_REMOVE;
}

/* --fixtures only in a private environment: never write over a phone's own stores or own its services' names */
static gboolean
private_environment (void)
{
  const char *system = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS"), *session = g_getenv ("DBUS_SESSION_BUS_ADDRESS");

  return g_str_has_prefix (g_get_home_dir (), "/tmp/") && g_str_has_prefix (g_get_user_data_dir (), "/tmp/")
         && system && session && !strcmp (system, session);
}

int
main (int argc, char **argv)
{
  Eval e = { 0 };
  gboolean fixtures = FALSE, serve = FALSE;
  char *only = NULL, *out = NULL, *label = NULL;
  double rate = 20;
  g_autoptr (GError) error = NULL;
  g_autoptr (GOptionContext) options = g_option_context_new ("CASES.json");
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree char *reason = NULL;
  JsonObject *root;
  GOptionEntry entries[] = {
    { "fixtures", 0, 0, G_OPTION_ARG_NONE, &fixtures, "the private phone: fixtures and stand-in services", NULL },
    { "real", 0, 0, G_OPTION_ARG_NONE, &e.real, "the phone's own data: what is printed is redacted", NULL },
    { "only", 0, 0, G_OPTION_ARG_STRING, &only, "these cases only", "ID,…" },
    { "label", 0, 0, G_OPTION_ARG_STRING, &label, "the version tried, for the results", "NAME" },
    { "out", 0, 0, G_OPTION_ARG_FILENAME, &out, "each turn as a line of JSON", "FILE" },
    { "rate", 0, 0, G_OPTION_ARG_DOUBLE, &rate, "requests a minute at most (20)", "N" },
    { "serve", 0, 0, G_OPTION_ARG_NONE, &serve, "with --fixtures: only the private phone, until killed", NULL },
    { NULL },
  };

  gst_init (&argc, &argv);
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error) || argc != 2 || fixtures == e.real
      || (serve && !fixtures))
    {
      g_printerr ("%s\n", error ? error->message : "agent-eval (--fixtures | --real) CASES.json");
      return 2;
    }
  if (!json_parser_load_from_file (parser, argv[1], &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }
  root = json_node_get_object (json_parser_get_root (parser));
  if (fixtures)
    {
      if (!private_environment ())
        {
          g_printerr ("--fixtures runs only in a private environment (run.sh eval)\n");
          return 2;
        }
      mock_fixtures_write (json_object_get_object_member (root, "phone"));
      mock_services_start ();
      if (serve)
        {
          g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);

          g_print ("the private phone is up\n");
          g_main_loop_run (loop);
          return 0;
        }
      /* what reaches outside (an app opened, Navi) is only logged */
      g_setenv ("DICTATE_TEST_DRY_RUN", "1", TRUE);
    }
  else
    g_setenv ("DICTATE_TEST_DRY_RUN", "1", TRUE);

  e.loop = g_main_loop_new (NULL, FALSE);
  e.talk = dictate_talk_new ();
  e.settings = g_settings_new ("vibeos.Dictate");
  e.cases = json_object_get_array_member (root, "cases");
  e.only = only ? g_strsplit (only, ",", -1) : NULL;
  e.label = label ? label : "unlabelled";
  e.rate = rate;
  e.tokens = BURST;
  e.tokens_at = g_get_monotonic_time ();
  e.calls = g_ptr_array_new_with_free_func ((GDestroyNotify) call_free);
  e.results = g_string_new (NULL);
  e.reasons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (out && !(e.out = fopen (out, "a")))
    {
      g_printerr ("%s: cannot write\n", out);
      return 2;
    }
  dictate_talk_set_speak (e.talk, FALSE);
  if (!dictate_talk_available (e.talk, "en", &reason))
    {
      g_printerr ("%s\n", reason);
      return 1;
    }
  g_signal_connect (e.talk, "reply", G_CALLBACK (on_reply), &e);
  g_signal_connect (e.talk, "state-changed", G_CALLBACK (on_state), &e);
  g_signal_connect (e.talk, "error", G_CALLBACK (on_error), &e);
  g_signal_connect (e.talk, "tool", G_CALLBACK (on_tool), &e);
  g_signal_connect (e.talk, "propose", G_CALLBACK (on_propose), &e);
  g_signal_connect (e.talk, "busy", G_CALLBACK (on_busy), &e);
  dictate_talk_prepare (e.talk, "en");
  g_timeout_add_seconds (3, start, &e);
  g_main_loop_run (e.loop);

  printf ("\n==== %s: %u of %u turns passed (%.1f %%)\n", e.label, e.passed, e.turns,
          e.turns ? 100.0 * e.passed / e.turns : 0);
  printf ("     confirm-card bypasses %u, invented data %u, harness failures (429) %u\n", e.bypasses, e.invented,
          e.harness);
  printf ("     average turn: %.0f ms with tools (%u), %.0f ms without (%u)\n",
          e.n_tools ? e.ms_tools / e.n_tools : 0, e.n_tools, e.n_plain ? e.ms_plain / e.n_plain : 0, e.n_plain);
  {
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init (&iter, e.reasons);
    while (g_hash_table_iter_next (&iter, &k, &v))
      printf ("     %-18s %u\n", (char *) k, GPOINTER_TO_UINT (v));
  }
  if (e.out)
    fclose (e.out);
  return e.turns && e.passed == e.turns ? 0 : 1;
}
