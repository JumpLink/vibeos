/* The app's side of the stopwatch (de.yesman.Clock.Stopwatch in the Clock's service, stopwatch-service.c): as
 * timers-client.h, for the one stopwatch. */
#pragma once

#include "stopwatch-model.h"

#include <gio/gio.h>

G_BEGIN_DECLS

#define STOPWATCH_TYPE_CLIENT (stopwatch_client_get_type ())
G_DECLARE_FINAL_TYPE (StopwatchClient, stopwatch_client, STOPWATCH, CLIENT, GObject)

/* the one client of the process */
StopwatchClient *stopwatch_client_get (void);

/* the stopwatch as the service last said; reset until it first answered */
const Stopwatch *stopwatch_client_get_stopwatch (StopwatchClient *self);

void stopwatch_client_start (StopwatchClient *self);
void stopwatch_client_pause (StopwatchClient *self);
void stopwatch_client_reset (StopwatchClient *self);
void stopwatch_client_lap (StopwatchClient *self);

/* Signal: "changed" */

G_END_DECLS
