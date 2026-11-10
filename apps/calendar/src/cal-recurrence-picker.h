/* Etar's RecurrencePickerDialog (com.android.calendar.recurrencepicker): the editor's "Repeat".
 *
 * A dialog with no title (300 × 340 dp on a phone held upright, 500 × 281 dp on its side; 480 × 300 and 600 × 300
 * on a tablet): "REPEAT WEEKLY ▾" (the frequency) and the repeat switch on top, then "Every [1] week", the weekdays
 * as round toggles (WeekButton; four over three on a narrow screen), for a monthly rule "on the same day each month"
 * or "on every second Tuesday", and the end: "Forever ▾", "Until ▾ 12/10/2026" (a tap on the date picks another)
 * or "For ▾ [5] events". A line, then DONE across the bottom. With the switch off everything but the switch is
 * greyed, and Done means "does not repeat". A tap outside or Back closes it unchanged.
 *
 * The rule is parsed and written as Etar's EventRecurrence does it (calendarcommon2), so what it writes is what
 * Etar would: "FREQ=WEEKLY;WKST=MO;BYDAY=TU,TH", "FREQ=MONTHLY;UNTIL=20261231T090000Z;WKST=SU;BYDAY=-1FR". The
 * parts of a rule the dialog has no control for (BYMONTH, BYSETPOS, BYHOUR …) are kept; BYDAY and BYMONTHDAY are
 * rewritten from what it shows, as Etar's are. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @rrule the rule chosen ("FREQ=WEEKLY;BYDAY=MO"), NULL for "does not repeat"; not called when dismissed */
typedef void (*CalRecurrenceSet) (const char *rrule, gpointer data);

/* RecurrencePickerDialog.canHandleRecurrenceRule: a rule the dialog can show (else the editor's button is off).
 * TRUE for none (NULL); FALSE for a rule Etar's parser refuses, and for the ones its dialog would throw on. */
gboolean cal_recurrence_picker_can_handle (const char *rrule);

/* the dialog over @parent for @rrule (NULL: none yet), the event starting at @start in @tzid; @tzid NULL is an
 * all-day event, whose dates have no zone: its end date is written as a DATE (UNTIL=20261231), RFC 5545's rule for
 * an all-day start. Done with the rule as it opened (nothing changed in the dialog) answers that rule itself, as
 * it was written. */
void     cal_recurrence_picker_show       (GtkWidget *parent, const char *rrule, gint64 start, const char *tzid,
                                           CalRecurrenceSet done, gpointer data);

/* @rrule with its UNTIL of the kind the event's start is (RFC 5545): a DATE for an all-day event (@tzid NULL: the
 * local date a UTC end falls on), else that date at the start's time of day in @tzid, in UTC. The rule chosen
 * before All day was switched would otherwise keep the other kind, and libical ends an all-day rule with a UTC
 * UNTIL a day early. Anything else comes back as it is. */
char    *cal_recurrence_fit_until         (const char *rrule, gint64 start, const char *tzid);

/* --- what the dialog works on, for its tests */

/* EventRecurrence's frequencies (its constants) */
typedef enum {
  CAL_RECUR_SECONDLY = 1,
  CAL_RECUR_MINUTELY,
  CAL_RECUR_HOURLY,
  CAL_RECUR_DAILY,
  CAL_RECUR_WEEKLY,
  CAL_RECUR_MONTHLY,
  CAL_RECUR_YEARLY,
} CalRecurFreq;

typedef struct {
  int n;
  int *values;
} CalRecurList;

/* EventRecurrence: an RRULE in parts. Weekdays are 0 Sunday … 6 Saturday (Time's). */
typedef struct {
  CalRecurFreq freq;
  char *until;                  /* as written: "20261231T225959Z", "20261231T235959" (floating), "20261231"; or NULL */
  int count;                    /* 0: none */
  int interval;                 /* 0: none (every one) */
  int wkst;                     /* WKST; Monday when the rule has none */
  int byday_count;
  int *byday;                   /* weekdays … */
  int *byday_num;               /* … and which of them in the month or year (1, 2, −1 …; 0: every one) */
  CalRecurList bysecond, byminute, byhour, bymonthday, byyearday, byweekno, bymonth, bysetpos;
} CalRecurrence;

/* EventRecurrence.parse: NULL for a rule it throws InvalidFormatException on (no FREQ, an unknown part, a part
 * twice, a number out of range …). X- parts are dropped, as Etar drops them. */
CalRecurrence *cal_recurrence_parse     (const char *rrule);
/* a rule with no parts but WKST @wkst: the one the dialog starts a new rule from */
CalRecurrence *cal_recurrence_new       (int wkst);
/* EventRecurrence.toString: FREQ, UNTIL, COUNT, INTERVAL, WKST, BYSECOND, BYMINUTE, BYHOUR, BYDAY, BYMONTHDAY,
 * BYYEARDAY, BYWEEKNO, BYMONTH, BYSETPOS, in that order */
char          *cal_recurrence_to_string (const CalRecurrence *er);
void           cal_recurrence_free      (CalRecurrence *er);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CalRecurrence, cal_recurrence_free)

/* RecurrenceModel */
typedef enum { CAL_REPEAT_DAILY, CAL_REPEAT_WEEKLY, CAL_REPEAT_MONTHLY, CAL_REPEAT_YEARLY } CalRepeatFreq;
typedef enum { CAL_REPEAT_END_NEVER, CAL_REPEAT_END_BY_DATE, CAL_REPEAT_END_BY_COUNT } CalRepeatEnd;
typedef enum { CAL_REPEAT_MONTHLY_BY_DATE, CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK } CalRepeatMonthly;

#define CAL_REPEAT_INTERVAL_MAX 99
#define CAL_REPEAT_COUNT_MAX    730
#define CAL_REPEAT_COUNT_DEFAULT 5

typedef struct {
  gboolean repeats;             /* the switch (recurrenceState) */
  CalRepeatFreq freq;
  int interval;                 /* every n days, weeks … */
  CalRepeatEnd end;
  int end_day;                  /* UNTIL's day (a day number) in the event's zone, the local one for all-day */
  int end_second;               /* UNTIL's time on that day, seconds into it (the start's for a new end) */
  int end_count;
  gboolean weekly_by_day_of_week[7];
  CalRepeatMonthly monthly_repeat;
  int monthly_by_month_day;     /* BYMONTHDAY, 0 none */
  int monthly_by_day_of_week;   /* the weekday of "on every second Tuesday" … */
  int monthly_by_nth_day_of_week; /* … and which (1 … 5, −1 last) */
} CalRecurrenceModel;

/* onCreateView: the model for @er (its parts become the model's, copyEventRecurrenceToModel; NULL or a rule without
 * FREQ: a new rule, not repeating) on an event starting at @start in @tzid (NULL: all-day). FALSE where Etar's
 * dialog would throw (a monthly rule's BYDAY without a supported nth, …): it is not shown. */
gboolean cal_recurrence_model_init  (CalRecurrenceModel *model, const CalRecurrence *er, gint64 start,
                                     const char *tzid);
/* copyModelToEventRecurrence and toString: the rule @model makes of @er (whose other parts stay), NULL when it does
 * not repeat. @er is changed as Etar's is. */
char    *cal_recurrence_model_store (const CalRecurrenceModel *model, CalRecurrence *er, const char *tzid);
gboolean cal_recurrence_model_equal (const CalRecurrenceModel *a, const CalRecurrenceModel *b);

G_END_DECLS
