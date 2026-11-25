/* DictateEnv: what the app needs from the phone, and whether it is there now: GStreamer started (dictate-gst.h),
 * a microphone on the sound server, a network. None of it is waited for: the app shows its window at once, says
 * what is missing, and takes it up as it appears.
 *
 * The microphone is the sound server's (PipeWire's PulseAudio side): a subscribed context on the main loop hears a
 * capture source come and go, so nothing is polled. The network is GNetworkMonitor's.
 *
 * One of it for the whole app, made by dictate_env_start () on the main thread once the app is up. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define DICTATE_TYPE_ENV (dictate_env_get_type ())
G_DECLARE_FINAL_TYPE (DictateEnv, dictate_env, DICTATE, ENV, GObject)

/* made and started on the first call (GStreamer's start-up, the sound server, the network); the same one after */
DictateEnv  *dictate_env_start          (void);
/* GStreamer has started: the microphone and the voice may be used */
gboolean     dictate_env_audio_ready    (DictateEnv *self);
/* a capture source is there (or not asked yet: it is taken to be there until the sound server says it is not) */
gboolean     dictate_env_has_microphone (DictateEnv *self);
/* what keeps the microphone and the voice from being used, for the user, NULL for nothing */
const char  *dictate_env_audio_notice   (DictateEnv *self);
/* a network is up, as GNetworkMonitor says (taken to be, until it has reported or a few seconds have passed) */
gboolean     dictate_env_online         (DictateEnv *self);
/* the user asks (a tap on the microphone): what may have changed unseen is looked at again; "changed" follows if it
 * did. Never blocks. */
void         dictate_env_recheck        (DictateEnv *self);

/* Signals, on the main thread:
 *   "changed" (): the audio notice or the network's state may be different now */

G_END_DECLS
