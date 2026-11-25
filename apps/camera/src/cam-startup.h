/* cam-startup: GStreamer's start and the first look at the cameras, off the main thread.
 *
 * gst_init (the plugin registry, and with no cache a scan that loads every plugin, the V4L2 ones open every
 * /dev/video*), libcamera's enumeration (it opens /dev/media* and /dev/video*) and the building of the stream's
 * pipeline may all wait on a driver that never answers. None of it runs on the main loop: the window is up first and
 * says what it is waiting for, and a stuck start leaves the app answering.
 *
 * Nothing of GStreamer may be called before cam_gst_ready () (the window gates every path on it). gst_init is
 * called once, by the first run; a run that is still stuck in it is not repeated (a second call would wait on
 * GStreamer's init lock for ever), the caller only asks again whether it has finished. */
#pragma once

#include "cam-pipeline.h"

G_BEGIN_DECLS

/* TRUE once gst_init has returned: GStreamer may be used from here on */
gboolean cam_gst_ready      (void);

/* Runs on the main loop when a run has finished: the cameras libcamera listed (never NULL, maybe empty), the
 * pipeline built if there is a camera (else NULL, and `error` says why when it could not be built). Takes over
 * `cameras`, `pipeline` and `error`. */
typedef void (*CamStartupDone) (GPtrArray *cameras, CamPipeline *pipeline, GError *error, gpointer data);

/* Starts a run on a thread of its own: gst_init if it has not been called yet, the cameras, and the pipeline if there
 * is one (libcamerasrc is made once in a process: a run that listed a camera is the last one). `done` follows on the
 * main loop, however long it takes; the caller starts no second run before it. */
void     cam_startup_run    (CamStartupDone done, gpointer data);

G_END_DECLS
