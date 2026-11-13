/* cam-recorder: one video recording, H.264 + AAC in MP4.
 *
 *   appsrc (the stream's frames, from cam_pipeline_set_recording) ! <H.264 encoder> ! taginject ! mp4mux
 *   pulsesrc ! audioconvert ! audioresample ! volume ! <AAC encoder>                         ! mp4mux
 *   mp4mux ! filesink .pending-<name>.mp4  → renamed to <name>.mp4 when it is complete
 *
 * Both pipelines run on the same clock, so a frame's capture time becomes the recording's time exactly, and the
 * microphone's samples line up with it. A pause drops frames and samples alike and takes the paused time out of
 * both. The picture is not turned: the file carries the
 * turn as its track matrix (the image-orientation tag), as a phone's recordings do. */
#pragma once

#include "cam-pipeline.h"
#include "cam-sensors.h"

G_BEGIN_DECLS

typedef struct _CamRecorder CamRecorder;

/* `done` gets the saved file's path once the recording has ended (cam_recorder_stop, or an error on the way), or
 * NULL and the error; the recorder is gone after it. */
typedef void (*CamRecorderDone) (const char *path, const GError *error, gpointer data);

/* `path` is the final file name; nothing exists under it until the file is complete. `rotation` is the clockwise
 * turn that shows the picture upright. `audio` FALSE records no sound track (MIC OFF). */
CamRecorder *cam_recorder_new          (CamPipeline *pipeline, const char *path, const char *encoder,
                                        int bitrate, gboolean audio, gboolean mirror, int rotation,
                                        const CamLocation *location, CamRecorderDone done, gpointer data,
                                        GError **error);
/* TRUE if the computer can record sound at all (a microphone source and an AAC encoder) */
gboolean     cam_recorder_can_record_audio (void);

void         cam_recorder_start        (CamRecorder *self);
void         cam_recorder_set_paused   (CamRecorder *self, gboolean paused);
void         cam_recorder_set_muted    (CamRecorder *self, gboolean muted);
/* recorded time without the pauses, µs */
gint64       cam_recorder_get_duration (CamRecorder *self);

/* finishes the file; `done` follows */
void         cam_recorder_stop         (CamRecorder *self);

G_END_DECLS
