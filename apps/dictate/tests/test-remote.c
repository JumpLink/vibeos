/* test-remote: the model on a server (dictate-remote.c), the server stood in for on a thread of its own. A reply with
 * nothing in it is asked for once more; given nothing twice, the conversation keeps its turns' order (the chat
 * templates want user and assistant by turns), and the turn can be taken out of it as if never asked. */
#include <string.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "dictate-remote.h"

/* dictate-brain.c's, which needs llama.cpp: the server's errors are of its domain */
G_DEFINE_QUARK (dictate-brain-error-quark, dictate_brain_error)

typedef struct {
  GMutex lock;
  GCond ready;
  guint port;
  GMainLoop *loop;
  guint chats;     /* the questions put to the model */
  guint empty;     /* the first this many get a reply with nothing in it */
  char *roles;     /* the last question's messages, by role: "system,user" */
  char *last;      /* and what its last one says */
} Server;

static const char NOTHING[] = "data: {\"choices\":[{\"delta\":{}}]}\n\n"
                              "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                              "data: [DONE]\n\n";
static const char ASKED[] = "data: {\"choices\":[{\"delta\":{\"content\":\"Who would you like to call?\"}}]}\n\n"
                            "data: [DONE]\n\n";

static void
on_models (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  static const char models[] = "{\"data\":[{\"id\":\"test-model\"}]}";

  soup_server_message_set_status (msg, 200, NULL);
  soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, models, strlen (models));
}

static void
on_chat (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  Server *s = data;
  SoupMessageBody *body = soup_server_message_get_request_body (msg);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GString) roles = g_string_new (NULL);
  JsonArray *messages;
  const char *answer;

  g_assert_true (json_parser_load_from_data (parser, body->data, body->length, NULL));
  messages = json_object_get_array_member (json_node_get_object (json_parser_get_root (parser)), "messages");
  for (guint i = 0; i < json_array_get_length (messages); i++)
    g_string_append_printf (roles, "%s%s", i ? "," : "",
                            json_object_get_string_member (json_array_get_object_element (messages, i), "role"));
  g_mutex_lock (&s->lock);
  g_free (s->roles);
  s->roles = g_strdup (roles->str);
  g_free (s->last);
  s->last = g_strdup (json_object_get_string_member_with_default (
    json_array_get_object_element (messages, json_array_get_length (messages) - 1), "content", ""));
  answer = s->chats++ < s->empty ? NOTHING : ASKED;
  g_mutex_unlock (&s->lock);
  soup_server_message_set_status (msg, 200, NULL);
  soup_server_message_set_response (msg, "text/event-stream", SOUP_MEMORY_STATIC, answer, strlen (answer));
}

static gpointer
server_thread (gpointer data)
{
  Server *s = data;
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (SoupServer) server = NULL;
  GSList *uris;

  g_main_context_push_thread_default (context);
  server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, "/v1/models", on_models, s, NULL);
  soup_server_add_handler (server, "/v1/chat/completions", on_chat, s, NULL);
  g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  uris = soup_server_get_uris (server);
  g_mutex_lock (&s->lock);
  s->port = g_uri_get_port (uris->data);
  s->loop = g_main_loop_new (context, FALSE);
  g_cond_signal (&s->ready);
  g_mutex_unlock (&s->lock);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  g_main_loop_run (s->loop);
  g_main_context_pop_thread_default (context);
  return NULL;
}

static GThread *
server_start (Server *s, guint empty)
{
  GThread *thread;

  g_mutex_init (&s->lock);
  g_cond_init (&s->ready);
  s->empty = empty;
  thread = g_thread_new ("model", server_thread, s);
  g_mutex_lock (&s->lock);
  while (!s->loop)
    g_cond_wait (&s->ready, &s->lock);
  g_mutex_unlock (&s->lock);
  return thread;
}

static void
server_stop (Server *s, GThread *thread)
{
  g_main_loop_quit (s->loop);
  g_thread_join (thread);
  g_main_loop_unref (s->loop);
  g_free (s->roles);
  g_free (s->last);
  g_mutex_clear (&s->lock);
  g_cond_clear (&s->ready);
}

static char *
ask (DictateRemote *remote, const char *text)
{
  g_autoptr (GPtrArray) calls = NULL;
  g_autoptr (GError) error = NULL;
  char *reply = dictate_remote_step (remote, text, NULL, NULL, NULL, &calls, NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (reply);
  g_assert_true (!calls || calls->len == 0);
  return reply;
}

/* nothing twice: asked once more, and "" then; the next question follows an assistant turn, not a user one */
static void
test_empty_twice (void)
{
  Server s = { 0 };
  GThread *thread = server_start (&s, 2);
  g_autofree char *address = g_strdup_printf ("http://127.0.0.1:%u/v1", s.port);
  DictateRemote *remote = dictate_remote_new (address);
  g_autofree char *nothing = NULL, *asked = NULL;

  dictate_remote_reset (remote, "You are a test.");
  nothing = ask (remote, "Call him.");
  g_assert_cmpstr (nothing, ==, "");
  g_assert_cmpuint (s.chats, ==, 2);
  g_assert_cmpstr (s.roles, ==, "system,user");
  asked = ask (remote, "Call him, please.");
  g_assert_cmpstr (asked, ==, "Who would you like to call?");
  g_assert_cmpuint (s.chats, ==, 3);
  g_assert_cmpstr (s.roles, ==, "system,user,assistant,user");
  dictate_remote_free (remote);
  server_stop (&s, thread);
}

/* nothing once: the second asking answers, and the user's question is in the conversation once */
static void
test_empty_once (void)
{
  Server s = { 0 };
  GThread *thread = server_start (&s, 1);
  g_autofree char *address = g_strdup_printf ("http://127.0.0.1:%u/v1", s.port);
  DictateRemote *remote = dictate_remote_new (address);
  g_autofree char *asked = NULL;

  dictate_remote_reset (remote, "You are a test.");
  asked = ask (remote, "Call him.");
  g_assert_cmpstr (asked, ==, "Who would you like to call?");
  g_assert_cmpuint (s.chats, ==, 2);
  g_assert_cmpstr (s.roles, ==, "system,user");
  g_assert_cmpstr (s.last, ==, "Call him.");
  dictate_remote_free (remote);
  server_stop (&s, thread);
}

/* an unanswered turn taken out: the next question is the conversation's only one */
static void
test_forget_unanswered (void)
{
  Server s = { 0 };
  GThread *thread = server_start (&s, 2);
  g_autofree char *address = g_strdup_printf ("http://127.0.0.1:%u/v1", s.port);
  DictateRemote *remote = dictate_remote_new (address);
  g_autofree char *nothing = NULL, *asked = NULL;

  dictate_remote_reset (remote, "You are a test.");
  nothing = ask (remote, "Call him.");
  g_assert_cmpstr (nothing, ==, "");
  dictate_remote_forget_turn (remote);
  asked = ask (remote, "Call Max.");
  g_assert_cmpstr (asked, ==, "Who would you like to call?");
  g_assert_cmpstr (s.roles, ==, "system,user");
  g_assert_cmpstr (s.last, ==, "Call Max.");
  dictate_remote_free (remote);
  server_stop (&s, thread);
}

int
main (int argc, char **argv)
{
  g_setenv ("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/remote/empty-twice", test_empty_twice);
  g_test_add_func ("/remote/empty-once", test_empty_once);
  g_test_add_func ("/remote/forget-unanswered", test_forget_unanswered);
  return g_test_run ();
}
