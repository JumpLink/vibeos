/* The calls (calls.h). */
#include "calls.h"
#include "blocked-numbers.h"
#include "call-notifications.h"
#include "contacts.h"
#include "modem.h"
#include "phone.h"
#include "phone-number.h"
#include "ringer.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

#define CALL_INTERFACE "org.freedesktop.ModemManager1.Call"
#define MM_NAME "org.freedesktop.ModemManager1"
#define CALLAUDIO_NAME "org.mobian_project.CallAudio"
#define CALLAUDIO_PATH "/org/mobian_project/CallAudio"
#define POST_DIAL_PAUSE_MS 3000 /* the dialpad's "2-sec pause" lasts 3 s */

/* ModemManager's MMCallState and MMCallStateReason */
enum {
  MM_CALL_STATE_UNKNOWN = 0,
  MM_CALL_STATE_DIALING = 1,
  MM_CALL_STATE_RINGING_OUT = 2,
  MM_CALL_STATE_RINGING_IN = 3,
  MM_CALL_STATE_ACTIVE = 4,
  MM_CALL_STATE_HELD = 5,
  MM_CALL_STATE_WAITING = 6,
  MM_CALL_STATE_TERMINATED = 7,
};
enum {
  MM_CALL_REASON_REFUSED_OR_BUSY = 5,
  MM_CALL_REASON_ERROR = 6,
  MM_CALL_REASON_AUDIO_SETUP_FAILED = 7,
};

/* An answer that has to wait until other calls are gone. A modem without +CHLD cannot hold the call that is up, so it
 * ends, and the waiting call (RINGING_IN once it is alone) is accepted; and GSM holds one call at most, so with one
 * on hold already, that one ends before the call that is up is held. */
typedef enum {
  ANSWER_NOW = 0,
  ANSWER_WHEN_ALONE,            /* Accept, once the others have ended and it rings as a call of its own */
  ANSWER_WHEN_NONE_HELD,        /* HoldAndAccept, once no call is on hold */
} AnswerAfter;

typedef struct {
  Calls *owner;
  guint id;                     /* ours, for the apps: stays the same whatever ModemManager does */
  char *path;                   /* ModemManager's call object; NULL until CreateCall answers */
  GDBusProxy *proxy;
  GCancellable *cancellable;
  CallDirection direction;
  char *number;
  Presentation presentation;
  CallState state;
  CallEnd end;
  int mm_reason;
  gint64 created;               /* Unix ms: when it began, the call log's date */
  gint64 connected;             /* Unix ms: when it was first up, 0 before */
  gboolean hangup_requested;    /* we asked for the hang-up, so it ended here (LOCAL, CANCELED or REJECTED) */
  gboolean multiparty;
  /* the post-dial: what came after the number (tones, "," a pause, ";" a wait), sent once the call is up */
  char *post_dial;
  guint post_dial_at;           /* the next character of it */
  guint post_dial_id;           /* a pause running */
  gboolean post_dial_sending;   /* tones on their way to the modem */
  gboolean post_dial_waiting;   /* at a wait: the in-call screen asks "Send the following tones?" */
  gboolean announced;           /* it rang and its screen was asked for */
  gboolean blocked;             /* from a blocked number: declined unseen, logged as "Blocked call" */
  AnswerAfter answer_after;     /* answered, the other calls going first */
  guint remove_id;              /* an ended call stays a moment */
} Call;

struct _Calls {
  GDBusConnection *session, *system;
  GSettings *settings;
  CallLog *log;
  CallsCallbacks callbacks;
  gpointer data;

  Modem *modem;
  Ringer *ringer;
  CallNotifications *notifications;
  Contacts *contacts;
  BlockedNumbers *blocked;      /* the numbers whose calls are declined unseen (blocked-numbers.h) */
  GPtrArray *calls;             /* Call, oldest first */
  GPtrArray *deferred;          /* call paths that came while one of ours waited for its path (char *) */
  guint next_id;

  gboolean call_audio;          /* the call audio is switched in (CallAudio's mode 1) */
  gboolean muted, speaker;
  gboolean gateway;             /* gsm2sip has the modem */
  gboolean incall_visible;      /* the in-call screen is in front */
  guint full_screen_call;       /* the call whose answer screen the shell put up */
  guint silenced_call;          /* the call whose ringing was silenced */
  gboolean can_hold;            /* the modem answered the hold probe */
  gboolean probed;
  gboolean dtmf_unsupported;    /* the modem said it cannot send tones */
  guint vibrate_45_id;
  guint active_id;              /* the call the in-call vibrations count as active */
  gint64 active_connected;
  gboolean active_was_outgoing;
};

static void sync_calls (Calls *self);
static void track_call (Calls *self, Call *call);
static void resume_answer (Calls *self);

/* --- the calls */

static void
call_free (Call *call)
{
  g_cancellable_cancel (call->cancellable);
  if (call->proxy)
    g_signal_handlers_disconnect_by_data (call->proxy, call);
  g_clear_object (&call->proxy);
  g_clear_object (&call->cancellable);
  g_clear_handle_id (&call->remove_id, g_source_remove);
  g_clear_handle_id (&call->post_dial_id, g_source_remove);
  g_free (call->path);
  g_free (call->number);
  g_free (call->post_dial);
  g_free (call);
}

static Call *
find_call (Calls *self, guint id)
{
  for (guint i = 0; i < self->calls->len; i++)
    if (((Call *) self->calls->pdata[i])->id == id)
      return self->calls->pdata[i];
  return NULL;
}

static Call *
find_call_by_path (Calls *self, const char *path)
{
  for (guint i = 0; i < self->calls->len; i++)
    if (g_strcmp0 (((Call *) self->calls->pdata[i])->path, path) == 0)
      return self->calls->pdata[i];
  return NULL;
}

static Call *
new_call (Calls *self, CallDirection direction, const char *number)
{
  Call *call = g_new0 (Call, 1);

  call->owner = self;
  call->id = ++self->next_id;
  call->direction = direction;
  call->number = g_strdup (number ? number : "");
  call->presentation = phone_number_presentation (call->number, PRESENTATION_ALLOWED);
  call->state = CALL_STATE_CONNECTING;
  call->created = g_get_real_time () / 1000;
  call->cancellable = g_cancellable_new ();
  g_ptr_array_add (self->calls, call);
  return call;
}

