/* The app's side of the timers (neo.Clock.Timers in the Clock's service, timer-service.c).
 *
 * As with the alarms (alarms-client.h), the service owns the timers and rings them; the app shows them and asks for
 * changes, and its list comes back through "changed". The timers are timer-model.h's: a running one is worked out
 * from its last start on the elapsed clock, which is the same here as in the service, so the app counts down by
 * itself and asks nothing while it does. */
#pragma once

#include "timer-model.h"

#include <gio/gio.h>

G_BEGIN_DECLS

#define TIMERS_TYPE_CLIENT (timers_client_get_type ())
G_DECLARE_FINAL_TYPE (TimersClient, timers_client, TIMERS, CLIENT, GObject)

/* the one client of the process */
TimersClient *timers_client_get (void);

/* Timer, the newest first; NULL until the service first answered */
GPtrArray *timers_client_get_timers (TimersClient *self);
Timer     *timers_client_find (TimersClient *self, guint id);

typedef void (*TimersAdded) (guint id, gpointer data);

/* a new timer, reset; @added gets its id */
void timers_client_add (TimersClient *self, gint64 length, const char *label, TimersAdded added, gpointer data);
void timers_client_start (TimersClient *self, guint id);
void timers_client_pause (TimersClient *self, guint id);
/* back to its length (deleted, if it rang and another app asked for it) */
void timers_client_reset (TimersClient *self, guint id);
void timers_client_add_minute (TimersClient *self, guint id);
void timers_client_delete (TimersClient *self, guint id);
void timers_client_set_label (TimersClient *self, guint id, const char *label);
/* every ringing timer stopped */
void timers_client_reset_expired (TimersClient *self);

/* Signal: "changed" (the timers) */

G_END_DECLS
