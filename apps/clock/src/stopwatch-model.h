/* The clock's stopwatch, as DeskClock keeps it.
 *
 * Like a timer (timer-model.h), a running stopwatch does not tick: it keeps the time it had at its last start and
 * when that was, on the elapsed clock, and the total now is worked out from those. A lap is noted as the total at
 * the moment it was taken; its own time is the difference from the lap before.
 *
 * Every time is in milliseconds. The service and the app both use this file; only the service writes the store. */
#pragma once

#include <glib.h>

typedef enum {
  STOPWATCH_RESET,
  STOPWATCH_RUNNING,
  STOPWATCH_PAUSED,
} StopwatchState;

#define STOPWATCH_MAX_LAPS 98                   /* the most laps it takes */

typedef struct {
  StopwatchState state;
  gint64 last_start;            /* the elapsed clock at the last start; running only */
  gint64 last_start_wall;       /* ... and the wall clock then */
  gint64 accumulated;           /* the total at the last start (running) or now (paused) */
  GArray *laps;                 /* gint64: the total at the end of each lap, the first lap first */
  char *boot_id;                /* the boot its times were saved in (elapsed_boot_id) */
} Stopwatch;

Stopwatch *stopwatch_new (void);
Stopwatch *stopwatch_copy (const Stopwatch *stopwatch);
void       stopwatch_free (Stopwatch *stopwatch);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Stopwatch, stopwatch_free)

/* the time on the stopwatch */
gint64   stopwatch_total (const Stopwatch *stopwatch, gint64 now);

void     stopwatch_start (Stopwatch *stopwatch, gint64 now, gint64 wall);
void     stopwatch_pause (Stopwatch *stopwatch, gint64 now);
/* back to zero, the laps gone */
void     stopwatch_reset (Stopwatch *stopwatch);
/* a lap ends now; FALSE when not running or at the limit of laps */
gboolean stopwatch_lap (Stopwatch *stopwatch, gint64 now);
gboolean stopwatch_can_lap (const Stopwatch *stopwatch);
void     stopwatch_update_after_reboot (Stopwatch *stopwatch, gint64 now, gint64 wall);
void     stopwatch_update_after_time_set (Stopwatch *stopwatch, gint64 now, gint64 wall);

/* lap @number's own time (1 is the first) */
gint64   stopwatch_lap_time (const Stopwatch *stopwatch, guint number);
/* the time of the lap still going, at @total */
gint64   stopwatch_current_lap_time (const Stopwatch *stopwatch, gint64 total);
/* the longest lap, the one still going included; 0 without laps */
gint64   stopwatch_longest_lap_time (const Stopwatch *stopwatch, gint64 total);

/* --- the store: one key file with a [stopwatch] group */

/* the stopwatch in the file, or a reset one when there is no file yet */
Stopwatch *stopwatch_load (const char *path, GError **error);
gboolean   stopwatch_save (const Stopwatch *stopwatch, const char *path, GError **error);
