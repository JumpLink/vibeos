/* dictate-cues: Agent's state cues, a short sound and a short vibration for listening, working, done and failed. Both
 * go through feedbackd, the phone's feedback service, like the shell's: so the phone's sound mode decides, a sound
 * only on "Sound", a vibration on "Sound" and "Vibrate", nothing on "Silent". Each can be switched off in Agent's
 * settings (cue-sounds, cue-haptics). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_CUE_LISTENING,        /* the microphone is about to listen */
  DICTATE_CUE_WORKING,          /* the question is sent, the answer is coming */
  DICTATE_CUE_DONE,             /* the answer is over */
  DICTATE_CUE_FAILED,           /* it went wrong: no answer */
} DictateCue;

/* plays `cue` as `settings` (vibeos.Dictate) allow; it never waits */
void dictate_cue_play (GSettings *settings, DictateCue cue);

G_END_DECLS
