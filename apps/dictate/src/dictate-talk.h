/* DictateTalk: a spoken conversation with a model on the phone: what the user said goes to the model
 * (dictate-brain.h), its reply is shown as it is written and spoken sentence by sentence (dictate-voice.h) as soon
 * as each one is finished.
 *
 * The model and the voice are found on the phone (talk_find_brain (), talk_find_voice ()), loaded the first time
 * they are needed and kept. Each runs on a thread of its own; the playback is GStreamer's, to the default output.
 *
 * When the model's server cannot be reached, a fixed set of commands answers instead (dictate-offline.h). In private
 * mode (talk-private) the replies play only in headphones; on the speaker they are short and say nothing personal. */
#pragma once

#include "dictate-tools.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_TALK_IDLE,
  DICTATE_TALK_THINKING,        /* the model reading and writing, nothing spoken yet */
  DICTATE_TALK_SPEAKING,        /* the reply being spoken (and maybe still written) */
} DictateTalkState;

#define DICTATE_TYPE_TALK (dictate_talk_get_type ())
G_DECLARE_FINAL_TYPE (DictateTalk, dictate_talk, DICTATE, TALK, GObject)

DictateTalk     *dictate_talk_new       (void);
/* FALSE, with the reason for the user, when there is no model or no voice for `language` on the phone */
gboolean         dictate_talk_available (DictateTalk *self, const char *language, char **reason);
/* there is a voice for `language` (the talk-voice setting's, else one in vibeos-dictate/voices/LANGUAGE) */
gboolean         dictate_talk_has_voice (DictateTalk *self, const char *language);
/* the model is a server's: what is asked of Vibe leaves the phone, and needs a network (the fixed commands answer
 * without one: dictate-offline.h) */
gboolean         dictate_talk_needs_network (DictateTalk *self);
/* the model and the voice loaded now, in the background, so that the first answer comes sooner */
void             dictate_talk_prepare   (DictateTalk *self, const char *language);
/* the user said `text` (in `language`): the reply follows */
void             dictate_talk_ask       (DictateTalk *self, const char *language, const char *text);

/* where a question came from, for the voice check (dictate-voicematch.h) */
typedef enum {
  DICTATE_ASK_NONE            = 0,
  DICTATE_ASK_TAPPED          = 1 << 0, /* a tap or typed text (Undo, a suggested reply), not a voice: not checked */
  DICTATE_ASK_OWNER_CONFIRMED = 1 << 1, /* the owner's PIN was given for this turn: not checked */
} DictateAskFlags;

/* the user said `text`, `audio` being the turn's speech (16 kHz mono float32; NULL for none): while Voice Match
 * applies, a turn whose voice is not the owner's runs nothing it would need the owner for ("refused") */
void             dictate_talk_ask_full  (DictateTalk *self, const char *language, const char *text, GBytes *audio,
                                         DictateAskFlags flags);

/* the user said "send" (or "yes", "call": dictate_offline_confirm_word ()) while `action` waits on the screen, on its
 * card or in its countdown; `audio` and `flags` as for dictate_talk_ask_full (). The voice is checked as for an
 * Actions turn: when it passes, the action runs (DICTATE_ACTION_BY_VOICE) and `callback` gets what was done
 * (dictate_talk_confirm_finish (), as dictate_action_run_finish ()); when it does not, "refused" is emitted, its line is
 * spoken, nothing runs and the error is G_IO_ERROR_PERMISSION_DENIED. "cancel" needs no check: dictate_action_cancel ()
 * directly. */
void             dictate_talk_confirm_async  (DictateTalk *self, DictateAction *action, GBytes *audio,
                                              DictateAskFlags flags, GCancellable *cancellable,
                                              GAsyncReadyCallback callback, gpointer data);
char            *dictate_talk_confirm_finish (DictateTalk *self, GAsyncResult *result, GError **error);

/* an exchange of the conversation as the screen keeps it */
typedef struct {
  const char *said;
  const char *reply;
} DictateExchange;

/* after a restart, the conversation on the screen (oldest first) goes on: the model is given its newest
 * DICTATE_TALK_HISTORY exchanges */
