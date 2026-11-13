/* The app's side of the call service (neo-dialer-calls, neo.Dialer.Calls on the session bus).
 *
 * The service has the calls; the app only shows them and asks for things. CallsClient keeps the service's call
 * list (its Calls property) and emits "changed" when it changes.
 * An ask changes nothing here: the new state comes back through "changed", so the screen shows what the modem
 * does, not what the user hoped for. Asking starts the service if it is not running (D-Bus activation). */
#pragma once

#include <gio/gio.h>

#include "call-types.h"

G_BEGIN_DECLS

/* one call as the service reports it */
typedef struct
{
  guint id;
  char *number;                 /* as the network sent it; "" when withheld */
  Presentation presentation;
  CallDirection direction;
  CallState state;
  CallEnd end;
  gint64 created;               /* Unix ms */
  gint64 connected;             /* Unix ms when it was first up; 0 before */
  gboolean multiparty;
  char *post_dial_wait;         /* at a wait in the number dialled: the tones after it, to ask about; else "" */
} CallInfo;

#define CALLS_TYPE_CLIENT (calls_client_get_type ())
G_DECLARE_FINAL_TYPE (CallsClient, calls_client, CALLS, CLIENT, GObject)

/* the one client of the process */
CallsClient *calls_client_get (void);

/* CallInfo, oldest first, owned by the client (valid until the next "changed") */
GPtrArray  *calls_client_get_calls (CallsClient *self);
CallInfo   *calls_client_find (CallsClient *self, guint id);
/* the call the screen is about: ringing, then outgoing, then up, then hanging
 * up, then ended, then held. NULL with no calls */
CallInfo   *calls_client_get_primary (CallsClient *self);
/* the other call that is still going, or NULL */
CallInfo   *calls_client_get_secondary (CallsClient *self, const CallInfo *primary);

gboolean    calls_client_get_muted (CallsClient *self);
gboolean    calls_client_get_speaker (CallsClient *self);
/* "ready", "no-modem", "emergency-only", "gateway"; "" while the service is not there */
const char *calls_client_get_status (CallsClient *self);
/* "hold", "dtmf": what the modem has shown it can do */
gboolean    calls_client_has_feature (CallsClient *self, const char *feature);
/* the network's country ("DE"), or NULL while it is not known */
const char *calls_client_get_country (CallsClient *self);
guint       calls_client_get_missed_count (CallsClient *self);

typedef void (*CallsDialed) (guint id, const GError *error, gpointer data);

/* the asks. A refusal comes back as "failed"; Dial's goes to @dialed, which the dialpad shows */
void calls_client_dial (CallsClient *self, const char *number, CallsDialed dialed, gpointer data);
void calls_client_answer (CallsClient *self, guint id);
void calls_client_answer_and_release (CallsClient *self, guint id);
void calls_client_decline (CallsClient *self, guint id);
void calls_client_hangup (CallsClient *self, guint id);
void calls_client_set_held (CallsClient *self, guint id, gboolean held);
void calls_client_send_dtmf (CallsClient *self, guint id, const char *tones);
/* "Send the following tones?" answered: Yes (@proceed) sends the rest of the number's tones, No drops them */
void calls_client_continue_post_dial (CallsClient *self, guint id, gboolean proceed);
void calls_client_set_muted (CallsClient *self, gboolean muted);
void calls_client_set_speaker (CallsClient *self, gboolean speaker);
void calls_client_silence_ringer (CallsClient *self);
/* Settings' ringtone picker: @uri ("default" for the default ringtone) played once; NULL stops it */
void calls_client_preview_ringtone (CallsClient *self, const char *uri);
/* the main dialpad's keypad tone while @key is held; the service plays it if "Keypad tones" is on */
void calls_client_play_tone (CallsClient *self, char key);
void calls_client_stop_tone (CallsClient *self);
/* the main dialpad came up, or went (the service opens the tones' stream ahead of the first key) */
void calls_client_prepare_tones (CallsClient *self);
void calls_client_release_tones (CallsClient *self);
/* the in-call screen is in front or not: the service keeps a ringing call's notification from peeking over it */
void calls_client_set_incall_visible (CallsClient *self, gboolean visible);

/* the call log, newest first: CallLogEntry (call-log.h), in a GPtrArray that frees them */
void       calls_client_get_call_log (CallsClient *self, GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer data);
GPtrArray *calls_client_get_call_log_finish (CallsClient *self, GAsyncResult *result, GError **error);
void       calls_client_delete_call_log (CallsClient *self, const gint64 *ids, guint n);
void       calls_client_clear_call_log (CallsClient *self);
/* the missed calls are seen (Recents was looked at): their notification goes */
void       calls_client_mark_missed_read (CallsClient *self);
/* these missed calls are seen (their row in Recents was opened) */
void       calls_client_mark_calls_read (CallsClient *self, const gint64 *ids, guint n);

/* Signals: "changed" (the calls), "audio-changed" (mute, speaker), "status-changed" (status, features, country),
 * "missed-changed", "call-log-changed", and "failed" (s method, u call id or 0, s message): the service refused an
 * ask (the modem cannot hold, the call ended meanwhile) */

G_END_DECLS
