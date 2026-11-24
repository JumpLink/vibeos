/* dictate-vad: voice activity detection, Silero VAD (an ONNX model, run by onnxruntime): how likely each 32 ms of
 * microphone audio (DICTATE_VAD_FRAME samples at 16 kHz) is speech. The engine cuts the speaker's turns with it
 * (dictate-engine.h, dictate_engine_set_turns ()).
 *
 * The model is vibeos-dictate/vad/silero_vad.onnx under the user's data directory or one of the system's. A
 * detector keeps its state from frame to frame: one stream at a time, on one thread. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define DICTATE_VAD_FRAME 512

typedef struct _DictateVad DictateVad;

/* the installed model's file, or NULL */
char       *dictate_vad_find  (void);
DictateVad *dictate_vad_load  (const char *model, GError **error);
void        dictate_vad_free  (DictateVad *self);
/* a new stream: what was heard before is forgotten */
void        dictate_vad_reset (DictateVad *self);
/* the probability that `frame` (DICTATE_VAD_FRAME samples, -1..1) is speech: 0..1, or -1 when the model failed */
float       dictate_vad_frame (DictateVad *self, const float *frame);

G_END_DECLS
