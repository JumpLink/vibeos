/* GStreamer's start-up and its state changes, off the main thread.
 *
 * gst_init () can hang for good: with no plugin registry it runs gst-plugin-scanner, whose plugins open every device
 * (a camera whose driver never answers holds it forever), and a state change that opens a sound sink waits for the
 * sound server. A service that did either on its main thread stopped answering D-Bus and showed nothing. So
 * gst_start_begin () starts GStreamer on a thread of its own, early in the service's life, so that it is ready by the
 * first sound, and a sound that wants GStreamer waits with gst_start_wait () instead of for it. */
#pragma once

#include <gst/gst.h>

/* the wait ends with ready TRUE when GStreamer is ready, FALSE when it is not within GST_START_WAIT_MS or failed;
 * from the main loop, never from inside gst_start_wait () */
typedef void (*GstStartReady) (gboolean ready, gpointer data);
/* a state change done, in the thread that made it */
typedef void (*GstStartDone) (gpointer data);

#define GST_START_WAIT_MS 10000

/* starts GStreamer on a thread of its own, once; main thread only. @factories (NULL-terminated, or NULL): elements
 * whose plugins are loaded once it is up, so that the first sound does not load them. A start that does not finish
 * is not tried again (GStreamer's init lock would hold the second too); a later wait finds out if it finished since. */
void     gst_start_begin (const char * const *factories);
/* whether GStreamer is up: only then may it be called on the main thread */
gboolean gst_start_is_ready (void);
/* @ready called once GStreamer is up, or after GST_START_WAIT_MS without it; the id (never 0) cancels the wait */
guint    gst_start_wait (GstStartReady ready, gpointer data);
void     gst_start_cancel (guint id);

/* @element to @state on a thread of its own, in the order asked, so the main thread does not wait for a sound sink
 * to open or close; @done, if any, runs there after it. Only once GStreamer is up. */
void     gst_start_set_state (GstElement *element, GstState state, GstStartDone done, gpointer data);
