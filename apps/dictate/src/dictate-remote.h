/* dictate-remote: a conversation with a language model on a server, for dictate-brain.h when its model is an
 * address (https://vibeos.top:8443/v1): an OpenAI-style chat API behind the Agent's gateway (the store's
 * docs/api.md, section 17), the reply streamed as it is written. The phone enrolls with the gateway by itself the
 * first time, and again whenever the gateway no longer knows it.
 *
 * Same contract as dictate-brain.h: one thread uses it; the reply's text comes piece by piece. Errors a person can
 * act on (no network, this phone refused, too many questions) are DICTATE_BRAIN_ERROR_USER, worded for them. */
#pragma once

#include "dictate-brain.h"

G_BEGIN_DECLS

typedef struct _DictateRemote DictateRemote;

DictateRemote *dictate_remote_new         (const char *address);
void           dictate_remote_free        (DictateRemote *self);
/* the connection opened and the phone known to the gateway ahead of the first question, so that one is answered
 * as fast as the rest; a failure is only logged (the first question tries again) */
void           dictate_remote_warm        (DictateRemote *self);
void           dictate_remote_reset       (DictateRemote *self, const char *system);
void           dictate_remote_set_system  (DictateRemote *self, const char *system);
char          *dictate_remote_reply       (DictateRemote *self, const char *text, DictateBrainPiece piece,
                                           gpointer data, GCancellable *cancellable, GError **error);
void           dictate_remote_last_timing (DictateRemote *self, int *read, int *written, double *read_ms,
                                           double *first_ms, double *total_ms);

G_END_DECLS