static gboolean
is_ringing (const Call *call)
{
  return call->state == CALL_STATE_INCOMING || call->state == CALL_STATE_CALL_WAITING;
}

static gboolean
is_live (const Call *call)
{
  return call->state != CALL_STATE_DISCONNECTED;
}

/* what a call is called: a contact's name, "Private number", or the number as it reads in this country */
static char *
call_title (Calls *self, const Call *call)
{
  const char *country = calls_get_country (self);
  const Contact *contact = contacts_lookup (self->contacts, call->number, *country ? country : NULL);
  const char *presentation = phone_number_presentation_name (call->presentation);

  if (contact && contact->name && *contact->name)
    return g_strdup (contact->name);
  if (presentation)
    return g_strdup (presentation);
  return phone_number_format (call->number, *country ? country : NULL);
}

/* --- what the modem is learned to do, kept by its model (a modem that learns to hold gets a new revision) */

static char *
learned_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "neo-dialer", "modem.ini", NULL);
}

static void
load_learned (Calls *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *path = learned_path ();
  g_autofree char *model = modem_get_model (self->modem);

  self->dtmf_unsupported = FALSE;
  if (model && g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
    self->dtmf_unsupported = g_key_file_get_boolean (file, model, "dtmf-unsupported", NULL);
}

static void
learn_dtmf_unsupported (Calls *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *path = learned_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *model = modem_get_model (self->modem);

  self->dtmf_unsupported = TRUE;
  if (!model)
    return;
  g_mkdir_with_parents (dir, 0700);
  g_key_file_load_from_file (file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_key_file_set_boolean (file, model, "dtmf-unsupported", TRUE);
  g_key_file_save_to_file (file, path, NULL);
  g_message ("calls: the modem cannot send tones (DTMF): the in-call keypad is off for it");
}

/* GSM's hold is +CHLD=2, which with no call up holds nothing and accepts nothing: a modem that does not know the
 * command at all answers "not supported", and then the Hold button stays disabled. Asked
 * only with no call up, so it can never touch a call. */
static void
on_hold_probe (GObject *source, GAsyncResult *result, gpointer data)
{
  Calls *self = data;
  g_autoptr(GError) error = NULL;

  self->can_hold = modem_voice_method_finish (self->modem, result, &error) || !modem_error_is_unsupported (error);
  g_message ("calls: the modem %s hold calls", self->can_hold ? "can" : "cannot");
  self->callbacks.changed (self->data);
}

static void
probe_modem (Calls *self)
{
  if (self->probed || self->gateway || !modem_is_available (self->modem) || self->calls->len > 0)
    return;
  self->probed = TRUE;
  load_learned (self);
  modem_voice_method (self->modem, "HoldAndAccept", on_hold_probe, self);
}

/* --- the call audio: callaudiod's org.mobian_project.CallAudio (Debian's callaudiod, or any daemon with its API) */

static void
call_audio (Calls *self, const char *method, GVariant *parameters)
{
  g_dbus_connection_call (self->session, CALLAUDIO_NAME, CALLAUDIO_PATH, CALLAUDIO_NAME, method, parameters,
                          G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* in call mode from dialling until the last call is over: the modem's voice path through the earpiece and the
 * mics. Not while a call only rings: the ringtone needs the phone's own sound */
static void
sync_call_audio (Calls *self)
{
  gboolean needed = FALSE;

  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];

      if (call->state == CALL_STATE_DIALING || call->state == CALL_STATE_ACTIVE || call->state == CALL_STATE_ONHOLD
          || (call->state == CALL_STATE_DISCONNECTING && call->connected))
        needed = TRUE;
    }
  if (needed == self->call_audio)
    return;
  self->call_audio = needed;
  call_audio (self, "SelectMode", g_variant_new ("(u)", needed ? 1 : 0));
  if (!needed)
    {
      /* the next call starts on the earpiece, unmuted (CallAudio puts both back as it leaves call mode) */
      self->muted = FALSE;
      self->speaker = FALSE;
    }
}

/* --- the in-call vibrations, each behind its setting */

static gboolean
vibrate_45 (gpointer data)
{
  Calls *self = data;

  ringer_vibrate (self->ringer, 70, 0, 0);
  self->vibrate_45_id = g_timeout_add_seconds (60, vibrate_45, self);
  return G_SOURCE_REMOVE;
}

static void
sync_vibrations (Calls *self)
{
  Call *active = NULL, *incoming = NULL;
  gint64 now = g_get_real_time () / 1000;

  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];

      if (call->state == CALL_STATE_ACTIVE && !active)
        active = call;
      if (is_ringing (call) && !incoming)
        incoming = call;
    }
  if (active && !self->active_id)
    {
      /* a call went up: "Vibrate on answer" and "Vibrate every minute" are for outgoing calls */
      self->active_id = active->id;
      self->active_connected = active->connected;
      self->active_was_outgoing = active->direction == CALL_DIRECTION_OUTGOING;
      if (self->active_was_outgoing && g_settings_get_boolean (self->settings, "incall-vibrate-outgoing")
          && now - active->connected < 200)
        ringer_vibrate (self->ringer, 100, 200, 0);
      if (self->active_was_outgoing && g_settings_get_boolean (self->settings, "incall-vibrate-45secs"))
        {
          gint64 into_minute = (now - active->connected) % 60000;

          self->vibrate_45_id = g_timeout_add (into_minute > 45000 ? 105000 - into_minute : 45000 - into_minute,
                                               vibrate_45, self);
        }
    }
  else if (!active && self->active_id)
    {
      /* it ended: "Vibrate on hang up" when the other side hung up after more than half a second */
      Call *ended = find_call (self, self->active_id);

      if (g_settings_get_boolean (self->settings, "incall-vibrate-hangup") && ended && !ended->hangup_requested
          && now - self->active_connected > 500)
        ringer_vibrate (self->ringer, 50, 100, 50);
      g_clear_handle_id (&self->vibrate_45_id, g_source_remove);
      self->active_id = 0;
    }
}

/* --- ringing and the answer screen */

