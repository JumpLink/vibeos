/* dictate-remote: see dictate-remote.h.
 *
 * The conversation is kept here as messages and sent whole with each turn (the server keeps nothing between
 * them). The reply comes as server-sent events, "data: {…}" lines each with a piece of it in
 * choices[0].delta.content, up to "data: [DONE]". The model's name is the server's first in /models, asked once. */
#include "dictate-remote.h"
#include "dictate-device.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define REPLY_MAX 400           /* tokens: a spoken answer is a few sentences */
#define TIMEOUT_S 60            /* no answer from the server for this long: it is not there */
#define TOO_MANY  429           /* libsoup has no name for it */

struct _DictateRemote {
  char *base;                   /* https://host:port/v1 */
  char *enroll;                 /* https://host:port/agent/enroll */
  char *model;                  /* the server's, once asked */
  SoupSession *session;
  GPtrArray *messages;          /* JsonNode *, {role, content} */
  gboolean enrolled;            /* this run */
  int read, written;
  double read_ms, first_ms, total_ms;
};

static void
user_error (GError **error, const char *message)
{
  g_set_error_literal (error, DICTATE_BRAIN_ERROR, DICTATE_BRAIN_ERROR_USER, message);
}

DictateRemote *
dictate_remote_new (const char *address)
{
  DictateRemote *self = g_new0 (DictateRemote, 1);
  g_autofree char *root = NULL;

  self->base = g_strdup (address);
  while (g_str_has_suffix (self->base, "/"))
    self->base[strlen (self->base) - 1] = '\0';
  /* the gateway's own endpoints are beside the API's /v1 */
  root = g_str_has_suffix (self->base, "/v1") ? g_strndup (self->base, strlen (self->base) - 3)
                                               : g_strdup (self->base);
  self->enroll = g_strconcat (root, "/agent/enroll", NULL);
  self->session = soup_session_new_with_options ("timeout", TIMEOUT_S, "idle-timeout", 120,
                                                 "user-agent", "vibeos-agent/1.0 ", NULL);
  self->messages = g_ptr_array_new_with_free_func ((GDestroyNotify) json_node_unref);
  return self;
}

void
dictate_remote_free (DictateRemote *self)
{
  if (!self)
    return;
  g_free (self->base);
  g_free (self->enroll);
  g_free (self->model);
  g_clear_object (&self->session);
  g_ptr_array_unref (self->messages);
  g_free (self);
}

static void
add_message (DictateRemote *self, const char *role, const char *content)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, role);
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, content);
  json_builder_end_object (b);
  g_ptr_array_add (self->messages, json_builder_get_root (b));
}

void
dictate_remote_reset (DictateRemote *self, const char *system)
{
  g_ptr_array_set_size (self->messages, 0);
  if (system && *system)
    add_message (self, "system", system);
}

/* --- the gateway */

static SoupMessage *
request (DictateRemote *self, const char *method, const char *url, GBytes *json)
{
  SoupMessage *msg = soup_message_new (method, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  g_autofree char *bearer = g_strconcat ("Bearer ", dictate_device_secret (), NULL);

  soup_message_headers_replace (headers, "X-Vibeos-Device", dictate_device_id ());
  soup_message_headers_replace (headers, "Authorization", bearer);
  soup_message_headers_replace (headers, "Accept", "application/json");
  if (json)
    soup_message_set_request_body_from_bytes (msg, "application/json", json);
  return msg;
}

static GBytes *
to_bytes (JsonBuilder *b)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  gsize length;
  char *text;

  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, &length);
  return g_bytes_new_take (text, length);
}

