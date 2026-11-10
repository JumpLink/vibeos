/* The calls: the heart of the Phone's service. It keeps the list of calls the modem has, in InCallUI's states,
 * and does what each change of a call asks for, as Telecom and InCallUI do on Android:
 *
 *  - a call comes: it rings (ringer.h), shows the answer screen over the lock screen, or a heads-up with Answer and
 *    Decline when the phone is in use (call-notifications.h);
 *  - a call is up: the call audio is switched in (exynos-callaudiod), the ongoing-call notification counts on;
 *  - a call ends: it stays a moment as "Call ended", goes into the call log (call-log.h), and a missed one gets its
 *    notification.
 *
 * What the user asks (dial, answer, hang up, mute) comes in from the D-Bus side (calls-service.c), which is also
 * the one that tells the apps what changed. */
#pragma once

#include <gio/gio.h>

#include "call-log.h"
#include "call-types.h"

typedef struct _Calls Calls;

typedef struct {
  /* the calls, a call's state, mute, speaker, the status or what the modem can do changed: the Calls property */
  void (*changed) (gpointer data);
  /* a row came into the call log or went: the apps read it again */
  void (*log_changed) (gpointer data);
} CallsCallbacks;

Calls      *calls_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings, CallLog *log,
                       const CallsCallbacks *callbacks, gpointer data);
void        calls_free (Calls *self);

/* the calls as the service hands them out: aa{sv}, oldest first. Each has "id" (u), "number" (s), "presentation"
 * (u, Presentation), "direction" (u, CallDirection), "state" (u, CallState), "end" (u, CallEnd), "created" and
 * "connected" (x, Unix ms; connected is 0 until the call is first up), "multiparty" (b) and "post-dial-wait" (s: the
 * tones after a wait in the number dialled, "Send the following tones?"; "" when it is not waiting) */
GVariant   *calls_get_calls (Calls *self);
gboolean    calls_get_muted (Calls *self);
gboolean    calls_get_speaker (Calls *self);
/* "ready"; "no-modem"; "emergency-only" (no SIM, or not registered); "gateway" (gsm2sip has the modem) */
const char *calls_get_status (Calls *self);
/* what the modem can do besides calling and answering: "hold" (and so swap and answer-and-hold), "dtmf" */
GStrv       calls_get_features (Calls *self);
/* the network's country, "DE"; "" while unknown */
const char *calls_get_country (Calls *self);

/* gsm2sip took the modem (its gateway mode) or gave it back: the service stands aside meanwhile */
void        calls_set_gateway (Calls *self, gboolean gateway);

/* the user's asks. Each answers with an error when it cannot be done; the call's state says when it is done */
guint       calls_dial (Calls *self, const char *number, GError **error);
gboolean    calls_answer (Calls *self, guint id, GError **error);
/* answer a waiting call and end the one that is up (GSM's CHLD=1) */
gboolean    calls_answer_and_release (Calls *self, guint id, GError **error);
gboolean    calls_decline (Calls *self, guint id, GError **error);
gboolean    calls_hangup (Calls *self, guint id, GError **error);
gboolean    calls_set_held (Calls *self, guint id, gboolean held, GError **error);
gboolean    calls_send_dtmf (Calls *self, guint id, const char *tones, GError **error);
/* the answer to "Send the following tones?": the rest of the number's tones go out, or not */
gboolean    calls_continue_post_dial (Calls *self, guint id, gboolean proceed, GError **error);
void        calls_set_muted (Calls *self, gboolean muted);
void        calls_set_speaker (Calls *self, gboolean speaker);
/* the ringing stops, the call goes on ringing silently (a volume key, Android's "silence ringer") */
void        calls_silence_ringer (Calls *self);
/* Settings' ringtone preview (NULL stops it) and the main dialpad's keypad tones (ringer.h); none while there is a
 * call, whose sound has the speaker */
void        calls_preview_ringtone (Calls *self, const char *uri);
void        calls_play_tone (Calls *self, char key);
void        calls_stop_tone (Calls *self);
/* the in-call screen is in front, or no longer: an incoming call's notification then does not peek over it */
void        calls_set_incall_visible (Calls *self, gboolean visible);
/* the missed calls are seen: their notification goes */
void        calls_missed_seen (Calls *self);
/* these missed calls are seen (their row in Recents was opened): their notifications go */
void        calls_calls_seen (Calls *self, const gint64 *ids, guint n);
