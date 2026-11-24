/* The call service's client: see calls-client.h. */
#include "calls-client.h"
#include "call-log.h"

struct _CallsClient
{
  GObject parent;
  GDBusProxy *proxy;
  GPtrArray *calls;             /* CallInfo */
  /* the properties as last read, to tell which of them a PropertiesChanged (which carries them all) changed */
  GVariant *last[8];
  gboolean muted, speaker, can_record;
  char *status;
  GStrv features;
  char *country;
  guint missed;
};

G_DEFINE_FINAL_TYPE (CallsClient, calls_client, G_TYPE_OBJECT)

enum { CHANGED, AUDIO_CHANGED, STATUS_CHANGED, MISSED_CHANGED, CALL_LOG_CHANGED, FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* which signal each property's change emits */
static const struct { const char *name; guint signal; } properties[] = {
  { "Calls", CHANGED },
  { "Muted", AUDIO_CHANGED },
  { "Speaker", AUDIO_CHANGED },
  { "Status", STATUS_CHANGED },
  { "Features", STATUS_CHANGED },
  { "Country", STATUS_CHANGED },
  { "MissedCount", MISSED_CHANGED },
  { "CanRecord", STATUS_CHANGED },
};

static void
call_info_free (CallInfo *call)
{
  g_free (call->number);
  g_free (call->post_dial_wait);
  g_free (call);
}

static CallInfo *
call_from_variant (GVariant *dict)
{
  CallInfo *call = g_new0 (CallInfo, 1);
  const char *number = "";
  guint value;

  g_variant_lookup (dict, "id", "u", &call->id);
  g_variant_lookup (dict, "number", "&s", &number);
  call->number = g_strdup (number);
  call->presentation = g_variant_lookup (dict, "presentation", "u", &value) ? value : PRESENTATION_ALLOWED;
  call->direction = g_variant_lookup (dict, "direction", "u", &value) ? value : CALL_DIRECTION_UNKNOWN;
  call->state = g_variant_lookup (dict, "state", "u", &value) ? value : CALL_STATE_CONNECTING;
  call->end = g_variant_lookup (dict, "end", "u", &value) ? value : CALL_END_NONE;
  g_variant_lookup (dict, "created", "x", &call->created);
  g_variant_lookup (dict, "connected", "x", &call->connected);
  g_variant_lookup (dict, "multiparty", "b", &call->multiparty);
  g_variant_lookup (dict, "recording", "x", &call->recording);
  if (!g_variant_lookup (dict, "post-dial-wait", "s", &call->post_dial_wait))
    call->post_dial_wait = g_strdup ("");
  return call;
}

/* --- reading the properties */

static void
read_property (CallsClient *self, guint i, GVariant *value)
{
  switch (i)
    {
    case 0:
      g_ptr_array_set_size (self->calls, 0);
      if (value)
        {
          GVariantIter iter;
          GVariant *dict;

          g_variant_iter_init (&iter, value);
          while ((dict = g_variant_iter_next_value (&iter)))
            {
              g_ptr_array_add (self->calls, call_from_variant (dict));
              g_variant_unref (dict);
            }
        }
      break;
    case 1:
      self->muted = value && g_variant_get_boolean (value);
      break;
    case 2:
      self->speaker = value && g_variant_get_boolean (value);
      break;
    case 3:
      g_free (self->status);
      self->status = g_strdup (value ? g_variant_get_string (value, NULL) : "");
      break;
    case 4:
      g_strfreev (self->features);
      self->features = value ? g_variant_dup_strv (value, NULL) : g_new0 (char *, 1);
      break;
    case 5:
      g_free (self->country);
      self->country = value && *g_variant_get_string (value, NULL) ? g_variant_dup_string (value, NULL) : NULL;
      break;
    case 6:
      self->missed = value ? g_variant_get_uint32 (value) : 0;
      break;
    case 7:
      self->can_record = value && g_variant_get_boolean (value);
      break;
    }
}

/* every property again from the proxy's cache, and a signal for each kind that changed */
static void
refresh (CallsClient *self)
{
  gboolean changed[FAILED] = { FALSE };

  for (guint i = 0; i < G_N_ELEMENTS (properties); i++)
    {
      g_autoptr(GVariant) value = self->proxy ? g_dbus_proxy_get_cached_property (self->proxy, properties[i].name)
                                              : NULL;

      if (value == self->last[i] || (value && self->last[i] && g_variant_equal (value, self->last[i])))
        continue;
      g_clear_pointer (&self->last[i], g_variant_unref);
      self->last[i] = value ? g_variant_ref (value) : NULL;
      read_property (self, i, value);
      changed[properties[i].signal] = TRUE;
    }
  for (guint s = 0; s < FAILED; s++)
    if (changed[s])
      g_signal_emit (self, signals[s], 0);
}

static void
on_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, CallsClient *self)
{
  refresh (self);
}

