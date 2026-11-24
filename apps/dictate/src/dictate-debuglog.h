/* dictate-debuglog: Agent's debug log, kept on the phone only and only while the "Debug log" switch (the
 * vibeos.Dictate debug-log key, off by default) is on. Each turn is recorded whole, as it went: every request to the
 * model (the system prompt, the conversation, the user's words, the tools offered), every answer as it came back,
 * put together from its stream, each tool call with its arguments and its result, the times, the HTTP statuses and
 * the errors.
 *
 * A turn is a JSON file of its own in vibeos-dictate/debug-log under the user's data directory (the directory 0700,
 * the files 0600): the newest DEBUGLOG_KEEP turns, DEBUGLOG_BYTES at most in all. Nothing in it is sent anywhere.
 * Where the gateway's key or the phone's id would appear in it, only their first 4 characters are written; audio
 * and the voice pattern are never in it.
 *
 * A turn is recorded on the brain's thread (dictate-talk.c): dictate_debuglog_begin () makes it that thread's
 * current turn, the model's client (dictate-remote.c) and the tools' runner add their steps to it, and
 * dictate_debuglog_end () writes it. On a thread with no current turn, adding a step does nothing.
 *
 * A turn's file (version 1), one JSON object:
 *   version       1
 *   started       when the turn began, ISO 8601 with the phone's offset; started_us: the same, Unix microseconds
 *   turn          its number in the conversation
 *   said          what the user said (or typed)
 *   language      the language Agent answered in ("en", "de")
 *   brain         the model's server address, or the model file's name
 *   locked        the phone was locked (only the reading tools were offered)
 *   private_mode  private mode was on
 *   steps         what happened, in order: objects with "kind" and "at_ms" (ms since the turn began), and
 *     "request"   method, url, body: the request as sent (a JSON object: model, messages, tools…; its "tools" is
 *                 the string "the same as in this turn's first request" after the first)
 *     "http"      method, url, status (0: no answer), ms: one HTTP exchange, a retry being another
 *     "response"  content (the text), tool_calls [{id, name, arguments}], prompt_tokens and completion_tokens
 *                 (when the server said), first_ms (to the first word), total_ms: one answer put back together
 *     "tool"      name, arguments, result, ms, ok: one tool's call
 *     "error", "offline", "retry", "cut-off"   text: what went wrong, or what was done about it
 *   reply         what Agent answered, in full
 *   outcome       "answered", "offline" (the fixed commands), "error" or "stopped"
 *   ms            the turn's time, in all
 * Arguments, results and bodies are JSON as they were; text that was not JSON is a string. */
#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define DEBUGLOG_KEEP  100
#define DEBUGLOG_BYTES (5 * 1024 * 1024)

typedef struct _DictateDebugTurn DictateDebugTurn;

/* a new turn, the calling thread's current one from now on: what was said, the language Agent answers in, the
 * model (a server's address or a file's name), and how the phone was */
DictateDebugTurn *dictate_debuglog_begin    (guint turn, const char *said, const char *language, const char *brain,
                                             gboolean locked, gboolean private_mode);
/* the turn written, with its reply and how it ended ("answered", "offline", "error", "stopped"), and freed; it
 * is no longer the thread's current turn. NULL does nothing */
void              dictate_debuglog_end      (DictateDebugTurn *turn, const char *reply, const char *outcome);
/* whether the calling thread has a turn to add steps to */
gboolean          dictate_debuglog_active   (void);

/* steps of the calling thread's current turn: */
/* a request, its JSON body as sent (`body`, `length` bytes) */
void              dictate_debuglog_request  (const char *method, const char *url, const char *body, gsize length);
/* a request's HTTP status (0 when no answer came), with how long it took */
void              dictate_debuglog_http     (const char *method, const char *url, guint status, double ms);
/* the model's answer, put together: its text, its tool calls (DictateBrainCall *, may be NULL), the tokens read and
 * written (-1: not given), the time to its first piece and to its end */
void              dictate_debuglog_response (const char *content, GPtrArray *calls, int prompt_tokens,
                                             int completion_tokens, double first_ms, double total_ms);
/* a tool run: its arguments and its result (JSON as text), its time, and whether it worked */
void              dictate_debuglog_tool     (const char *name, const char *arguments, const char *result,
                                             double ms, gboolean ok);
/* anything else worth knowing: kind "error", "offline", "retry", … and what happened */
void              dictate_debuglog_note     (const char *kind, const char *text);

/* for the viewer: */
typedef struct {
  char *path;                   /* the turn's file */
  gint64 started;               /* Unix microseconds */
  char *said;
  guint tools;                  /* tool calls in it */
  guint steps;
  gint64 ms;                    /* the whole turn */
  char *outcome;
} DictateDebugEntry;

void              dictate_debug_entry_free  (DictateDebugEntry *entry);
/* the turns kept, newest first: DictateDebugEntry (owned by the array) */
GPtrArray        *dictate_debuglog_list     (void);
/* a turn's file, or NULL */
JsonNode         *dictate_debuglog_read     (const char *path);
/* every turn removed */
void              dictate_debuglog_clear    (void);
/* the log's directory */
char             *dictate_debuglog_dir      (void);
/* `text` with the gateway's key, the phone's id and any bearer token cut to their first 4 characters */
char             *dictate_debuglog_redact   (const char *text);

G_END_DECLS
