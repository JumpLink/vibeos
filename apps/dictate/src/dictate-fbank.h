/* dictate-fbank: the log mel filterbank a speaker model is fed, as Kaldi makes it (torchaudio's kaldi.fbank with its
 * defaults): 16 kHz samples (-1..1, not rescaled), 25 ms frames every 10 ms with no frame past either end, no dither,
 * each frame's mean taken off, pre-emphasis 0.97, Povey's window, a 512-point power spectrum, 80 triangles on the mel
 * scale from 20 Hz to 8 kHz, the log of each (at least FLT_EPSILON's); then each band's mean over the whole of it
 * taken off. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define DICTATE_FBANK_BANDS 80
#define DICTATE_FBANK_RATE  16000
#define DICTATE_FBANK_FRAME 400   /* samples: 25 ms */
#define DICTATE_FBANK_HOP   160   /* 10 ms */

/* the frames `n` samples make: 0 for fewer than a frame's */
gsize   dictate_fbank_frames (gsize n);
/* frames × DICTATE_FBANK_BANDS, frame by frame; with `normalise`, each band's mean taken off. NULL (and 0 frames)
 * for fewer samples than a frame's */
float  *dictate_fbank        (const float *samples, gsize n, gboolean normalise, gsize *frames);

G_END_DECLS
