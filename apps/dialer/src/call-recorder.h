/* The service's call recorder: a call's audio into a file, and the announcement that the call is being recorded.
 *
 * What it records is the sound card's "Capture Voice" PCM (call-audio-ucm.h), both sides of the call, read through the
 * sound server (PipeWire's pulse server) from the source it has on that PCM. The announcement goes to the sink on the
 * "Play Voice" PCM, into the call, for the other side, and to the default sink, the phone's own during a call, for
 * this side. Those are nodes of the card's "Voice Call" profile, which the call audio daemon (callaudiod, or a phone's
 * own) switches to as the call comes up: the recorder waits for them, up to CALL_RECORDER_WAIT_MS.
 *
 * A recording is Ogg Opus, mono (the two sides mixed), written as it goes, so one cut short still plays. Its folder is
 * made 0700 and the file 0600: only the user reads their recordings. With an announcement asked for, a call is
 * recorded only as announced: when it cannot be played to the other side, the recording ends and its file goes.
 *
 * One recording at a time. */
#pragma once

#include <glib.h>

#define CALL_RECORDER_WAIT_MS 10000

typedef struct _CallRecorder CallRecorder;

/* the recording ended by itself: failed when it never recorded (no source in time, the file could not be written,
 * the announcement could not be played into the call), not when its source went after it had recorded */
typedef void (*CallRecorderStopped) (gboolean failed, gpointer data);

CallRecorder *call_recorder_new (CallRecorderStopped stopped, gpointer data);
void          call_recorder_free (CallRecorder *self);

/* record into @path (its folder made as needed) from @capture_pcm, as PipeWire names it (call_audio_ucm_pcm_path),
 * and once that runs, play @announcement (a sound file) into @playback_pcm and the phone's own sink; NULL for no
 * announcement. FALSE with @error when it is recording already; what goes wrong later ends it with stopped () */
gboolean      call_recorder_start (CallRecorder *self, const char *path, const char *capture_pcm,
                                   const char *playback_pcm, const char *announcement, GError **error);
/* the recording ends, its file complete; stopped () is not called */
void          call_recorder_stop (CallRecorder *self);
gboolean      call_recorder_is_recording (CallRecorder *self);

/* the announcement in the user's language (g_get_language_names), German or English, from
 * vibeos-dialer/sounds in the system's data directories; NULL when it is not there */
char         *call_recorder_announcement (void);

/* for the tests: GStreamer elements in place of pulsesrc and pulsesink, with no sound server to look the PCMs up in
 * ("audiotestsrc is-live=true", "fakesink sync=true") */
void          call_recorder_set_test_elements (CallRecorder *self, const char *source, const char *sink);