static void
on_full_screen (gboolean shown, gpointer data)
{
  Calls *self = data;

  /* shown: its notification need not peek over it */
  if (!shown)
    self->full_screen_call = 0;
  sync_calls (self);
}

/* a call starts to ring: the answer screen over the lock screen, or the heads-up if the phone is in use */
static void
announce (Calls *self, Call *call)
{
  call->announced = TRUE;
  if (call->state == CALL_STATE_CALL_WAITING && g_settings_get_boolean (self->settings,
                                                                        "incall-vibrate-call-waiting"))
    ringer_vibrate (self->ringer, 200, 300, 500);
  self->full_screen_call = call->id;
  phone_show_full_screen (self->session, INCALL_APP_ID, on_full_screen, self);
}

static void
sync_ringer (Calls *self)
{
  Call *incoming = NULL;
  gboolean busy = FALSE;

  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];

      if (call->state == CALL_STATE_INCOMING && !incoming)
        incoming = call;
      else if (is_live (call) && !is_ringing (call))
        busy = TRUE;
    }
  /* an incoming call rings while nothing else is up; a waiting one only beeps (ringer.h) */
  if (incoming && !busy && incoming->id != self->silenced_call)
    ringer_start (self->ringer);
  else
    ringer_stop (self->ringer);
}

/* --- the notification of the call in front */

/* the call to show: incoming, then outgoing, then active, then held */
static Call *
call_to_display (Calls *self)
{
  static const CallState order[] = { CALL_STATE_INCOMING, CALL_STATE_CALL_WAITING, CALL_STATE_CONNECTING,
                                     CALL_STATE_DIALING, CALL_STATE_ACTIVE, CALL_STATE_DISCONNECTING,
                                     CALL_STATE_ONHOLD };

  for (guint o = 0; o < G_N_ELEMENTS (order); o++)
    for (guint i = 0; i < self->calls->len; i++)
      if (((Call *) self->calls->pdata[i])->state == order[o])
        return self->calls->pdata[i];
  return NULL;
}

static void
sync_notification (Calls *self)
{
  Call *call = call_to_display (self);
  g_autofree char *title = NULL;
  CallNotificationKind kind;

  if (!call)
    {
      call_notifications_clear_call (self->notifications);
      return;
    }
  title = call_title (self, call);
  switch (call->state)
    {
    case CALL_STATE_INCOMING:
    case CALL_STATE_CALL_WAITING:
      kind = self->incall_visible || self->full_screen_call == call->id ? CALL_NOTIFICATION_INCOMING_QUIET
                                                                        : CALL_NOTIFICATION_INCOMING;
      break;
    case CALL_STATE_CONNECTING:
    case CALL_STATE_DIALING:
      kind = CALL_NOTIFICATION_DIALING;
      break;
    case CALL_STATE_ONHOLD:
      kind = CALL_NOTIFICATION_HELD;
      break;
    default:
      kind = CALL_NOTIFICATION_ONGOING;
      break;
    }
  call_notifications_show_call (self->notifications, call->id, kind, title, call->connected, self->speaker);
}

/* everything that follows from the calls as they are now */
static void
sync_calls (Calls *self)
{
  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];

      if (is_ringing (call) && !call->announced)
        announce (self, call);
    }
  resume_answer (self);
  sync_ringer (self);
  sync_call_audio (self);
  sync_vibrations (self);
  sync_notification (self);
  self->callbacks.changed (self->data);
}

/* --- a call ends */

static gboolean
remove_call (gpointer data)
{
  Call *call = data;
  Calls *self = call->owner;

  call->remove_id = 0;
  if (call->id == self->full_screen_call)
    self->full_screen_call = 0;
  g_ptr_array_remove (self->calls, call);
  sync_calls (self);
  probe_modem (self);
  return G_SOURCE_REMOVE;
}

static CallEnd
end_cause (const Call *call)
{
  if (call->direction == CALL_DIRECTION_INCOMING && !call->connected)
    return call->hangup_requested ? CALL_END_REJECTED : CALL_END_MISSED;
  if (call->mm_reason == MM_CALL_REASON_ERROR || call->mm_reason == MM_CALL_REASON_AUDIO_SETUP_FAILED)
    return CALL_END_ERROR;
  if (call->hangup_requested)
    return call->connected || call->direction == CALL_DIRECTION_INCOMING ? CALL_END_LOCAL : CALL_END_CANCELED;
  if (call->mm_reason == MM_CALL_REASON_REFUSED_OR_BUSY && !call->connected)
    return CALL_END_BUSY;
  return CALL_END_REMOTE;
}

/* how long "Call ended" (or why) stays */
static guint
end_delay (CallEnd end)
{
  switch (end)
    {
    case CALL_END_LOCAL:
      return 200;
    case CALL_END_REMOTE:
    case CALL_END_ERROR:
      return 2000;
    case CALL_END_REJECTED:
    case CALL_END_MISSED:
    case CALL_END_CANCELED:
      return 0;
    default:
      return 5000;
    }
}

static void
log_call (Calls *self, Call *call)
{
  gint64 now = g_get_real_time () / 1000;
  const char *country = calls_get_country (self);
  /* the number as it was dialled, its pauses and tones too */
  g_autofree char *dialled = call->post_dial ? g_strconcat (call->number, call->post_dial, NULL) : NULL;
  CallLogEntry entry = {
    .number = dialled ? dialled : call->number,
    .date = call->created,
    .duration = call->connected ? (now - call->connected + 500) / 1000 : 0,
    .presentation = call->presentation,
    .country_iso = *country ? (char *) country : NULL,
    .is_read = TRUE,
  };
  gint64 id;

  if (call->direction == CALL_DIRECTION_OUTGOING)
    entry.type = CALL_TYPE_OUTGOING;
  else if (call->blocked)
    entry.type = CALL_TYPE_BLOCKED;
  else if (call->end == CALL_END_MISSED)
    {
      entry.type = CALL_TYPE_MISSED;
      entry.is_read = FALSE;
    }
  else if (call->end == CALL_END_REJECTED)
    entry.type = CALL_TYPE_REJECTED;
  else
    entry.type = CALL_TYPE_INCOMING;
  id = call_log_add (self->log, &entry);
  if (id && entry.type == CALL_TYPE_MISSED)
    {
      g_autofree char *who = call_title (self, call);

      call_notifications_show_missed (self->notifications, id, who,
                                      call->presentation == PRESENTATION_ALLOWED ? call->number : NULL);
    }
  self->callbacks.log_changed (self->data);
}

