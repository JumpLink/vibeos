/* CalMonthView: Etar's Month view (MonthByWeekFragment with MonthListView and MonthWeekEventsView;
 * spec/calendar.md §4): the weekday names, then an endless list of weeks, six to the screen, scrolled vertically.
 * The month in focus is lighter; each day shows its number and as many of its events as fit, then "+N"; how much of
 * an event shows follows the Display options (cal_month_view_set_details).
 *
 * Signals:
 *   "day-activated" (int day, gboolean has_events)   a tap on a day
 *   "moved" ()                                       the month in focus changed: the title and the events follow */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_MONTH_VIEW (cal_month_view_get_type ())
G_DECLARE_FINAL_TYPE (CalMonthView, cal_month_view, CAL, MONTH_VIEW, GtkWidget)

GtkWidget *cal_month_view_new            (void);
/* 0 Sunday … 6 Saturday */
void       cal_month_view_set_week_start (CalMonthView *self, int week_start);
/* "Show week number": each week's number in its first day's corner (drawWeekNums) */
void       cal_month_view_set_show_week_number (CalMonthView *self, gboolean show);
/* the month of @day in focus, its first week at the top (SimpleDayPickerFragment.goTo) */
void       cal_month_view_go_to          (CalMonthView *self, int day);
/* the first day of the month in focus */
int        cal_month_view_get_focus_day  (CalMonthView *self);
/* the days whose events the view wants: the focus month and the weeks a scroll may show around it */
void       cal_month_view_get_range      (CalMonthView *self, int *first, int *last);
void       cal_month_view_set_events     (CalMonthView *self, GPtrArray *events);
/* Display options for the orientation shown (ViewDetailsPreferences): an event's lines at most (1 … 7), its time
 * (0 none, 1 start, 2 start and end, 3 start and duration, 4 start and end on a line below), its location on a line
 * of its own. A phone's defaults: 7, 0, FALSE. */
void       cal_month_view_set_details    (CalMonthView *self, int max_lines, int time, gboolean location);

G_END_DECLS
