/* test-tools: a switch in "What Agent can use" that is off takes that area's tools away from the model, and a call
 * the model makes to one anyway is refused before anything is read. Phone calls first: with Calls off, the call log
 * and the call card are not offered. Nothing here reads the phone: a refused call returns before its tool runs.
 * And Agent's action log (dictate-actions.c), in a directory of the test's own: kept private, newest first, trimmed,
 * and read back by get_action_log and undo_last_action. */
#include "dictate-access.h"
#include "dictate-actions.h"
#include "dictate-tools.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

/* the areas with one switched off (an access-* key), or all on (NULL) */
static guint32
areas_without (const char *key)
{
  guint n;
  const DictateAccess *list = dictate_access_list (&n);
  guint32 areas = 0;

  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (list[i].key, key) != 0)
      areas |= 1u << i;
  return areas;
}

/* the names of the tools the model is offered */
static GPtrArray *
offered (guint32 areas)
{
  g_autofree char *json = dictate_tools_definitions (areas);
  g_autoptr (JsonNode) root = json_from_string (json, NULL);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  JsonArray *tools;

  g_assert_nonnull (root);
  g_assert_true (JSON_NODE_HOLDS_ARRAY (root));
  tools = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (tools); i++)
    {
      JsonObject *function = json_object_get_object_member (json_array_get_object_element (tools, i), "function");
      g_ptr_array_add (names, g_strdup (json_object_get_string_member (function, "name")));
    }
  return names;
}

static gboolean
has (GPtrArray *names, const char *name)
{
  for (guint i = 0; i < names->len; i++)
    if (!strcmp (names->pdata[i], name))
      return TRUE;
  return FALSE;
}

static void
test_calls_off (void)
{
  g_autoptr (GPtrArray) all = offered (areas_without (NULL));
  g_autoptr (GPtrArray) some = offered (areas_without ("access-calls"));

  g_assert_true (has (all, "get_recent_calls"));
  g_assert_true (has (all, "propose_call"));
  g_assert_false (has (some, "get_recent_calls"));
  g_assert_false (has (some, "propose_call"));
  /* the rest stay */
  g_assert_true (has (some, "find_contacts"));
  g_assert_true (has (some, "get_messages"));
  g_assert_true (has (some, "get_time"));
}

/* every area: off, none of its tools is offered; those without an area (time, battery, storage) always are */
static void
test_every_area (void)
{
  guint n;
  const DictateAccess *list = dictate_access_list (&n);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GPtrArray) all = offered (areas_without (NULL));
      g_autoptr (GPtrArray) some = offered (areas_without (list[i].key));
      guint gone = 0;

      for (guint j = 0; j < all->len; j++)
        if (!has (some, all->pdata[j]))
          gone++;
      /* an area with tools loses them all, one without loses none */
      g_assert_cmpuint (gone > 0, ==, dictate_tools_area_has_tools (list[i].key));
      g_assert_true (has (some, "get_time"));
      g_assert_true (has (some, "get_battery"));
      g_assert_true (has (some, "get_storage"));
    }
}

/* the model names a tool it was not offered: refused, with what to tell the user */
static void
test_refused (void)
{
  DictateToolContext ctx = { .areas = areas_without ("access-calls"), .language = "en" };
  g_autofree char *calls = dictate_tools_run ("get_recent_calls", "{}", &ctx, 1000);
  g_autofree char *card = dictate_tools_run ("propose_call", "{\"to\":\"Anna\"}", &ctx, 1000);
  g_autofree char *none = dictate_tools_run ("format_phone", "{}", &ctx, 1000);

  g_assert_nonnull (strstr (calls, "\"error\""));
  g_assert_nonnull (strstr (calls, "No access"));
  g_assert_nonnull (strstr (card, "No access"));
  g_assert_nonnull (strstr (none, "no such tool"));
}

static const char *const SERVER_ONLY[] = { DICTATE_SENT_SERVER, NULL };