static void
end_call (Calls *self, Call *call)
{
  if (call->state == CALL_STATE_DISCONNECTED)
    return;
  call->state = CALL_STATE_DISCONNECTED;
  call->end = end_cause (call);
  g_message ("calls: call %u ended (%d)", call->id, call->end);
  log_call (self, call);
  /* ModemManager keeps a call that ended until someone deletes it */
  if (call->path && modem_is_available (self->modem))
    modem_delete_call (self->modem, call->path);
  call->remove_id = g_timeout_add (end_delay (call->end), remove_call, call);
  sync_calls (self);
}

/* --- a call from a blocked number */

typedef struct {
  Calls *self;
  char *path;
} BlockedHangup;

static void
on_blocked_hung_up (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  BlockedHangup *hangup = data;

  /* ModemManager keeps a call that ended until someone deletes it; once hung up, it is ours to delete */
  if (modem_is_available (hangup->self->modem))
    modem_delete_call (hangup->self->modem, hangup->path);
  g_free (hangup->path);
  g_free (hangup);
}

/* a blocked number: the call is declined before it rings, never
 * shown, never notified, and logged as "Blocked call". If its number came only after it started
 * ringing (the +CLIP can come after the first RING), it is taken away the moment the number shows who it is. */
static void
block_call (Calls *self, Call *call)
{
  g_message ("calls: call %u is from a blocked number: declined", call->id);
  call->blocked = TRUE;
  call->state = CALL_STATE_DISCONNECTED;
  call->end = CALL_END_REJECTED;
  log_call (self, call);
  if (call->path)
    {
      BlockedHangup *hangup = g_new0 (BlockedHangup, 1);

      hangup->self = self;
      hangup->path = g_strdup (call->path);
      /* not through call_method: that one goes with the call's cancellable, cancelled as the call is freed below */
      g_dbus_connection_call (self->system, MM_NAME, call->path, CALL_INTERFACE, "Hangup", NULL, NULL,
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_blocked_hung_up, hangup);
    }
  if (call->id == self->full_screen_call)
    self->full_screen_call = 0;
  g_ptr_array_remove (self->calls, call);
  sync_calls (self);
}

/* --- post-dial: the tones after the number. Once the call is up they go out in order: a run of tones in one SendDtmf,
 * a "," as a pause of 3 s, a ";" as a wait that the in-call screen turns into "Send the following tones?" (Yes goes on,
 * No drops the rest). A call on hold waits until it is up again. */

static void post_dial_next (Call *call);

static gboolean
on_post_dial_pause (gpointer data)
{
  Call *call = data;

  call->post_dial_id = 0;
  post_dial_next (call);
  return G_SOURCE_REMOVE;
}

static void
on_post_dial_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  Call *call = data;
  Calls *self;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;                     /* the call is gone, and @call with it */
  self = call->owner;
  call->post_dial_sending = FALSE;
  if (!reply)
    {
      g_warning ("calls: cannot send the tones after the number: %s", error->message);
      if (modem_error_is_unsupported (error) && !self->dtmf_unsupported)
        {
          learn_dtmf_unsupported (self);
          self->callbacks.changed (self->data);
        }
      /* the rest would fare no better */
      call->post_dial_at = strlen (call->post_dial);
      return;
    }
  post_dial_next (call);
}

static void
post_dial_next (Call *call)
{
  Calls *self = call->owner;
  g_autofree char *tones = NULL;
  const char *rest;
  size_t run;

  if (!call->post_dial || call->post_dial_id || call->post_dial_sending || call->post_dial_waiting)
    return;
  if (call->state != CALL_STATE_ACTIVE || !call->path || call->hangup_requested)
    return;
  rest = call->post_dial + call->post_dial_at;
  if (!*rest)
    return;
  if (*rest == ',')
    {
      call->post_dial_at++;
      call->post_dial_id = g_timeout_add (POST_DIAL_PAUSE_MS, on_post_dial_pause, call);
      return;
    }
  if (*rest == ';')
    {
      call->post_dial_at++;
      if (call->post_dial[call->post_dial_at])
        {
          call->post_dial_waiting = TRUE;
          sync_calls (self);
        }
      return;
    }
  run = strcspn (rest, ",;");
  tones = g_strndup (rest, run);
  call->post_dial_sending = TRUE;
  call->post_dial_at += run;
  g_dbus_connection_call (self->system, MM_NAME, call->path, CALL_INTERFACE, "SendDtmf", g_variant_new ("(s)", tones),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, call->cancellable, on_post_dial_sent, call);
}

/* --- following ModemManager's call */

static void
update_from_modem (Calls *self, Call *call)
{
  g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property (call->proxy, "State");
  g_autoptr(GVariant) reason = g_dbus_proxy_get_cached_property (call->proxy, "StateReason");
  g_autoptr(GVariant) number = g_dbus_proxy_get_cached_property (call->proxy, "Number");
  g_autoptr(GVariant) multiparty = g_dbus_proxy_get_cached_property (call->proxy, "Multiparty");
  int mm_state = state ? g_variant_get_int32 (state) : MM_CALL_STATE_UNKNOWN;
  CallState next;

  if (call->state == CALL_STATE_DISCONNECTED)
    return;
  call->mm_reason = reason ? g_variant_get_int32 (reason) : 0;
  call->multiparty = multiparty && g_variant_get_boolean (multiparty);
  /* the caller's number comes with the +CLIP after the first RING: it may arrive after the call */
  if (number && *g_variant_get_string (number, NULL) && g_strcmp0 (call->number, g_variant_get_string (number, NULL)))
    {
      g_free (call->number);
      call->number = g_variant_dup_string (number, NULL);
      call->presentation = phone_number_presentation (call->number, PRESENTATION_ALLOWED);
    }

  if (call->direction == CALL_DIRECTION_INCOMING
      && (mm_state == MM_CALL_STATE_RINGING_IN || mm_state == MM_CALL_STATE_WAITING)
      && call->presentation == PRESENTATION_ALLOWED
      && blocked_numbers_contains (self->blocked, call->number, calls_get_country (self)))
    {
      block_call (self, call);
      return;
    }

  switch (mm_state)
    {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
      next = CALL_STATE_DIALING;
      break;
    case MM_CALL_STATE_RINGING_IN:
      next = CALL_STATE_INCOMING;
      break;
    case MM_CALL_STATE_WAITING:
      next = CALL_STATE_CALL_WAITING;
      break;
    case MM_CALL_STATE_ACTIVE:
      next = CALL_STATE_ACTIVE;
      break;
    case MM_CALL_STATE_HELD:
      next = CALL_STATE_ONHOLD;
      break;
    case MM_CALL_STATE_TERMINATED:
      end_call (self, call);
      return;
    default:
      next = call->state;
      break;
    }
  /* hung up by us and not over yet: "Hanging up" (a declined call just goes) */
  if (call->hangup_requested && !is_ringing (call))
    next = CALL_STATE_DISCONNECTING;
  if (next == CALL_STATE_ACTIVE && !call->connected)
    call->connected = g_get_real_time () / 1000;
  call->state = next;
  sync_calls (self);
  /* up: the tones after the number go out (or go on, after a hold) */
  post_dial_next (call);
}

