/* cam-shutdown: work the process must not end before, counted across threads.
 *
 * The camera closes in the pipeline's worker thread (cam_pipeline_stop () only asks for it), so the close asked for as
 * the last window goes ran on while main () returned: libcamera's software ISP went on debayering on the GPU in a
 * thread of its own, and exit () running Mesa's handlers under it ended in a SIGSEGV in libgallium (Xiaomi Pad 5,
 * 2026-11-26: the camera closed by SIGTERM right after a recording). The application's shutdown waits for the count
 * to come down.
 *
 * The wait runs the main context, owned by the waiting thread: the closing elements hand work to the main thread
 * (gtk4paintablesink flushes its paintable there), and a g_main_context_invoke () into a context nobody owns runs in
 * the caller's own thread instead, where the paintable's thread guard aborts the process. */
#pragma once

#include <glib.h>

/* one more piece of work to finish first; any thread */
void     cam_shutdown_hold    (void);
/* one of them finished; any thread */
void     cam_shutdown_release (void);
/* the main thread: runs the default main context until none is left, at most timeout_ms; TRUE when none is, FALSE
 * when the time ran out first */
gboolean cam_shutdown_wait    (guint timeout_ms);
