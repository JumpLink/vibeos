/* The alarm engine: the rules that take each alarm instance through its life.
 *
 *   SILENT ── 2 h before ──> LOW_NOTIFICATION ── 30 min before ──> HIGH_NOTIFICATION ── its time ──> FIRED
 *                                  │ swiped away                                                    │
 *                                  └──> HIDE_NOTIFICATION ── 30 min before ──> HIGH_NOTIFICATION     │
 *   "Dismiss now" before it rings: PREDISMISSED ── its time ──> gone                                 │
 *   FIRED ── Snooze ──> SNOOZE ── snooze length later ──> FIRED                                      │
 *   FIRED ── Dismiss ──> gone; ── auto-silence ──> MISSED ── 12 h ──> gone  <────────────────────────┘
 *
 * When an instance is gone (or missed), its alarm makes the next one if it repeats, or switches off (or is deleted,
 * if it was made to be used once).
 *
 * The engine is logic only: it never sleeps, rings or shows anything. Its host (vibeos-clock-alarms) asks it when the
 * next change is due (alarm_engine_next_change), wakes the phone for then, and calls alarm_engine_tick() at that
 * time; it shows each instance as its state says (the "instance-changed" signal), takes down what showed one that is
 * gone ("instance-removed"), and saves the store when told ("store-changed"). The time comes from the clock set with
 * alarm_engine_set_clock, so the tests can set it. */
#pragma once

#include <glib-object.h>

#include "alarm-model.h"

#define ALARM_TYPE_ENGINE (alarm_engine_get_type ())
G_DECLARE_FINAL_TYPE (AlarmEngine, alarm_engine, ALARM, ENGINE, GObject)

/* the settings that the engine needs */
typedef struct {
  int snooze_minutes;           /* "Snooze length", 10 */
  int auto_silence_minutes;     /* "Silence after", 10; -1 never: it rings until it is answered */
} AlarmEngineSettings;

/* takes over @store */
AlarmEngine *alarm_engine_new (AlarmStore *store);
AlarmStore  *alarm_engine_get_store (AlarmEngine *engine);
void         alarm_engine_set_settings (AlarmEngine *engine, const AlarmEngineSettings *settings);
/* what the engine takes as "now"; NULL for the real clock */
void         alarm_engine_set_clock (AlarmEngine *engine, GDateTime *(*now) (gpointer data), gpointer data);

/* the user's changes: each makes the alarm's instance anew */
guint        alarm_engine_add_alarm (AlarmEngine *engine, const Alarm *alarm);
gboolean     alarm_engine_update_alarm (AlarmEngine *engine, const Alarm *alarm);
gboolean     alarm_engine_delete_alarm (AlarmEngine *engine, guint alarm_id);

/* the answers to an instance */
gboolean     alarm_engine_snooze (AlarmEngine *engine, guint instance_id);
/* a ringing or snoozed instance is over; one that has not rung yet is skipped ("Dismiss now") */
gboolean     alarm_engine_dismiss (AlarmEngine *engine, guint instance_id);
/* the "Upcoming alarm" notification was swiped away */
gboolean     alarm_engine_hide_notification (AlarmEngine *engine, guint instance_id);

/* every instance looked at anew, as after a reboot or a change of time or time zone */
void         alarm_engine_fix (AlarmEngine *engine);
/* the changes due by now */
void         alarm_engine_tick (AlarmEngine *engine);

/* when the next change is due, or NULL if none is */
GDateTime   *alarm_engine_next_change (AlarmEngine *engine);
/* the instance that rings next (the status bar's next alarm), or NULL */
AlarmInstance *alarm_engine_next_alarm (AlarmEngine *engine);
/* the instance ringing now, or NULL */
AlarmInstance *alarm_engine_ringing (AlarmEngine *engine);