static void
on_call_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  Call *call = data;

  update_from_modem (call->owner, call);
}

static void
on_call_signal (GDBusProxy *proxy, const char *sender, const char *signal, GVariant *parameters, gpointer data)
{
  Call *call = data;

  /* StateChanged (old, new, reason) comes before the properties change: the reason with it is the one that counts */
  if (g_str_equal (signal, "StateChanged"))
    {
      int old_state, new_state;
      guint reason;

      g_variant_get (parameters, "(iiu)", &old_state, &new_state, &reason);
      call->mm_reason = reason;
      if (new_state == MM_CALL_STATE_TERMINATED)
        end_call (call->owner, call);
    }
}

static void
on_call_proxy (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_finish (result, &error);
  Call *call = data;
  g_autoptr(GVariant) direction = NULL;

  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("calls: cannot follow %s: %s", call->path, error->message);
          end_call (call->owner, call);
        }
      return;
    }
  call->proxy = proxy;
  direction = g_dbus_proxy_get_cached_property (proxy, "Direction");
  if (direction && g_variant_get_int32 (direction))
    call->direction = g_variant_get_int32 (direction);
  g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_call_properties), call);
  g_signal_connect (proxy, "g-signal", G_CALLBACK (on_call_signal), call);
  update_from_modem (call->owner, call);
}

static void
track_call (Calls *self, Call *call)
{
  g_dbus_proxy_new (self->system, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, MM_NAME, call->path, CALL_INTERFACE,
                    call->cancellable, on_call_proxy, call);
}

static gboolean
awaiting_path (Calls *self)
{
  for (guint i = 0; i < self->calls->len; i++)
    if (!((Call *) self->calls->pdata[i])->path && is_live (self->calls->pdata[i]))
      return TRUE;
  return FALSE;
}

static void
on_call_added (const char *path, gpointer data)
{
  Calls *self = data;
  Call *call;

  if (self->gateway || find_call_by_path (self, path))
    return;
  /* a call we dialled comes as CallAdded and as CreateCall's answer, in either order: while one of ours waits for
   * its answer, a new call waits too, so that it is not taken for another */
  if (awaiting_path (self))
    {
      g_ptr_array_add (self->deferred, g_strdup (path));
      return;
    }
  /* someone else's call (an incoming one; one dialled with mmcli): its direction comes with its proxy */
  call = new_call (self, CALL_DIRECTION_UNKNOWN, NULL);
  call->path = g_strdup (path);
  track_call (self, call);
}

static void
on_call_deleted (const char *path, gpointer data)
{
  Calls *self = data;
  Call *call = find_call_by_path (self, path);

  /* deleted by someone else before it ended: it is over for us too */
  if (call && is_live (call))
    end_call (self, call);
}

static void
take_deferred (Calls *self)
{
  g_autoptr(GPtrArray) deferred = g_steal_pointer (&self->deferred);

  self->deferred = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < deferred->len; i++)
    on_call_added (deferred->pdata[i], self);
}

static void
on_available_changed (gboolean available, gpointer data)
{
  Calls *self = data;

  if (!available)
    {
      /* the modem went with its calls: each is over */
      for (guint i = 0; i < self->calls->len; i++)
        if (is_live (self->calls->pdata[i]))
          end_call (self, self->calls->pdata[i]);
      self->probed = FALSE;
      self->can_hold = FALSE;
    }
  else
    probe_modem (self);
  self->callbacks.changed (self->data);
}

/* --- the user's asks */

static void
on_started (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  Call *call = data;

  if (!reply && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_warning ("calls: cannot dial: %s", error->message);
      call->mm_reason = MM_CALL_REASON_ERROR;
      end_call (call->owner, call);
    }
}

static void
on_created (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  Call *call = data;
  Calls *self;
  g_autofree char *path = modem_create_call_finish (NULL, result, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  self = call->owner;
  if (!path)
    {
      g_warning ("calls: cannot make the call: %s", error->message);
      call->mm_reason = MM_CALL_REASON_ERROR;
      end_call (self, call);
      take_deferred (self);
      return;
    }
  call->path = g_steal_pointer (&path);
  /* our own call's CallAdded is among the deferred ones: it now has its call */
  take_deferred (self);
  track_call (self, call);
  /* hung up while it was being made: the call object goes without ever dialling */
  if (call->hangup_requested)
    {
      call->mm_reason = 0;
      end_call (self, call);
      return;
    }
  g_dbus_connection_call (self->system, MM_NAME, call->path, CALL_INTERFACE, "Start", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, call->cancellable, on_started, call);
}

static gboolean
check_ready (Calls *self, GError **error)
{
  if (self->gateway)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY, _("The phone is in gateway mode: calls go over SIP"));
  else if (!modem_is_available (self->modem))
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, _("Mobile network not available"));
  else
    return TRUE;
  return FALSE;
}

/* what follows the number, from its first "," or ";": tones, pauses and waits (the dialpad's "Add 2-sec pause"
 * and "Add wait"), or NULL */