static void
on_signal (GDBusProxy *proxy, const char *sender, const char *name, GVariant *parameters, CallsClient *self)
{
  if (g_str_equal (name, "CallLogChanged"))
    g_signal_emit (self, signals[CALL_LOG_CHANGED], 0);
  /* a recording asked for, or one of "Record every call", did not start after all: told as SetRecording's refusal */
  else if (g_str_equal (name, "RecordingFailed") && g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(us)")))
    {
      guint id;
      const char *message;

      g_variant_get (parameters, "(u&s)", &id, &message);
      g_signal_emit (self, signals[FAILED], 0, "SetRecording", id, message);
    }
}

/* the service went (it restarts on failure) or came back: its calls are none, then whatever it has now */
static void
on_owner (GDBusProxy *proxy, GParamSpec *pspec, CallsClient *self)
{
  refresh (self);
  g_signal_emit (self, signals[CALL_LOG_CHANGED], 0);
}

/* --- GObject */

static void
calls_client_dispose (GObject *object)
{
  CallsClient *self = CALLS_CLIENT (object);

  g_clear_object (&self->proxy);
  g_clear_pointer (&self->calls, g_ptr_array_unref);
  for (guint i = 0; i < G_N_ELEMENTS (self->last); i++)
    g_clear_pointer (&self->last[i], g_variant_unref);
  g_clear_pointer (&self->status, g_free);
  g_clear_pointer (&self->features, g_strfreev);
  g_clear_pointer (&self->country, g_free);
  G_OBJECT_CLASS (calls_client_parent_class)->dispose (object);
}

static void
calls_client_class_init (CallsClientClass *klass)
{
  static const char * const names[FAILED] = { "changed", "audio-changed", "status-changed", "missed-changed",
                                              "call-log-changed" };

  G_OBJECT_CLASS (klass)->dispose = calls_client_dispose;
  for (guint s = 0; s < FAILED; s++)
    signals[s] = g_signal_new (names[s], G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                               G_TYPE_NONE, 0);
  signals[FAILED] = g_signal_new ("failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);
}

static void
calls_client_init (CallsClient *self)
{
  g_autoptr(GError) error = NULL;

  self->calls = g_ptr_array_new_with_free_func ((GDestroyNotify) call_info_free);
  self->status = g_strdup ("");
  self->features = g_new0 (char *, 1);
  /* synchronous: the in-call screen has nothing to show before it, and the service answers at once */
  self->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL, CALLS_BUS_NAME,
                                               CALLS_PATH, CALLS_INTERFACE, NULL, &error);
  if (!self->proxy)
    {
      g_warning ("no session bus for the call service: %s", error->message);
      return;
    }
  g_signal_connect (self->proxy, "g-properties-changed", G_CALLBACK (on_properties), self);
  g_signal_connect (self->proxy, "g-signal", G_CALLBACK (on_signal), self);
  g_signal_connect (self->proxy, "notify::g-name-owner", G_CALLBACK (on_owner), self);
  refresh (self);
}

CallsClient *
calls_client_get (void)
{
  static CallsClient *client;

  if (!client)
    client = g_object_new (CALLS_TYPE_CLIENT, NULL);
  return client;
}

/* --- the calls */

GPtrArray *
calls_client_get_calls (CallsClient *self)
{
  return self->calls;
}

CallInfo *
calls_client_find (CallsClient *self, guint id)
{
  for (guint i = 0; i < self->calls->len; i++)
    if (((CallInfo *) self->calls->pdata[i])->id == id)
      return self->calls->pdata[i];
  return NULL;
}

static const CallState display_order[] = {
  CALL_STATE_INCOMING, CALL_STATE_CALL_WAITING, CALL_STATE_CONNECTING, CALL_STATE_DIALING, CALL_STATE_ACTIVE,
  CALL_STATE_DISCONNECTING, CALL_STATE_DISCONNECTED, CALL_STATE_ONHOLD,
};

static CallInfo *
first_in_order (CallsClient *self, const CallInfo *skip, gboolean skip_ended)
{
  for (guint o = 0; o < G_N_ELEMENTS (display_order); o++)
    {
      if (skip_ended && display_order[o] == CALL_STATE_DISCONNECTED)
        continue;
      for (guint i = 0; i < self->calls->len; i++)
        {
          CallInfo *call = self->calls->pdata[i];

          if (call != skip && call->state == display_order[o])
            return call;
        }
    }
  return NULL;
}

CallInfo *
calls_client_get_primary (CallsClient *self)
{
  return first_in_order (self, NULL, FALSE);
}

CallInfo *
calls_client_get_secondary (CallsClient *self, const CallInfo *primary)
{
  return primary ? first_in_order (self, primary, TRUE) : NULL;
}

gboolean
calls_client_get_muted (CallsClient *self)
{
  return self->muted;
}

gboolean
calls_client_get_speaker (CallsClient *self)
{
  return self->speaker;
}

const char *
calls_client_get_status (CallsClient *self)
{
  return self->status;
}

gboolean
calls_client_has_feature (CallsClient *self, const char *feature)
{
  return g_strv_contains ((const char * const *) self->features, feature);
}

const char *
calls_client_get_country (CallsClient *self)
{
  return self->country;
}

gboolean
calls_client_get_can_record (CallsClient *self)
{
  return self->can_record;
}

guint
calls_client_get_missed_count (CallsClient *self)
{
  return self->missed;
}

/* --- the asks */

typedef struct
{
  CallsClient *self;
  char *method;
  guint id;
} Asking;

static void
on_called (GObject *source, GAsyncResult *result, gpointer data)
{
  Asking *asking = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);

  if (!reply)
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("%s: %s", asking->method, error->message);
      g_signal_emit (asking->self, signals[FAILED], 0, asking->method, asking->id, error->message);
    }
  g_free (asking->method);
  g_free (asking);
}

