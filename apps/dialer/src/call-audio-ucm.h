/* What the phone's sound card says it can do with a call's audio, read from its ALSA use case configuration (UCM).
 *
 * This is the way alsa-lib documents for recording a voice call (use-case.h: set the "Voice Call" verb, set the
 * "Capture Voice" modifier, and its CapturePCM carries the call). In the card's "Voice Call" verb:
 *   - the "Capture Voice" modifier: a capture PCM that carries the call, both sides: the other person and what the
 *     phone's microphones send;
 *   - the "Play Voice" modifier: a playback PCM that goes into the call's uplink, so the other person hears it.
 * PipeWire makes each modifier a node of the card's "Voice Call" profile, the one the call audio daemon (callaudiod,
 * or a phone's own) switches to for a call. A phone whose configuration has no "Capture Voice" cannot record calls;
 * one without "Play Voice" cannot announce a recording to the other person.
 *
 * Only read: no verb or modifier is set, and the card stays as it is. */
#pragma once

#include <glib.h>

typedef struct
{
  char *capture_pcm;            /* the Capture Voice PCM, "hw:S10e,4"; NULL when there is none */
  char *playback_pcm;           /* the Play Voice PCM; NULL when there is none */
} CallAudioUcm;

/* the first sound card with a Capture Voice modifier in its Voice Call verb: TRUE and its PCMs in @out, or FALSE
 * and @out cleared */
gboolean    call_audio_ucm_probe (CallAudioUcm *out);

/* the same for the card alsa-lib's UCM opens as @card ("hw:0"; or, for the tests, the name of a configuration for no
 * card in ALSA_CONFIG_UCM2's conf.virt.d) */
gboolean    call_audio_ucm_probe_card (const char *card, CallAudioUcm *out);

void        call_audio_ucm_clear (CallAudioUcm *ucm);

/* @pcm as PipeWire's api.alsa.path names it: without the "_ucmNNNN." prefix alsa-lib gives UCM's PCM names */
const char *call_audio_ucm_pcm_path (const char *pcm);