static char *
post_dial_of (const char *number)
{
  const char *start = strpbrk (number, ",;");
  GString *out;

  if (!start)
    return NULL;
  out = g_string_new (NULL);
  for (const char *c = start; *c; c++)
    if (g_ascii_isdigit (*c) || strchr ("*#,;", *c))
      g_string_append_c (out, *c);
  return g_string_free (out, FALSE);
}

guint
calls_dial (Calls *self, const char *number, GError **error)
{
  /* the network part goes to the modem; the rest is post-dial (a modem's ATD would take a "," as its own pause, a
   * QMI or MBIM one would refuse it) */
  g_autofree char *post_dial = post_dial_of (number ? number : "");
  const char *start = post_dial ? strpbrk (number, ",;") : NULL;
  g_autofree char *network = start ? g_strndup (number, start - number) : g_strdup (number);
  g_autofree char *digits = phone_number_normalize (network);
  Call *call;

  if (!check_ready (self, error))
    return 0;
  if (!*digits)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("No number to call"));
      return 0;
    }
  /* no SIM, or no network of its own: only the emergency numbers go through (and the modem knows which) */
  if (modem_get_emergency_only (self->modem) && !phone_number_is_emergency (digits))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, _("Emergency calls only"));
      return 0;
    }
  call = new_call (self, CALL_DIRECTION_OUTGOING, digits);
  if (post_dial && *post_dial)
    call->post_dial = g_steal_pointer (&post_dial);
  g_message ("calls: dialling (call %u)", call->id);
  modem_create_call (self->modem, digits, call->cancellable, on_created, call);
  sync_calls (self);
  return call->id;
}

static Call *
live_call (Calls *self, guint id, GError **error)
{
  Call *call = find_call (self, id);

  if (!call || !is_live (call))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "There is no call %u", id);
      return NULL;
    }
  return call;
}

static void
on_call_method (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autofree char *method = data;

  if (!reply && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("calls: %s failed: %s", method, error->message);
}

static void
call_method (Calls *self, Call *call, const char *method, GVariant *parameters)
{
  g_dbus_connection_call (self->system, MM_NAME, call->path, CALL_INTERFACE, method, parameters, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, call->cancellable, on_call_method, g_strdup (method));
}

static void
on_voice_method (GObject *source, GAsyncResult *result, gpointer data)
{
  Calls *self = data;
  g_autoptr(GError) error = NULL;

  if (!modem_voice_method_finish (self->modem, result, &error))
    g_warning ("calls: %s", error->message);
}

/* ends every call that is up or held but @keep (and, with @held_only, only the held ones) */
static void
end_others (Calls *self, Call *keep, gboolean held_only)
{
  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *other = self->calls->pdata[i];

      if (other == keep || !is_live (other) || is_ringing (other) || other->hangup_requested
          || (held_only && other->state != CALL_STATE_ONHOLD))
        continue;
      other->hangup_requested = TRUE;
      if (other->path)
        call_method (self, other, "Hangup", NULL);
    }
}

static void
resume_answer (Calls *self)
{
  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];
      gboolean waiting_on_others = FALSE;

      if (call->answer_after == ANSWER_NOW)
        continue;
      /* the caller gave up meanwhile, or it was declined */
      if (!is_ringing (call))
        {
          call->answer_after = ANSWER_NOW;
          continue;
        }
      for (guint j = 0; j < self->calls->len; j++)
        {
          Call *other = self->calls->pdata[j];

          if (other != call && is_live (other) && !is_ringing (other)
              && (call->answer_after == ANSWER_WHEN_ALONE || other->state == CALL_STATE_ONHOLD))
            waiting_on_others = TRUE;
        }
      if (waiting_on_others)
        continue;
      if (call->answer_after == ANSWER_WHEN_NONE_HELD)
        modem_voice_method (self->modem, "HoldAndAccept", on_voice_method, self);
      else if (call->state == CALL_STATE_INCOMING)
        call_method (self, call, "Accept", NULL);
      else
        continue;               /* still WAITING: Accept takes only a ringing call */
      call->answer_after = ANSWER_NOW;
    }
}

gboolean
calls_answer (Calls *self, guint id, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  if (!is_ringing (call) || !call->path)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Call %u is not ringing", id);
      return FALSE;
    }
  /* the ringtone first: the call's audio takes the phone's sound away from it */
  ringer_stop (self->ringer);
  self->silenced_call = call->id;
  if (call->state != CALL_STATE_CALL_WAITING)
    call_method (self, call, "Accept", NULL);
  else if (!self->can_hold)
    {
      /* a second call on a modem that cannot hold: the call that is up ends, then this one is answered */
      call->answer_after = ANSWER_WHEN_ALONE;
      end_others (self, call, FALSE);
    }
  else
    {
      gboolean held = FALSE;

      for (guint i = 0; i < self->calls->len; i++)
        held |= ((Call *) self->calls->pdata[i])->state == CALL_STATE_ONHOLD;
      /* a second call holds the one that is up (CHLD=2); a third ends the one on hold first */
      if (held)
        {
          call->answer_after = ANSWER_WHEN_NONE_HELD;
          end_others (self, call, TRUE);
        }
      else
        modem_voice_method (self->modem, "HoldAndAccept", on_voice_method, self);
    }
  sync_calls (self);
  return TRUE;
}

gboolean
calls_answer_and_release (Calls *self, guint id, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  ringer_stop (self->ringer);
  self->silenced_call = call->id;
  if (!self->can_hold)
    {
      /* no +CHLD: each call ends on its own, then this one is answered */
      call->answer_after = ANSWER_WHEN_ALONE;
      end_others (self, call, FALSE);
      sync_calls (self);
      return TRUE;
    }
  /* CHLD=1: the calls that are up end, the waiting one is answered */
  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *other = self->calls->pdata[i];

      if (other != call && is_live (other) && !is_ringing (other) && other->state != CALL_STATE_ONHOLD)
        other->hangup_requested = TRUE;
    }
  modem_voice_method (self->modem, "HangupAndAccept", on_voice_method, self);
  return TRUE;
}

gboolean
calls_decline (Calls *self, guint id, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  call->hangup_requested = TRUE;
  ringer_stop (self->ringer);
  self->silenced_call = call->id;
  if (call->path)
    call_method (self, call, "Hangup", NULL);
  return TRUE;
}