/* @id: the call the ask is about, for "failed" (0 for none) */
static void
call (CallsClient *self, const char *method, guint id, GVariant *parameters)
{
  Asking *asking;

  if (!self->proxy)
    {
      if (parameters)
        g_variant_unref (g_variant_ref_sink (parameters));
      g_signal_emit (self, signals[FAILED], 0, method, id, "No call service");
      return;
    }
  asking = g_new0 (Asking, 1);
  asking->self = self;          /* the client lives as long as the process */
  asking->method = g_strdup (method);
  asking->id = id;
  g_dbus_proxy_call (self->proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_called, asking);
}

typedef struct
{
  CallsDialed dialed;
  gpointer data;
} Dialing;

static void
on_dialed (GObject *source, GAsyncResult *result, gpointer data)
{
  Dialing *dialing = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  guint id = 0;

  if (reply)
    g_variant_get (reply, "(u)", &id);
  else
    g_dbus_error_strip_remote_error (error);
  if (dialing->dialed)
    dialing->dialed (id, error, dialing->data);
  g_free (dialing);
}

void
calls_client_dial (CallsClient *self, const char *number, CallsDialed dialed, gpointer data)
{
  Dialing *dialing = g_new0 (Dialing, 1);

  dialing->dialed = dialed;
  dialing->data = data;
  if (!self->proxy)
    {
      g_autoptr(GError) error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No call service");

      if (dialed)
        dialed (0, error, data);
      g_free (dialing);
      return;
    }
  g_dbus_proxy_call (self->proxy, "Dial", g_variant_new ("(s)", number), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_dialed, dialing);
}

void
calls_client_answer (CallsClient *self, guint id)
{
  call (self, "Answer", id, g_variant_new ("(u)", id));
}

void
calls_client_answer_and_release (CallsClient *self, guint id)
{
  call (self, "AnswerAndRelease", id, g_variant_new ("(u)", id));
}

void
calls_client_decline (CallsClient *self, guint id)
{
  call (self, "Decline", id, g_variant_new ("(u)", id));
}

