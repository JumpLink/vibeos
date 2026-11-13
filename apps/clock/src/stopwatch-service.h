/* The stopwatch in the Clock's service: it keeps it (the store, ~/.local/share/neo.Clock/stopwatch.ini) and serves it
 * to the app over D-Bus (neo.Clock.Stopwatch, in stopwatch-service.c). It never ticks either (stopwatch-model.h); while
 * the app is not in front and the stopwatch has time on it, a notification shows it running, with its buttons. */
#pragma once

#include <gio/gio.h>

typedef struct _StopwatchService StopwatchService;

/* serves the stopwatch on @session; NULL (and @error) if it cannot */
StopwatchService *stopwatch_service_new (GDBusConnection *session, const char *store_path, GError **error);

/* the Clock app is in front (or not): the stopwatch then shows in it, not in a notification */
void              stopwatch_service_set_foreground (StopwatchService *self, gboolean foreground);
/* the wall clock was set */
void              stopwatch_service_time_set (StopwatchService *self);