gboolean
calls_hangup (Calls *self, guint id, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  if (is_ringing (call))
    return calls_decline (self, id, error);
  call->hangup_requested = TRUE;
  /* still being made: on_created ends it as soon as it has its path */
  if (!call->path)
    return TRUE;
  call->state = CALL_STATE_DISCONNECTING;
  call_method (self, call, "Hangup", NULL);
  sync_calls (self);
  return TRUE;
}

gboolean
calls_set_held (Calls *self, guint id, gboolean held, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  if (!self->can_hold)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This phone cannot hold calls"));
      return FALSE;
    }
  if (held == (call->state == CALL_STATE_ONHOLD))
    return TRUE;
  /* GSM has no "hold this one": CHLD=2 swaps the active and the held calls, which with one call holds or resumes it */
  modem_voice_method (self->modem, "HoldAndAccept", on_voice_method, self);
  return TRUE;
}

static void
on_dtmf (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  Calls *self = data;

  if (reply || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_warning ("calls: cannot send tones: %s", error->message);
  if (modem_error_is_unsupported (error) && !self->dtmf_unsupported)
    {
      learn_dtmf_unsupported (self);
      self->callbacks.changed (self->data);
    }
}

gboolean
calls_send_dtmf (Calls *self, guint id, const char *tones, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  if (call->state != CALL_STATE_ACTIVE || !call->path)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Call %u is not up", id);
      return FALSE;
    }
  g_dbus_connection_call (self->system, MM_NAME, call->path, CALL_INTERFACE, "SendDtmf", g_variant_new ("(s)", tones),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, call->cancellable, on_dtmf, self);
  return TRUE;
}

gboolean
calls_continue_post_dial (Calls *self, guint id, gboolean proceed, GError **error)
{
  Call *call = live_call (self, id, error);

  if (!call)
    return FALSE;
  if (!call->post_dial_waiting)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Call %u is not waiting to send tones", id);
      return FALSE;
    }
  call->post_dial_waiting = FALSE;
  /* No: the rest is not sent (Connection.cancelPostDial) */
  if (!proceed)
    call->post_dial_at = strlen (call->post_dial);
  sync_calls (self);
  post_dial_next (call);
  return TRUE;
}

void
calls_set_muted (Calls *self, gboolean muted)
{
  if (muted == self->muted)
    return;
  self->muted = muted;
  call_audio (self, "MuteMic", g_variant_new ("(b)", muted));
  self->callbacks.changed (self->data);
}

void
calls_set_speaker (Calls *self, gboolean speaker)
{
  if (speaker == self->speaker)
    return;
  self->speaker = speaker;
  call_audio (self, "EnableSpeaker", g_variant_new ("(b)", speaker));
  sync_notification (self);
  self->callbacks.changed (self->data);
}

void
calls_silence_ringer (Calls *self)
{
  for (guint i = 0; i < self->calls->len; i++)
    if (((Call *) self->calls->pdata[i])->state == CALL_STATE_INCOMING)
      self->silenced_call = ((Call *) self->calls->pdata[i])->id;
  ringer_stop (self->ringer);
}

void
calls_preview_ringtone (Calls *self, const char *uri)
{
  ringer_preview (self->ringer, self->calls->len ? NULL : uri);
}

void
calls_play_tone (Calls *self, char key)
{
  if (!self->calls->len)
    ringer_play_tone (self->ringer, key);
}

void
calls_stop_tone (Calls *self)
{
  ringer_stop_tone (self->ringer);
}

void
calls_prepare_tones (Calls *self)
{
  if (!self->calls->len)
    ringer_prepare_tones (self->ringer);
}

void
calls_release_tones (Calls *self)
{
  ringer_release_tones (self->ringer);
}

void
calls_set_incall_visible (Calls *self, gboolean visible)
{
  if (visible == self->incall_visible)
    return;
  self->incall_visible = visible;
  sync_notification (self);
}

void
calls_missed_seen (Calls *self)
{
  if (call_log_mark_missed_read (self->log, 0))
    self->callbacks.log_changed (self->data);
  call_notifications_clear_all_missed (self->notifications);
}

void
calls_calls_seen (Calls *self, const gint64 *ids, guint n)
{
  guint changed = 0;

  for (guint i = 0; i < n; i++)
    {
      changed += call_log_mark_missed_read (self->log, ids[i]);
      call_notifications_clear_missed (self->notifications, ids[i]);
    }
  if (changed)
    self->callbacks.log_changed (self->data);
}

void
calls_set_gateway (Calls *self, gboolean gateway)
{
  if (gateway == self->gateway)
    return;
  self->gateway = gateway;
  g_message ("calls: %s", gateway ? "gateway mode: gsm2sip has the modem" : "gateway mode off");
  if (gateway)
    {
      /* gsm2sip answers and ends the calls now: ours are let go of without a word to the modem */
      ringer_stop (self->ringer);
      g_ptr_array_set_size (self->calls, 0);
      call_notifications_clear_call (self->notifications);
      sync_calls (self);
    }
  else
    {
      self->probed = FALSE;
      probe_modem (self);
      self->callbacks.changed (self->data);
    }
}

/* --- what the apps read */

GVariant *
calls_get_calls (Calls *self)
{
  GVariantBuilder calls;

  g_variant_builder_init (&calls, G_VARIANT_TYPE ("aa{sv}"));
  for (guint i = 0; i < self->calls->len; i++)
    {
      Call *call = self->calls->pdata[i];
      GVariantBuilder dict;

      g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&dict, "{sv}", "id", g_variant_new_uint32 (call->id));
      g_variant_builder_add (&dict, "{sv}", "number", g_variant_new_string (call->number));
      g_variant_builder_add (&dict, "{sv}", "presentation", g_variant_new_uint32 (call->presentation));
      g_variant_builder_add (&dict, "{sv}", "direction", g_variant_new_uint32 (call->direction));
      g_variant_builder_add (&dict, "{sv}", "state", g_variant_new_uint32 (call->state));
      g_variant_builder_add (&dict, "{sv}", "end", g_variant_new_uint32 (call->end));
      g_variant_builder_add (&dict, "{sv}", "created", g_variant_new_int64 (call->created));
      g_variant_builder_add (&dict, "{sv}", "connected", g_variant_new_int64 (call->connected));
      g_variant_builder_add (&dict, "{sv}", "multiparty", g_variant_new_boolean (call->multiparty));
      g_variant_builder_add (&dict, "{sv}", "post-dial-wait",
                             g_variant_new_string (call->post_dial_waiting ? call->post_dial + call->post_dial_at
                                                                           : ""));
      g_variant_builder_add (&calls, "a{sv}", &dict);
    }
  return g_variant_builder_end (&calls);
}

