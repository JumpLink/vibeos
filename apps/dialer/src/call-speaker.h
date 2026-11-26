/* The speaker in a call, where the sound card has it in a Voice Call profile of its own.
 *
 * The call audio daemon (callaudiod) turns the speaker on by moving the call's sink to its speaker port. That needs
 * the earpiece and the speaker as two ports of one sink. In a UCM configuration where both play through the same
 * PCM (the OnePlus 6T's, and most Qualcomm phones': the call itself goes through the DSP's voice mixers, not the
 * PCM), PipeWire cannot open both at once and makes a Voice Call profile for each: "Voice Call (Earpiece, Mic)" and
 * "Voice Call (Mic, Speaker)". Each one's sink has only its own port, and callaudiod finds nothing to switch to. So
 * the speaker goes on and off by switching the card between those profiles.
 *
 * Where one Voice Call profile has both ports (the Galaxy S10e's), this does nothing and the daemon switches the
 * port. */
#pragma once

#include <glib.h>
#include <pulse/introspect.h>

typedef struct _CallSpeaker CallSpeaker;

/* the Voice Call profile of @card to switch to for the speaker on (@speaker) or off; NULL when the active profile
 * already plays where wanted, when the card is not in a Voice Call profile (no call, or the daemon has not switched
 * yet), or when no profile plays there */
const char  *call_speaker_profile (const pa_card_info *card, gboolean speaker);

/* the sound server is reached at the first call_speaker_set () */
CallSpeaker *call_speaker_new (void);
void         call_speaker_free (CallSpeaker *self);

/* every card in a Voice Call profile switches to the one call_speaker_profile () names; with the speaker on, also a
 * card that comes into a Voice Call profile later */
void         call_speaker_set (CallSpeaker *self, gboolean speaker);
/* the call is over: nothing to switch back, the card leaves its Voice Call profile anyway */
void         call_speaker_reset (CallSpeaker *self);