/* the network's own failure, in words a person can act on */
static void
network_error (GError **error, GError *cause)
{
  if (g_error_matches (cause, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_propagate_error (error, cause);
      return;
    }
  g_warning ("agent: %s", cause->message);
  g_error_free (cause);
  user_error (error, _("The assistant cannot be reached. Is the phone online?"));
}

/* the phone made known to the gateway: its id, its secret and what it is */
static gboolean
enroll (DictateRemote *self, GCancellable *cancellable, GError **error)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GBytes) body = NULL;
  g_autoptr (SoupMessage) msg = NULL;
  g_autoptr (GBytes) answer = NULL;
  GError *failure = NULL;
  guint status;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "device_id");
  json_builder_add_string_value (b, dictate_device_id ());
  json_builder_set_member_name (b, "secret");
  json_builder_add_string_value (b, dictate_device_secret ());
  if (dictate_device_model ())
    {
      json_builder_set_member_name (b, "model");
      json_builder_add_string_value (b, dictate_device_model ());
    }
  if (dictate_device_release ())
    {
      json_builder_set_member_name (b, "vibeos_version");
      json_builder_add_string_value (b, dictate_device_release ());
    }
  json_builder_set_member_name (b, "app_version");
  json_builder_add_string_value (b, "1.0");
  json_builder_end_object (b);
  body = to_bytes (b);
  msg = soup_message_new ("POST", self->enroll);
  soup_message_headers_replace (soup_message_get_request_headers (msg), "Accept", "application/json");
  soup_message_set_request_body_from_bytes (msg, "application/json", body);
  answer = soup_session_send_and_read (self->session, msg, cancellable, &failure);
  if (!answer)
    {
      network_error (error, failure);
      return FALSE;
    }
  status = soup_message_get_status (msg);
  if (status == TOO_MANY)
    {
      user_error (error, _("Too many questions at once. Wait a minute, then ask again."));
      return FALSE;
    }
  if (status != SOUP_STATUS_OK && status != SOUP_STATUS_CREATED)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "enroll: HTTP %u", status);
      return FALSE;
    }
  self->enrolled = TRUE;
  return TRUE;
}

/* `msg` sent; a gateway that does not know the phone (401) has it enroll, and the message is sent again, once.
 * The stream of the answer, or NULL with the error */
static GInputStream *
call (DictateRemote *self, SoupMessage **msg, const char *method, const char *url, GBytes *json,
      GCancellable *cancellable, GError **error)
{
  for (int attempt = 0;; attempt++)
    {
      GError *failure = NULL;
      GInputStream *in;
      guint status;

      *msg = request (self, method, url, json);
      in = soup_session_send (self->session, *msg, cancellable, &failure);
      if (!in)
        {
          network_error (error, failure);
          return NULL;
        }
      status = soup_message_get_status (*msg);
      if (status == SOUP_STATUS_OK)
        return in;
      g_input_stream_close (in, NULL, NULL);
      g_object_unref (in);
      if (status == SOUP_STATUS_UNAUTHORIZED && attempt == 0)
        {
          g_clear_object (msg);
          if (!enroll (self, cancellable, error))
            return NULL;
          continue;
        }
      if (status == SOUP_STATUS_FORBIDDEN)
        user_error (error, _("This phone may not use the assistant."));
      else if (status == TOO_MANY)
        user_error (error, _("Too many questions at once. Wait a minute, then ask again."));
      else if (status >= 500)
        user_error (error, _("The assistant is not working right now. Try again later."));
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: HTTP %u", url, status);
      return NULL;
    }
}

/* the server's model, the first it lists */
static gboolean
find_model (DictateRemote *self, GCancellable *cancellable, GError **error)
{
  g_autofree char *url = g_strconcat (self->base, "/models", NULL);
  g_autoptr (SoupMessage) msg = NULL;
  g_autoptr (GInputStream) in = call (self, &msg, "GET", url, NULL, cancellable, error);
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root;
  JsonArray *data;

  if (!in)
    return FALSE;
  if (!json_parser_load_from_stream (parser, in, cancellable, error))
    return FALSE;
  root = JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)) ? json_node_get_object (json_parser_get_root (parser))
                                                               : NULL;
  data = root && json_object_has_member (root, "data") ? json_object_get_array_member (root, "data") : NULL;
  if (!data || json_array_get_length (data) == 0 ||
      !JSON_NODE_HOLDS_OBJECT (json_array_get_element (data, 0)))
    {
      user_error (error, _("The assistant is not working right now. Try again later."));
      return FALSE;
    }
  self->model = g_strdup (json_object_get_string_member_with_default (
                            json_array_get_object_element (data, 0), "id", ""));
  return *self->model != '\0';
}

void
dictate_remote_warm (DictateRemote *self)
{
  g_autoptr (GError) error = NULL;

  if (!self->model && !find_model (self, NULL, &error))
    g_message ("agent: not ready yet: %s", error->message);
}

/* --- a turn */

static GBytes *
chat_body (DictateRemote *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, self->model);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, TRUE);
  /* the token counts, in a last event of their own: for the log */
  json_builder_set_member_name (b, "stream_options");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "include_usage");
  json_builder_add_boolean_value (b, TRUE);
  json_builder_end_object (b);
  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, REPLY_MAX);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, 0.7);
  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  for (guint i = 0; i < self->messages->len; i++)
    json_builder_add_value (b, json_node_copy (self->messages->pdata[i]));
  json_builder_end_array (b);
  json_builder_end_object (b);
  return to_bytes (b);
}

