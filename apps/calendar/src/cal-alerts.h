/* When reminders are due: Etar's AlertService and CalendarProvider's alarm scheduling, as pure rules over the
 * occurrences the store reads (spec/calendar.md §10). The reminder service (reminders-main.c) keeps the state and
 * does the notifying; this decides what and when, so the tests can check it.
 *
 * A reminder of an occurrence is due at its start less its minutes. One that came due while nothing ran (the phone
 * was off, the service was not running) still notifies while its event has not ended, as Etar's missed alerts do;
 * after the end it is dropped. Only reminders that notify count: Notification (VALARM DISPLAY) and Alarm (AUDIO);
 * an Email one is the server's to send. */
#pragma once

#include "cal-model.h"

G_BEGIN_DECLS

/* the key of one occurrence (its calendar, UID, RECURRENCE-ID and start), and of one of its reminders: short
 * strings, safe as GKeyFile keys */
char    *cal_alert_occurrence_key (const CalEvent *event);
char    *cal_alert_key            (const CalEvent *event, int minutes);

/* the reminders of @event that notify */
gboolean cal_alert_notifies (const CalReminder *reminder);

typedef struct {
  CalEvent *event;              /* a reference */
  int minutes;
  gint64 time;                  /* when it is due, Unix seconds */
  GPtrArray *keys;              /* the keys of every due reminder of the occurrence: all are notified by this one */
} CalAlert;

void cal_alert_free (CalAlert *alert);

/* Of @events (CalEvent), the reminders due at @now that are not in @fired (keys from cal_alert_key) and whose
 * event has not ended, into @due (CalAlert, latest per occurrence only: one notification per occurrence); the
 * earliest reminder after @now in @next (0 if none). */
void cal_alerts_compute (GPtrArray *events, GHashTable *fired, gint64 now, GPtrArray *due, gint64 *next);

G_END_DECLS
