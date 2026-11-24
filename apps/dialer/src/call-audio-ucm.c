/* What the sound card's UCM says about a call's audio (call-audio-ucm.h). */
#include "call-audio-ucm.h"

#include <alsa/asoundlib.h>
#include <alsa/use-case.h>
#include <string.h>

#define VOICE_CALL      SND_USE_CASE_VERB_VOICECALL     /* "Voice Call" */
#define CAPTURE_VOICE   SND_USE_CASE_MOD_CAPTURE_VOICE  /* "Capture Voice" */
#define PLAY_VOICE      SND_USE_CASE_MOD_PLAY_VOICE     /* "Play Voice" */

void
call_audio_ucm_clear (CallAudioUcm *ucm)
{
  g_clear_pointer (&ucm->capture_pcm, g_free);
  g_clear_pointer (&ucm->playback_pcm, g_free);
}

/* the verb's modifier has @modifier (the verb's list holds names and comments in turn) */
static gboolean
has_modifier (const char **list, int n, const char *modifier)
{
  for (int i = 0; i < n; i += 2)
    if (list[i] && !strcmp (list[i], modifier))
      return TRUE;
  return FALSE;
}

/* @value ("CapturePCM") of @modifier in the Voice Call verb only, not inherited from the verb or the card */
static char *
modifier_value (snd_use_case_mgr_t *uc, const char *value, const char *modifier)
{
  g_autofree char *id = g_strdup_printf ("=%s/%s/%s", value, modifier, VOICE_CALL);
  const char *found = NULL;
  char *copy;

  if (snd_use_case_get (uc, id, &found) < 0 || !found)
    return NULL;
  copy = *found ? g_strdup (found) : NULL;
  free ((char *) found);
  return copy;
}

gboolean
call_audio_ucm_probe_card (const char *card, CallAudioUcm *out)
{
  snd_use_case_mgr_t *uc = NULL;
  const char **list = NULL;
  int n;

  memset (out, 0, sizeof *out);
  if (snd_use_case_mgr_open (&uc, card) < 0)
    return FALSE;
  n = snd_use_case_get_list (uc, "_modifiers/" VOICE_CALL, &list);
  if (n > 0 && has_modifier (list, n, CAPTURE_VOICE))
    {
      out->capture_pcm = modifier_value (uc, "CapturePCM", CAPTURE_VOICE);
      if (out->capture_pcm && has_modifier (list, n, PLAY_VOICE))
        out->playback_pcm = modifier_value (uc, "PlaybackPCM", PLAY_VOICE);
    }
  if (n > 0)
    snd_use_case_free_list (list, n);
  snd_use_case_mgr_close (uc);
  return out->capture_pcm != NULL;
}

gboolean
call_audio_ucm_probe (CallAudioUcm *out)
{
  int card = -1;

  memset (out, 0, sizeof *out);
  while (snd_card_next (&card) == 0 && card >= 0)
    {
      /* "-": a card with no UCM configuration is no error */
      g_autofree char *name = g_strdup_printf ("-hw:%d", card);

      if (call_audio_ucm_probe_card (name, out))
        return TRUE;
    }
  return FALSE;
}

const char *
call_audio_ucm_pcm_path (const char *pcm)
{
  const char *p;

  if (!pcm || !g_str_has_prefix (pcm, "_ucm"))
    return pcm;
  for (p = pcm + 4; g_ascii_isxdigit (*p); p++)
    ;
  return *p == '.' ? p + 1 : pcm;
}
