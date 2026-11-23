/* navi-plan: a planned drive ("Leave later"): where to, and a reminder when it is time to leave.
 *
 * Without traffic data a planned arrival time would mean nothing, so what Navi plans is the reminder: at the time
 * picked, a notification says "Time to leave for Potsdamer Platz", and a tap on it opens the routes there from where
 * the car is then. One plan at a time; a new one replaces it.
 *
 * Navi need not be running at that time. The plan is kept in ~/.config/vibeos-navi/planned.ini, and the reminder is
 * a systemd user timer (a transient one, from systemd-run --user --on-calendar: unit vibeos-navi-leave), which runs
 * `vibeos-navi --leave-reminder` at the time. That posts the notification to the shell (GNotification) and exits. The
 * tap is the app's action "app.planned-drive": a running Navi gets it, or D-Bus activation starts one
 * (vibeos.Navi.service). A sleeping phone runs no timer, so vibeos-wakeup wakes it then (the real-time clock), as it
 * does for the Calendar's reminders. A transient timer does not outlive a reboot: Navi arms a plan still to come again
 * whenever it starts (navi_plan_rearm).
 *
 * Privacy: the place is in the plan's file only, as the saved places are in navi.ini; the timer's command line and
 * description say nothing of it, so the journal does not either. */
#pragma once

#include "navi-search.h"

G_BEGIN_DECLS

typedef struct {
  NaviPlace *place;
  gint64 time; /* when to leave: Unix seconds */
} NaviPlan;

/* the plan, or NULL */
NaviPlan *navi_plan_load (void);
void navi_plan_free (NaviPlan *plan);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NaviPlan, navi_plan_free)

typedef void (*NaviPlanDone) (gboolean armed, gpointer data);
/* plans a drive to `place` with its reminder at `time`, in place of any plan; `done` hears whether the reminder is
 * set (systemd-run worked) */
void navi_plan_set (const NaviPlace *place, gint64 time, NaviPlanDone done, gpointer data);
/* no plan any more, and no reminder */
void navi_plan_cancel (void);
/* at the app's start: a plan still to come gets its reminder again (a reboot took the timer), one long gone is
 * dropped */
void navi_plan_rearm (void);
/* `vibeos-navi --leave-reminder`, the timer's: posts the reminder when the plan is due; the exit status */
int navi_plan_remind (void);

/* when the reminder comes: "Reminder at 08:15", "Reminder tomorrow at 08:15" */
char *navi_plan_describe (gint64 time, gboolean use_24h);

G_END_DECLS