#define DICTATE_TALK_HISTORY 12
void             dictate_talk_restore   (DictateTalk *self, const DictateExchange *exchanges, guint n);
/* the reply stops at once, written and spoken; what was written of it stays in the conversation */
void             dictate_talk_stop      (DictateTalk *self);
/* a new conversation: the model forgets this one */
void             dictate_talk_reset     (DictateTalk *self);
/* `text` said as Agent's reply (in `language`'s voice), the model not asked: a call ringing */
void             dictate_talk_announce  (DictateTalk *self, const char *language, const char *text);
/* what goes on now ("a call is ringing, from Anna"), told to the model with each question; NULL for nothing */
void             dictate_talk_set_context (DictateTalk *self, const char *context);
/* the phone is locked (Agent was asked by holding the power key on the lock screen): from the next question on, only
 * the tools that read are offered and run, until it is unlocked */
void             dictate_talk_set_locked  (DictateTalk *self, gboolean locked);
DictateTalkState dictate_talk_get_state (DictateTalk *self);
void             dictate_talk_set_speed (DictateTalk *self, double speed);
/* the voice stops at once (a tap while it speaks); the reply goes on being written */
void             dictate_talk_hush      (DictateTalk *self);
/* the replies spoken (TRUE, at first) or only written; turned off while one is spoken, it stops speaking at once */
void             dictate_talk_set_speak (DictateTalk *self, gboolean speak);
gboolean         dictate_talk_get_speak (DictateTalk *self);

/* Signals, all on the main thread:
 *   "reply" (const char *text): the reply as written so far (the whole of it each time)
 *   "state-changed" ()
 *   "error" (const char *message): what went wrong, for the user; the reply stops
 *   "status" (const char *text): what a tool is doing ("Checking your calendar…"), NULL when it is done
 *   "propose" (DictateAction *action): an action for the user to confirm (dictate-tools.h); ref it to keep it
 *   "busy" (): the server had too many questions; the reply is a line saying so (for tests: not the model's)
 *   "tool" (const char *name, const char *arguments, const char *result, double ms): a tool ran, for tests: its
 *          arguments and result may hold the user's data, never log them
 *   "refused" (guint reason, double score, double needed, const char *line): the turn's voice is not the owner's
 *          for what it asks (DICTATE_REFUSED_VOICE), there was too little of it to tell (_TOO_SHORT, score -1), or
 *          there is a sample and the voice could not be checked against it (_UNCHECKED, score -1);
 *          nothing more runs, `line` is what Vibe says instead, and what the turn showed so far is to be cleared
 *   "acted" (gboolean can_undo): a tool did something for this reply; can_undo: "undo" (DICTATE_ASK_TAPPED) takes it
 *          back
 *   "reply-info" (guint flags): at a reply's start, DICTATE_REPLY_* below; again with _OFFLINE when the fixed
 *          commands answer instead of the model (the latest holds)
 *   "written" (const char *const *lines): what is shown and not spoken (private mode on the speaker)
 *   "no-access" (const char *key): the user asked for something whose access-* switch is off
 *   "suggestions" (const char *const *labels): replies the user may tap under the answer ("Read them", "Call back");
 *          a tap asks its label as said, with DICTATE_ASK_TAPPED
 *   "choices" (const char *const *rows): Vibe asks the user to pick one ("Which Kim?"): the rows alternate a label and
 *          its detail ("Kim Novak", "Mobile", "Kim Ortega", "Work"; a detail may be ""); a tap asks the label, with
 *          DICTATE_ASK_TAPPED
 *   "facts" (const char *const *rows): what the turn's reading tools found, for the card above the answer: the rows
 *          alternate a label and its value ("Battery", "64 %, about 9 hours", "Signal", "Good, 4 of 5"), all of them so
 *          far each time
 *   "person" (const char *name): the contact the answer is about, for the orb's initials ("Sarah Lind")
 * All of them for the reply being written, before its end; none after "refused". */

/* "refused"'s reasons */
#define DICTATE_REFUSED_VOICE     0     /* not the owner's voice */
#define DICTATE_REFUSED_TOO_SHORT 1     /* too little speech to tell whose; for Personal and Actions */
#define DICTATE_REFUSED_UNCHECKED 2     /* a sample, and the voice could not be checked against it (Voice Match not
                                         * installed, or its model changed: dictate_voice_match_stale ()); for
                                         * Personal and Actions */

/* "reply-info"'s flags */
#define DICTATE_REPLY_OFFLINE         (1 << 0)  /* the fixed commands answered (dictate-offline.h) */
#define DICTATE_REPLY_PRIVATE_SPEAKER (1 << 1)  /* private mode, the reply on the speaker */

G_END_DECLS
