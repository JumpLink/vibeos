/* The clock's timers, as DeskClock keeps them.
 *
 * A running timer does not tick. It keeps when it last started (on the elapsed clock, elapsed.h) and how much was
 * left then; the time left now is worked out from those two whenever someone asks, so the service only has to wake
 * at the moment it runs out, and the app, given the same two numbers, shows the same time. At zero the timer
 * expires and rings, and goes on counting into overtime until it is stopped. One that ran out more than a minute
 * before the phone came back on is "missed" instead: it does not ring.
 *
 * The transitions below each change the timer in place; one that does not apply (a paused timer asked to pause,
 * say) leaves it as it is. Every time is in milliseconds, `now` on the elapsed
 * clock and `wall` on the wall clock.
 *
 * Both the clock service and the app use this file; only the service writes the store. */
#pragma once

#include <glib.h>

/* a Timer's states, by the numbers the store keeps */
typedef enum {
  TIMER_RUNNING = 1,
  TIMER_PAUSED = 2,
  TIMER_EXPIRED = 3,            /* ran out: ringing, counting the overtime */
  TIMER_RESET = 4,              /* set but not started, or back to its length */
  TIMER_MISSED = 5,             /* ran out while the phone was off */
} TimerState;

#define TIMER_MIN_LENGTH 1000                   /* the shortest timer, in ms */
#define TIMER_MISSED_THRESHOLD (-60 * 1000)     /* over a minute gone at boot: missed */

typedef struct {
  guint id;
  TimerState state;
  gint64 length;                /* as it was set */
  gint64 total_length;          /* the length with the minutes added since; 0 once it expired */
  gint64 last_start;            /* the elapsed clock when it last started (or expired); running ones only */
  gint64 last_start_wall;       /* ... and the wall clock then */
  gint64 remaining;             /* the time left at last_start; for a paused or reset one, the time left */
  char *label;                  /* "" for none */
  gboolean delete_after_use;    /* one another app asked for: gone, not reset, once it rang */
} Timer;

/* a new timer, reset, @length long */
Timer *timer_new (guint id, gint64 length, const char *label, gboolean delete_after_use);
Timer *timer_copy (const Timer *timer);
void   timer_free (Timer *timer);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Timer, timer_free)

/* the time left (negative in overtime) */
gint64   timer_remaining (const Timer *timer, gint64 now);
/* how much of the total has gone */
gint64   timer_elapsed (const Timer *timer, gint64 now);
/* when it runs (or ran) out, on the elapsed clock; for running, expired and missed timers */
gint64   timer_expiration (const Timer *timer);
/* the length it counts down from now */
gint64   timer_total_length (const Timer *timer);

void timer_start (Timer *timer, gint64 now, gint64 wall);
/* an expired or missed one is reset instead */
void timer_pause (Timer *timer, gint64 now);
void timer_expire (Timer *timer, gint64 now, gint64 wall);
void timer_miss (Timer *timer, gint64 now, gint64 wall);
void timer_reset (Timer *timer);
/* a minute more; an expired or missed one starts again with one minute */
void timer_add_minute (Timer *timer, gint64 now, gint64 wall);
void timer_set_label (Timer *timer, const char *label);
/* after a reboot (the elapsed clock started again: the wall clock says how long it was off) and after the wall
 * clock was set (the elapsed one is right: the wall time is noted again) */
void timer_update_after_reboot (Timer *timer, gint64 now, gint64 wall);
void timer_update_after_time_set (Timer *timer, gint64 now, gint64 wall);

/* the order by expiry: missed, expired, running, paused, reset, each the soonest first */
int  timer_compare_expiry (const Timer *a, const Timer *b, gint64 now);

/* --- the store: one key file, a group [timer N] for each */

typedef struct {
  GPtrArray *timers;            /* Timer, the newest first, as the list shows them */
  guint next_id;
  char *boot_id;                /* the boot its times were saved in (elapsed_boot_id) */
} TimerStore;

/* the store in the file, or an empty one when there is no file yet */
TimerStore *timer_store_load (const char *path, GError **error);
gboolean    timer_store_save (TimerStore *store, const char *path, GError **error);
void        timer_store_free (TimerStore *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (TimerStore, timer_store_free)

Timer      *timer_store_find (TimerStore *store, guint id);
/* @timer (the store takes it) with a new id, at the top of the list */
Timer      *timer_store_add (TimerStore *store, Timer *timer);
gboolean    timer_store_remove (TimerStore *store, guint id);
