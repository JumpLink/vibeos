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
/* the model's reply to `text`, streamed to `piece` as it comes, and the whole of it returned. Cancelled (or stopped
 * by `piece`), the reply so far is returned and kept in the conversation. */
char         *dictate_brain_reply       (DictateBrain *self, const char *text, DictateBrainPiece piece, gpointer data,
                                         GCancellable *cancellable, GError **error);
/* for the log: the last reply's tokens read (the prompt's new part), written, and the times (ms) to read them and to
 * the first written token */
void          dictate_brain_last_timing (DictateBrain *self, int *read, int *written, double *read_ms,
                                         double *first_ms, double *total_ms);

G_END_DECLS
