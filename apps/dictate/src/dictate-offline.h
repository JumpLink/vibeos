/* dictate-offline: Agent's commands when its model cannot be reached (no network, or VibeOS's server down): a fixed
 * grammar in Agent's six languages (en, de, es, fr, it, ru) for the core of what it does, each command run with the
 * same tools the model uses, and the reply written from the tool's result in the user's language.
 *
 * The commands: the time; the battery; an alarm or a timer set, cancelled or listed; Wi-Fi or Bluetooth on or off;
 * an app opened; "call X" and "text X saying Y" (the confirm card, as always); the latest messages read; undo;
 * "what did you just do?"; and a ringing call answered or declined, an ongoing one hung up.
 *
 * The words are matched loosely: lower case, the punctuation gone, numbers said in words read as numbers ("sept
 * heures trente" is 7:30), so that what Moonshine and Vosk write both work. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* runs the tool `name` with `arguments` (JSON): its result (JSON), owned by the caller */
typedef char *(*DictateOfflineRun) (const char *name, const char *arguments, gpointer data);

/* the reply to `said` in `language`, the tools it needs run through `run`; `private_speaker`: the loudspeaker is on in
 * private mode, so no names or message texts in it. Never NULL: what it cannot do, it says it cannot do offline */
char       *dictate_offline_reply  (const char *language, const char *said, gboolean private_speaker,
                                    DictateOfflineRun run, gpointer data);
/* the line said once, before the first reply offline */
const char *dictate_offline_notice (const char *language);

/* what the whole of a turn says about a card waiting on the screen ("send it", "cancel"), the model not asked */
typedef enum {
  DICTATE_CONFIRM_WORD_NONE,    /* something else: a correction, a question; for the model */
  DICTATE_CONFIRM_WORD_CONFIRM, /* "send", "yes", "call", "go ahead": dictate_talk_confirm_async () */
  DICTATE_CONFIRM_WORD_CANCEL,  /* "cancel", "no", "stop", "don't": dictate_action_cancel () */
} DictateConfirmWord;
DictateConfirmWord dictate_offline_confirm_word (const char *language, const char *text);

/* for the tests: the tool `said` asks for and its arguments (JSON), or NULL when it is none of the commands. Some
 * commands are a tool name of their own: "read_messages" (get_messages, then each unread conversation's text) */
char       *dictate_offline_parse  (const char *language, const char *said, char **arguments);
/* for the tests: `said` lower case, without punctuation, numbers in words as digits */
char       *dictate_offline_normalize (const char *language, const char *said);

G_END_DECLS
