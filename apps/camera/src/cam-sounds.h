/* cam-sounds: the capture sounds: camera_click.ogg on a photo,
 * VideoRecord.ogg / VideoStop.ogg around a recording (AOSP frameworks/base data/sounds, Apache-2.0).
 *
 * Played as event sounds (PulseAudio's media.role "event"), when the "Shutter sound" setting is on and the phone is
 * not silenced: feedbackd's profile is "full" (as the Messaging app keeps to). NEO_CAMERA_SOUND_MUTE=1 plays them
 * muted, to test the path without a sound in the room. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  CAM_SOUND_SHUTTER,
  CAM_SOUND_VIDEO_START,
  CAM_SOUND_VIDEO_STOP,
} CamSound;

/* TRUE if it plays (the setting and the profile allow it) */
gboolean cam_sounds_play     (CamSound sound, gboolean setting);

G_END_DECLS
