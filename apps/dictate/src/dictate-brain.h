/* dictate-brain: a conversation with a small language model on the phone itself (llama.cpp, a GGUF file), for the
 * conversation (dictate-talk.c).
 *
 * One conversation at a time: a system prompt, then the user's turns and the model's replies. What the model has
 * read is kept between turns, so a turn costs only its own words. The model runs on the device's fastest cores (by
 * their clock), on the thread that calls dictate_brain_reply (). Not thread-safe: one thread uses a brain. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _DictateBrain DictateBrain;

/* an error worded for the person talking (no network, refused, too many questions): show its message as it is */
#define DICTATE_BRAIN_ERROR (dictate_brain_error_quark ())
GQuark dictate_brain_error_quark (void);
typedef enum {
  DICTATE_BRAIN_ERROR_USER,
  DICTATE_BRAIN_ERROR_BUSY,     /* the server has had too many questions from this phone: said, not shown */
  DICTATE_BRAIN_ERROR_OFFLINE,  /* the server cannot be reached, or is down: Agent works offline (dictate-offline.h) */
} DictateBrainError;

/* a piece of the reply as it is written, on the replying thread: return FALSE to stop the reply there */
typedef gboolean (*DictateBrainPiece) (const char *piece, gpointer data);

/* `model`: a GGUF file, or a server's address (https://…/v1, dictate-remote.h), for which threads and context do
 * not matter. threads: 0 for the number of the fastest cores (at most 4); context: tokens, 0 for 4096 */
DictateBrain *dictate_brain_load        (const char *model, int threads, int context, GError **error);
void          dictate_brain_free        (DictateBrain *self);
/* whether `model` is a server's address rather than a file */
gboolean      dictate_brain_is_remote   (const char *model);
/* the conversation starts again, with this system prompt (NULL: none) */
void          dictate_brain_reset       (DictateBrain *self, const char *system);
/* the conversation goes on under another system prompt (NULL: none): on the phone the model reads all of it again at
 * the next reply */
void          dictate_brain_set_system  (DictateBrain *self, const char *system);
/* the last question and all that followed it gone from the conversation, as if never asked (a refused turn, one the
 * model gave no reply to) */
void          dictate_brain_forget_turn (DictateBrain *self);
/* an earlier conversation goes on (`n` questions and their replies, oldest first), after the system prompt; a model on
 * the phone starts afresh instead */
void          dictate_brain_restore     (DictateBrain *self, const char *const *said, const char *const *replies,
                                         guint n);
/* the model's reply to `text`, streamed to `piece` as it comes, and the whole of it returned. Cancelled (or stopped
 * by `piece`), the reply so far is returned and kept in the conversation. */
char         *dictate_brain_reply       (DictateBrain *self, const char *text, DictateBrainPiece piece, gpointer data,
                                         GCancellable *cancellable, GError **error);
/* a tool the model asked for (dictate-tools.h runs it): the call's id, the tool's name, its arguments (JSON) */
typedef struct {
  char *id;
  char *name;
  char *arguments;
} DictateBrainCall;

void          dictate_brain_call_free   (DictateBrainCall *call);
/* whether the model can be given tools: a server's can, the one on the phone cannot */
gboolean      dictate_brain_has_tools   (DictateBrain *self);
/* one step of the model's turn: `text` is the user's (NULL: the turn goes on after the tools' results), `tools`
 * the functions it may call (an OpenAI "tools" JSON array; NULL: none, it answers). Returns what it wrote, as
 * dictate_brain_reply (); *calls is set to the DictateBrainCall it asked for, empty when that was its answer. Each
 * call's result must be added (dictate_brain_add_result ()) before the next step. */
char         *dictate_brain_step        (DictateBrain *self, const char *text, const char *tools,
                                         DictateBrainPiece piece, gpointer data, GPtrArray **calls,
                                         GCancellable *cancellable, GError **error);
void          dictate_brain_add_result  (DictateBrain *self, const char *call_id, const char *result);
/* for the log: the last reply's tokens read (the prompt's new part), written, and the times (ms) to read them and to
 * the first written token */
void          dictate_brain_last_timing (DictateBrain *self, int *read, int *written, double *read_ms,
                                         double *first_ms, double *total_ms);

G_END_DECLS