/* the log: private (0600), newest first, ids counting up, an undo marked, the oldest dropped past its size */
static void
test_log (void)
{
  g_autofree char *path = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "agent-log.jsonl", NULL);
  g_autoptr (GPtrArray) entries = NULL;
  gint64 alarm, undo;
  GStatBuf st;

  dictate_actions_forget ();
  g_assert_cmpint (dictate_actions_record ("question", DICTATE_TIER_READ, "answered", SERVER_ONLY, NULL, 0), ==, 1);
  alarm = dictate_actions_record ("set_alarm", DICTATE_TIER_UNDOABLE, "done", SERVER_ONLY,
                                  json_from_string ("{\"delete_alarm\": 7, \"about\": \"the alarm at 06:30\"}", NULL),
                                  0);
  g_assert_cmpint (alarm, ==, 2);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  entries = dictate_actions_recent (10);
  g_assert_cmpuint (entries->len, ==, 2);
  g_assert_cmpstr (json_object_get_string_member (entries->pdata[0], "tool"), ==, "set_alarm");
  g_assert_false (dictate_actions_undone (entries, alarm));
  g_clear_pointer (&entries, g_ptr_array_unref);
  undo = dictate_actions_record ("undo_last_action", DICTATE_TIER_UNDOABLE, "done", SERVER_ONLY, NULL, alarm);
  entries = dictate_actions_recent (10);
  g_assert_true (dictate_actions_undone (entries, alarm));
  g_assert_false (dictate_actions_undone (entries, undo));
  g_clear_pointer (&entries, g_ptr_array_unref);
  for (guint i = 0; i < 400; i++)
    dictate_actions_record ("get_time", DICTATE_TIER_READ, "done", SERVER_ONLY, NULL, 0);
  entries = dictate_actions_recent (1000);
  g_assert_cmpuint (entries->len, <=, 300);
  g_assert_cmpint (json_object_get_int_member (entries->pdata[0], "id"), ==, 403);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  dictate_actions_forget ();
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
}

/* "what did you just do?" and "undo": read from the log, with what went where */
static void
test_log_tools (void)
{
  DictateToolContext ctx = { .areas = G_MAXUINT32, .language = "en" };
  g_autofree char *empty = NULL, *nothing = NULL, *log = NULL, *card = NULL;
  const char *const sms[] = { "the message, by SMS to its recipient", NULL };

  dictate_actions_forget ();
  empty = dictate_tools_run ("get_action_log", "{}", &ctx, 1000);
  g_assert_nonnull (strstr (empty, "has not done anything yet"));
  g_assert_nonnull (strstr (empty, "model server"));
  nothing = dictate_tools_run ("undo_last_action", "{}", &ctx, 1000);
  g_assert_nonnull (strstr (nothing, "\"error\""));
  g_assert_nonnull (strstr (nothing, "nothing"));
  dictate_actions_record ("question", DICTATE_TIER_READ, "answered", SERVER_ONLY, NULL, 0);
  dictate_actions_record ("propose_message", DICTATE_TIER_CONFIRMED, "the user tapped it, and it was done", sms,
                          NULL, 0);
  log = dictate_tools_run ("get_action_log", "{}", &ctx, 1000);
  g_assert_nonnull (strstr (log, "a text message, on a card"));
  g_assert_nonnull (strstr (log, "by SMS"));
  g_assert_nonnull (strstr (log, "\"questions_answered_meanwhile\":1"));
  /* reading the log is not an entry of its own */
  {
    g_autoptr (GPtrArray) last = dictate_actions_recent (1);
    g_assert_cmpstr (json_object_get_string_member (last->pdata[0], "tool"), ==, "propose_message");
  }
  card = dictate_tools_run ("undo_last_action", "{}", &ctx, 1000);
  g_assert_nonnull (strstr (card, "cannot undo"));
  dictate_actions_forget ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/tools/log", test_log);
  g_test_add_func ("/tools/log-tools", test_log_tools);
  g_test_add_func ("/tools/calls-off", test_calls_off);
  g_test_add_func ("/tools/every-area", test_every_area);
  g_test_add_func ("/tools/refused", test_refused);
  return g_test_run ();
}
