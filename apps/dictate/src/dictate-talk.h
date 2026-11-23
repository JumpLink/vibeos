/* DictateTalk: a spoken conversation with a model on the phone: what the user said goes to the model
 * (dictate-brain.h), its reply is shown as it is written and spoken sentence by sentence (dictate-voice.h) as soon
 * as each one is finished.
 *
 * The model and the voice are found on the phone (talk_find_brain (), talk_find_voice ()), loaded the first time
 * they are needed and kept. Each runs on a thread of its own; the playback is GStreamer's, to the default output. */
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
/* the reply stops at once, written and spoken; what was written of it stays in the conversation */
void             dictate_talk_stop      (DictateTalk *self);
/* a new conversation: the model forgets this one */
void             dictate_talk_reset     (DictateTalk *self);
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
 *          arguments and result may hold the user's data, never log them */

G_END_DECLS