gboolean
calls_get_muted (Calls *self)
{
  return self->muted;
}

gboolean
calls_get_speaker (Calls *self)
{
  return self->speaker;
}

const char *
calls_get_status (Calls *self)
{
  if (self->gateway)
    return "gateway";
  if (!modem_is_available (self->modem))
    return "no-modem";
  if (modem_get_emergency_only (self->modem))
    return "emergency-only";
  return "ready";
}

GStrv
calls_get_features (Calls *self)
{
  g_autoptr(GStrvBuilder) features = g_strv_builder_new ();

  if (self->can_hold)
    g_strv_builder_add (features, "hold");
  if (!self->dtmf_unsupported)
    g_strv_builder_add (features, "dtmf");
  return g_strv_builder_end (features);
}

const char *
calls_get_country (Calls *self)
{
  const char *country = modem_get_country (self->modem);

  if (!country)
    country = phone_number_locale_country ();
  return country ? country : "";
}

/* --- the notifications' buttons */

static void
on_notification_action (gboolean missed, gint64 id, const char *action, gpointer data)
{
  Calls *self = data;
  g_autoptr(GError) error = NULL;

  if (missed)
    {
      /* tapped or swiped away, a missed call is seen; tapped, the call log opens */
      if (!g_str_equal (action, "call-back") && call_log_mark_missed_read (self->log, id))
        self->callbacks.log_changed (self->data);
      if (g_str_equal (action, "open"))
        phone_open_app (self->session, DIALER_APP_ID, "show-call-log", NULL);
      else if (g_str_equal (action, "call-back"))
        {
          g_autoptr(GPtrArray) entries = call_log_list (self->log, 200);

          for (guint i = 0; i < entries->len; i++)
            {
              CallLogEntry *entry = entries->pdata[i];

              if (entry->id == id && calls_dial (self, entry->number, &error))
                {
                  call_log_mark_missed_read (self->log, id);
                  call_notifications_clear_missed (self->notifications, id);
                  self->callbacks.log_changed (self->data);
                  phone_start_app (INCALL_APP_ID);
                }
            }
        }
    }
  else if (g_str_equal (action, "answer"))
    {
      if (calls_answer (self, id, &error))
        phone_start_app (INCALL_APP_ID);
    }
  else if (g_str_equal (action, "decline") || g_str_equal (action, "hangup"))
    calls_hangup (self, id, &error);
  else if (g_str_equal (action, "speaker-on") || g_str_equal (action, "speaker-off"))
    calls_set_speaker (self, g_str_equal (action, "speaker-on"));
  else if (g_str_equal (action, "open"))
    phone_start_app (INCALL_APP_ID);
  if (error)
    g_warning ("calls: %s", error->message);
}

/* the missed calls not yet seen get their notifications again (the service started after a restart or a boot) */
static void
show_new_missed (Calls *self)
{
  g_autoptr(GPtrArray) entries = call_log_list (self->log, 50);

  for (guint i = 0; i < entries->len; i++)
    {
      CallLogEntry *entry = entries->pdata[i];
      Call stand_in = { .number = entry->number, .presentation = entry->presentation };
      g_autofree char *who = NULL;

      if (entry->type != CALL_TYPE_MISSED || !entry->is_new || entry->is_read)
        continue;
      who = call_title (self, &stand_in);
      call_notifications_show_missed (self->notifications, entry->id, who,
                                      entry->presentation == PRESENTATION_ALLOWED ? entry->number : NULL);
    }
}

static void
on_contacts_changed (Contacts *contacts, gpointer data)
{
  Calls *self = data;

  /* the name a call shows may have come (a slow address book): the notification says it */
  sync_notification (self);
}

static void
on_contacts_loaded (Contacts *contacts, gpointer data)
{
  Calls *self = data;

  if (!contacts_is_loaded (contacts))
    return;
  g_signal_handlers_disconnect_by_func (contacts, on_contacts_loaded, self);
  show_new_missed (self);
}

Calls *
calls_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings, CallLog *log,
           const CallsCallbacks *callbacks, gpointer data)
{
  static const ModemCallbacks modem_callbacks = {
    .available_changed = on_available_changed,
    .call_added = on_call_added,
    .call_deleted = on_call_deleted,
  };
  Calls *self = g_new0 (Calls, 1);

  self->session = g_object_ref (session);
  self->system = g_object_ref (system);
  self->settings = g_object_ref (settings);
  self->log = log;
  self->callbacks = *callbacks;
  self->data = data;
  self->calls = g_ptr_array_new_with_free_func ((GDestroyNotify) call_free);
  self->deferred = g_ptr_array_new_with_free_func (g_free);
  self->ringer = ringer_new (settings);
  self->notifications = call_notifications_new (on_notification_action, self);
  self->contacts = contacts_new ();
  self->blocked = blocked_numbers_new (NULL);
  g_signal_connect (self->contacts, "changed", G_CALLBACK (on_contacts_changed), self);
  /* the missed calls' notifications wait for the contacts, to say who called */
  g_signal_connect (self->contacts, "changed", G_CALLBACK (on_contacts_loaded), self);
  self->modem = modem_new (system, &modem_callbacks, self);
  return self;
}

void
calls_free (Calls *self)
{
  g_clear_handle_id (&self->vibrate_45_id, g_source_remove);
  g_ptr_array_unref (self->calls);
  g_ptr_array_unref (self->deferred);
  modem_free (self->modem);
  g_signal_handlers_disconnect_by_data (self->contacts, self);
  g_object_unref (self->contacts);
  g_object_unref (self->blocked);
  call_notifications_free (self->notifications);
  ringer_free (self->ringer);
  g_object_unref (self->settings);
  g_object_unref (self->system);
  g_object_unref (self->session);
  g_free (self);
}