void
calls_client_hangup (CallsClient *self, guint id)
{
  call (self, "Hangup", id, g_variant_new ("(u)", id));
}

void
calls_client_set_held (CallsClient *self, guint id, gboolean held)
{
  call (self, "SetHeld", id, g_variant_new ("(ub)", id, held));
}

void
calls_client_send_dtmf (CallsClient *self, guint id, const char *tones)
{
  call (self, "SendDtmf", id, g_variant_new ("(us)", id, tones));
}

void
calls_client_continue_post_dial (CallsClient *self, guint id, gboolean proceed)
{
  call (self, "ContinuePostDial", id, g_variant_new ("(ub)", id, proceed));
}

void
calls_client_set_muted (CallsClient *self, gboolean muted)
{
  call (self, "SetMuted", 0, g_variant_new ("(b)", muted));
}

void
calls_client_set_recording (CallsClient *self, guint id, gboolean recording)
{
  call (self, "SetRecording", id, g_variant_new ("(ub)", id, recording));
}

void
calls_client_set_speaker (CallsClient *self, gboolean speaker)
{
  call (self, "SetSpeaker", 0, g_variant_new ("(b)", speaker));
}

void
calls_client_silence_ringer (CallsClient *self)
{
  call (self, "SilenceRinger", 0, NULL);
}

void
calls_client_preview_ringtone (CallsClient *self, const char *uri)
{
  call (self, "PreviewRingtone", 0, g_variant_new ("(s)", uri ? uri : ""));
}

void
calls_client_play_tone (CallsClient *self, char key)
{
  char tone[2] = { key, 0 };

  call (self, "PlayTone", 0, g_variant_new ("(s)", tone));
}

void
calls_client_stop_tone (CallsClient *self)
{
  call (self, "StopTone", 0, NULL);
}

void
calls_client_prepare_tones (CallsClient *self)
{
  call (self, "PrepareTones", 0, NULL);
}

void
calls_client_release_tones (CallsClient *self)
{
  call (self, "ReleaseTones", 0, NULL);
}

void
calls_client_set_incall_visible (CallsClient *self, gboolean visible)
{
  call (self, "SetInCallVisible", 0, g_variant_new ("(b)", visible));
}

/* --- the call log */

static void
on_call_log (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  g_autoptr(GVariant) rows = NULL;
  GPtrArray *entries;
  GVariantIter iter;
  GVariant *dict;

  if (!reply)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  entries = g_ptr_array_new_with_free_func ((GDestroyNotify) call_log_entry_free);
  rows = g_variant_get_child_value (reply, 0);
  g_variant_iter_init (&iter, rows);
  while ((dict = g_variant_iter_next_value (&iter)))
    {
      g_ptr_array_add (entries, call_log_entry_from_variant (dict));
      g_variant_unref (dict);
    }
  g_task_return_pointer (task, entries, (GDestroyNotify) g_ptr_array_unref);
}

void
calls_client_get_call_log (CallsClient *self, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new (self, cancellable, callback, data);

  if (!self->proxy)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No call service");
      g_object_unref (task);
      return;
    }
  g_dbus_proxy_call (self->proxy, "GetCallLog", NULL, G_DBUS_CALL_FLAGS_NONE, -1, cancellable, on_call_log, task);
}

GPtrArray *
calls_client_get_call_log_finish (CallsClient *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
calls_client_delete_call_log (CallsClient *self, const gint64 *ids, guint n)
{
  call (self, "DeleteCallLog", 0,
        g_variant_new ("(@ax)", g_variant_new_fixed_array (G_VARIANT_TYPE_INT64, ids, n, sizeof *ids)));
}

void
calls_client_clear_call_log (CallsClient *self)
{
  call (self, "ClearCallLog", 0, NULL);
}

void
calls_client_mark_missed_read (CallsClient *self)
{
  call (self, "MarkMissedRead", 0, NULL);
}

void
calls_client_mark_calls_read (CallsClient *self, const gint64 *ids, guint n)
{
  call (self, "MarkCallsRead", 0,
        g_variant_new ("(@ax)", g_variant_new_fixed_array (G_VARIANT_TYPE_INT64, ids, n, sizeof *ids)));
}
