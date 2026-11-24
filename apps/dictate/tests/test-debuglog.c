/* test-debuglog: Agent's debug log (dictate-debuglog.h), each test in a private data directory: a turn written
 * whole, its file and directory only the user's, the gateway's key and the phone's id cut to 4 characters wherever
 * they would be, nothing written without a turn, the newest turns kept, and clearing. */
#include "dictate-brain.h"
#include "dictate-debuglog.h"
#include "dictate-device.h"

#include <glib/gstdio.h>
#include <string.h>

static void
test_turn (void)
{
  /* the phone's id and the gateway's key, made in this test's data directory as the app makes them */
  const char *secret = dictate_device_secret ();
  const char *id = dictate_device_id ();
  g_autofree char *body = g_strdup_printf ("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi %s\"}],"
                                           "\"tools\":[{\"type\":\"function\"}]}", secret);
  g_autofree char *header = g_strdup_printf ("Authorization: Bearer %s", secret);
  g_autofree char *cut_secret = g_strdup_printf ("%.4s…", secret);
  g_autoptr (GPtrArray) calls = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_brain_call_free);
  g_autoptr (GPtrArray) list = NULL;
  g_autofree char *dir = dictate_debuglog_dir ();
  g_autofree char *text = NULL;
  DictateBrainCall *c = g_new0 (DictateBrainCall, 1);
  DictateDebugTurn *t;
  DictateDebugEntry *e;
  GStatBuf st;

  g_assert_false (dictate_debuglog_active ());
  t = dictate_debuglog_begin (7, "Who called me?", "en", "https://example.org/v1", FALSE, FALSE);
  g_assert_true (dictate_debuglog_active ());
  dictate_debuglog_request ("POST", "https://example.org/v1/chat/completions", body, strlen (body));
  dictate_debuglog_http ("POST", "https://example.org/v1/chat/completions", 200, 12);
  c->id = g_strdup ("call-0"), c->name = g_strdup ("get_recent_calls"), c->arguments = g_strdup ("{\"count\":3}");
  g_ptr_array_add (calls, c);
  dictate_debuglog_response ("", calls, 900, 20, 300, 410);
  dictate_debuglog_tool ("get_recent_calls", "{\"count\":3}", "{\"calls\":[]}", 25, TRUE);
  /* the next step's request: the same tools, written as the same */
  dictate_debuglog_request ("POST", "https://example.org/v1/chat/completions", body, strlen (body));
  dictate_debuglog_note ("error", header);
  dictate_debuglog_note ("error", id);
  dictate_debuglog_end (t, "Nobody called today.", "answered");
  g_assert_false (dictate_debuglog_active ());

  list = dictate_debuglog_list ();
  g_assert_cmpuint (list->len, ==, 1);
  e = list->pdata[0];
  g_assert_cmpstr (e->said, ==, "Who called me?");
  g_assert_cmpstr (e->outcome, ==, "answered");
  g_assert_cmpuint (e->tools, ==, 1);
  g_assert_cmpuint (e->steps, ==, 7);

  g_assert_cmpint (g_stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  g_assert_cmpint (g_stat (e->path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);

  g_assert_true (g_file_get_contents (e->path, &text, NULL, NULL));
  g_assert_null (strstr (text, secret));
  g_assert_null (strstr (text, id));
  g_assert_nonnull (strstr (text, cut_secret));
  g_assert_nonnull (strstr (text, "the same as in this turn's first request"));
  {
    g_autoptr (JsonNode) node = dictate_debuglog_read (e->path);
    JsonObject *o = json_node_get_object (node);
    JsonArray *steps = json_object_get_array_member (o, "steps");
    JsonObject *request = json_array_get_object_element (steps, 0);
    JsonObject *tool = json_array_get_object_element (steps, 3);

    g_assert_cmpstr (json_object_get_string_member (o, "reply"), ==, "Nobody called today.");
    /* a request's body and a tool's result are JSON, not strings of it */
    g_assert_true (JSON_NODE_HOLDS_OBJECT (json_object_get_member (request, "body")));
    g_assert_cmpstr (json_object_get_string_member (tool, "kind"), ==, "tool");
    g_assert_cmpstr (json_object_get_string_member (tool, "name"), ==, "get_recent_calls");
    g_assert_true (JSON_NODE_HOLDS_OBJECT (json_object_get_member (tool, "result")));
  }
}

static void
test_no_turn (void)
{
  g_autoptr (GPtrArray) list = NULL;

  dictate_debuglog_request ("POST", "u", "{}", 2);
  dictate_debuglog_tool ("get_time", "{}", "{}", 1, TRUE);
  dictate_debuglog_note ("error", "nothing");
  dictate_debuglog_end (NULL, "x", "answered");
  list = dictate_debuglog_list ();
  g_assert_cmpuint (list->len, ==, 0);
}

static void
test_rotation (void)
{
  g_autoptr (GPtrArray) list = NULL;
  g_autoptr (GPtrArray) after = NULL;
  g_autofree char *big = g_strnfill (1024 * 1024, 'x');
  goffset bytes = 0;

  for (guint i = 0; i < DEBUGLOG_KEEP + 5; i++)
    {
      g_autofree char *said = g_strdup_printf ("turn %u", i);
      DictateDebugTurn *t = dictate_debuglog_begin (i, said, "en", "", FALSE, FALSE);

      dictate_debuglog_end (t, "ok", "answered");
      /* the files are named by their time to the millisecond */
      g_usleep (1500);
    }
  list = dictate_debuglog_list ();
  g_assert_cmpuint (list->len, ==, DEBUGLOG_KEEP);
  g_assert_cmpstr (((DictateDebugEntry *) list->pdata[0])->said, ==, "turn 104");
  g_assert_cmpstr (((DictateDebugEntry *) list->pdata[DEBUGLOG_KEEP - 1])->said, ==, "turn 5");

  /* big turns: the oldest go until the rest fit in DEBUGLOG_BYTES */
  for (guint i = 0; i < 7; i++)
    {
      DictateDebugTurn *t = dictate_debuglog_begin (200 + i, big, "en", "", FALSE, FALSE);

      dictate_debuglog_end (t, "ok", "answered");
      g_usleep (1500);
    }
  after = dictate_debuglog_list ();
  for (guint i = 0; i < after->len; i++)
    {
      GStatBuf st;

      g_assert_cmpint (g_stat (((DictateDebugEntry *) after->pdata[i])->path, &st), ==, 0);
      bytes += st.st_size;
    }
  g_assert_cmpint (bytes, <=, DEBUGLOG_BYTES);
  g_assert_cmpuint (after->len, >=, 4);

  dictate_debuglog_clear ();
  g_clear_pointer (&after, g_ptr_array_unref);
  after = dictate_debuglog_list ();
  g_assert_cmpuint (after->len, ==, 0);
}

static void
test_redact (void)
{
  g_autofree char *a = dictate_debuglog_redact ("Authorization: Bearer 0123456789abcdef and \"Bearer zz\"");

  g_assert_cmpstr (a, ==, "Authorization: Bearer 0123… and \"Bearer zz\"");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/debuglog/redact", test_redact);
  g_test_add_func ("/debuglog/no-turn", test_no_turn);
  g_test_add_func ("/debuglog/turn", test_turn);
  g_test_add_func ("/debuglog/rotation", test_rotation);
  return g_test_run ();
}
