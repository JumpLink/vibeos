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

#include <glib-object.h>

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
 *          for what it asks (reason 0); nothing ran, and `line` is what Agent says instead
 *   "acted" (gboolean can_undo): a tool did something for this reply; can_undo: "undo" (DICTATE_ASK_TAPPED) takes it
 *          back
 *   "reply-info" (guint flags): at a reply's start, DICTATE_REPLY_* below
 *   "written" (const char *const *lines): what is shown and not spoken (private mode on the speaker)
 *   "no-access" (const char *key): the user asked for something whose access-* switch is off */

/* "reply-info"'s flags */
#define DICTATE_REPLY_OFFLINE         (1 << 0)  /* the fixed commands answered (dictate-offline.h) */
#define DICTATE_REPLY_PRIVATE_SPEAKER (1 << 1)  /* private mode, the reply on the speaker */

G_END_DECLS
