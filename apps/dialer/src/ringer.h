/* The ringer: the ringtone and the vibration of an incoming call (Telecom's Ringer and AsyncRingtonePlayer).
 *
 * Android rings by its ringer mode, which the volume panel sets: Ring plays the ringtone (and vibrates too if "Also
 * vibrate for calls" is on), Vibrate only vibrates, Mute does neither. On this phone feedbackd's profile is the
 * ringer mode (full, quiet, silent: extension/volume.js), so the ringer reads it each time a call comes.
 *
 * Android rings on the ring stream, at its own volume, loud even with the media turned down. The phone has one
 * speaker and no volume per kind of sound, so while it rings the speaker is set to the "Ring volume" and put back
 * after, as the Clock does for an alarm (apps/clock/src/alarm-klaxon.h). */
#pragma once

#include <gio/gio.h>

typedef struct _Ringer Ringer;

/* the "ringtone" setting's two words besides a URI: the default ringtone (Orion; also the empty string) and None */
#define RINGER_DEFAULT "default"
#define RINGER_SILENT  "none"

/* reads its settings from @settings (neo.Dialer) each time it starts */
Ringer  *ringer_new (GSettings *settings);
void     ringer_free (Ringer *ringer);

/* rings for an incoming call until stopped; again while it rings does nothing */
void     ringer_start (Ringer *ringer);
void     ringer_stop (Ringer *ringer);
gboolean ringer_is_ringing (Ringer *ringer);

/* one of InCallUI's in-call vibrations (InCallVibrationHandler): on for @on1 ms, off for @off, on for @on2; none
 * when the ringer is on Mute. (Android also plays a tone into the call when a second one waits. Here the call's
 * sound runs in the modem's audio path with the phone's sound server stopped, so nothing can play into it.) */
void     ringer_vibrate (Ringer *ringer, guint on1, guint off, guint on2);

/* Settings' ringtone picker: @uri (or RINGER_DEFAULT) played once at the ring volume, NULL to stop. Nothing while
 * it rings, or in Vibrate or Mute */
void     ringer_preview (Ringer *ringer, const char *uri);
/* the main dialpad's keypad tone for @key while it is held (DialpadFragment.playTone), when "Keypad tones" is on */
void     ringer_play_tone (Ringer *ringer, char key);
void     ringer_stop_tone (Ringer *ringer);