/* one event's piece of the reply ("" when it has none), or NULL at the end */
static char *
event_piece (const char *json, gboolean *usage, int *prompt_tokens, int *completion_tokens)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root, *delta;
  JsonArray *choices;

  if (!json_parser_load_from_data (parser, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return g_strdup ("");
  root = json_node_get_object (json_parser_get_root (parser));
  if (json_object_has_member (root, "usage") && JSON_NODE_HOLDS_OBJECT (json_object_get_member (root, "usage")))
    {
      JsonObject *u = json_object_get_object_member (root, "usage");
      *usage = TRUE;
      *prompt_tokens = json_object_get_int_member_with_default (u, "prompt_tokens", 0);
      *completion_tokens = json_object_get_int_member_with_default (u, "completion_tokens", 0);
    }
  choices = json_object_has_member (root, "choices") ? json_object_get_array_member (root, "choices") : NULL;
  if (!choices || json_array_get_length (choices) == 0 || !JSON_NODE_HOLDS_OBJECT (json_array_get_element (choices, 0)))
    return g_strdup ("");
  delta = json_object_has_member (json_array_get_object_element (choices, 0), "delta")
          ? json_object_get_object_member (json_array_get_object_element (choices, 0), "delta") : NULL;
  if (!delta || !json_object_has_member (delta, "content") || JSON_NODE_HOLDS_NULL (json_object_get_member (delta, "content")))
    return g_strdup ("");
  return g_strdup (json_object_get_string_member (delta, "content"));
}

char *
dictate_remote_reply (DictateRemote *self, const char *text, DictateBrainPiece piece, gpointer data,
                      GCancellable *cancellable, GError **error)
{
  gint64 t0 = g_get_monotonic_time ();
  g_autofree char *url = g_strconcat (self->base, "/chat/completions", NULL);
  g_autoptr (SoupMessage) msg = NULL;
  g_autoptr (GInputStream) in = NULL;
  g_autoptr (GDataInputStream) lines = NULL;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GString) reply = g_string_new (NULL);
  gboolean usage = FALSE;
  int prompt_tokens = 0, completion_tokens = 0, pieces = 0;

  self->read = self->written = 0;
  self->read_ms = self->first_ms = self->total_ms = -1;
  if (!self->model && !find_model (self, cancellable, error))
    return NULL;
  add_message (self, "user", text);
  body = chat_body (self);
  in = call (self, &msg, "POST", url, body, cancellable, error);
  if (!in)
    {
      /* the question was not asked: it is not part of the conversation */
      g_ptr_array_remove_index (self->messages, self->messages->len - 1);
      return NULL;
    }
  self->read_ms = (g_get_monotonic_time () - t0) / 1000.0;
  lines = g_data_input_stream_new (in);
  for (;;)
    {
      GError *failure = NULL;
      gsize length;
      g_autofree char *line = g_data_input_stream_read_line_utf8 (lines, &length, cancellable, &failure);
      g_autofree char *part = NULL;

      if (!line)
        {
          /* the server ended the stream; or it was cancelled, or cut off after some of the reply: what came of
           * it is the reply, as with a stop */
          if (failure && !g_error_matches (failure, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              if (reply->len == 0)
                {
                  network_error (error, failure);
                  g_ptr_array_remove_index (self->messages, self->messages->len - 1);
                  return NULL;
                }
              g_warning ("agent: the reply was cut off: %s", failure->message);
            }
          g_clear_error (&failure);
          break;
        }
      g_strstrip (line);
      if (!g_str_has_prefix (line, "data:"))
        continue;
      if (g_str_equal (g_strstrip (line + 5), "[DONE]"))
        break;
      part = event_piece (line + 5, &usage, &prompt_tokens, &completion_tokens);
      if (!*part)
        continue;
      pieces++;
      if (self->first_ms < 0)
        self->first_ms = (g_get_monotonic_time () - t0) / 1000.0;
      g_string_append (reply, part);
      /* stopped: the connection goes, and with it the rest of the reply */
      if (piece && !piece (part, data))
        break;
    }
  g_input_stream_close (G_INPUT_STREAM (lines), NULL, NULL);
  self->read = usage ? prompt_tokens : 0;
  self->written = usage ? completion_tokens : pieces;
  self->total_ms = (g_get_monotonic_time () - t0) / 1000.0;
  add_message (self, "assistant", reply->str);
  return g_string_free (g_steal_pointer (&reply), FALSE);
}

void
dictate_remote_last_timing (DictateRemote *self, int *read, int *written, double *read_ms, double *first_ms,
                            double *total_ms)
{
  if (read) *read = self->read;
  if (written) *written = self->written;
  if (read_ms) *read_ms = self->read_ms;
  if (first_ms) *first_ms = self->first_ms;
  if (total_ms) *total_ms = self->total_ms;
}
