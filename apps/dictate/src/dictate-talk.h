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
/* the replies spoken (TRUE, at first) or only written; turned off while one is spoken, it stops speaking at once */
void             dictate_talk_set_speak (DictateTalk *self, gboolean speak);
gboolean         dictate_talk_get_speak (DictateTalk *self);

/* Signals, all on the main thread:
 *   "reply" (const char *text): the reply as written so far (the whole of it each time)
 *   "state-changed" ()
 *   "error" (const char *message): what went wrong, for the user; the reply stops */

G_END_DECLS
