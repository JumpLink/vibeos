/* dictate-gst: GStreamer's start-up, kept off the main thread.
 *
 * gst_init () scans the plugins when there is no registry cache, which forks gst-plugin-scanner, and a plugin's
 * driver that hangs hangs it for good. So the app does not call it before it shows its window: it runs on a thread
 * of its own once the app is up, and nothing calls into GStreamer until dictate_gst_ready () says it is done. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* a callback on the main thread: gst_init () has returned */
typedef void (*DictateGstDone) (gpointer data);

/* begins gst_init () on a thread; once only. `done` is called on the main thread when it has returned, however
 * late. Never blocks. */
void     dictate_gst_start (DictateGstDone done, DictateGstDone timeout, gpointer data);
/* gst_init () has returned (in any thread, at any time): GStreamer may be used. A program that initialised it
 * itself without calling dictate_gst_start () counts as ready. */
gboolean dictate_gst_ready (void);
/* gst_init () has not returned DICTATE_GST_TIMEOUT_S seconds after dictate_gst_start (): it is given up on for now,
 * and `timeout` is called on the main thread; `done` still comes if it returns later */
#define DICTATE_GST_TIMEOUT_S 10

G_END_DECLS
