/* The timers in the Clock's service: it keeps them (the store, ~/.local/share/neo.Clock/timers.ini), serves them to the
 * app over D-Bus (neo.Clock.Timers, in timer-service.c), and rings them.
 *
 * A running timer costs nothing until it runs out: one timerfd waits for the first to end, on the elapsed clock
 * (elapsed.h), which counts the time the phone sleeps; neo-wakeup wakes the phone for it. Then the timer rings (the
 * klaxon, the "Time's up" notification, and the ringing screen over the lock screen) until it is stopped or given
 * a minute more. While the app is not in front, the timers that are running or paused have a notification too, and
 * so do the ones missed while the phone was off. */
#pragma once

#include <gio/gio.h>

typedef struct _TimerService TimerService;

/* serves the timers on @session; NULL (and @error) if it cannot. @system: for logind and neo-wakeup, or NULL */
TimerService *timer_service_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings,
                                 const char *store_path, GError **error);

/* the Clock app is in front (or not): its timers then show in it, not in notifications */
void          timer_service_set_foreground (TimerService *self, gboolean foreground);
/* the wall clock was set: only the record of when each timer started on it changes */
void          timer_service_time_set (TimerService *self);
