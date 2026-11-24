/* CalSide: a tablet's controls beside the Day and Week views, as Etar's: a small month over the calendars.
 *
 * The small month has the month's name, the weekday initials and six weeks; the days the view shows are on a band,
 * today is framed, the other months' days are dimmer. A tap on a day goes to it ("day-activated"); a swipe up or
 * down, or a scroll, shows the next or the previous month. The calendars are a list of a colour square and the
 * name; a tap shows or hides that calendar's events (a hidden one's square is only a frame).
 *
 * Held sideways it is a column, the month over the calendars; held upright a strip, the month at its start and the
 * calendars beside it (cal_side_set_strip).
 *
 * Signals:
 *   "day-activated" (int day)   a tap on a day of the small month */
#pragma once

#include "cal-store.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_SIDE_COLUMN_WIDTH 280       /* the column held sideways */
#define CAL_SIDE_STRIP_HEIGHT 300       /* the strip held upright */

#define CAL_TYPE_SIDE (cal_side_get_type ())
G_DECLARE_FINAL_TYPE (CalSide, cal_side, CAL, SIDE, GtkWidget)

GtkWidget *cal_side_new            (CalStore *store);
/* the strip (upright) or the column (sideways) */
void       cal_side_set_strip      (CalSide *self, gboolean strip);
/* 0 Sunday … 6 Saturday */
void       cal_side_set_week_start (CalSide *self, int week_start);
/* the days the view shows, @first to @last: the band, and the month of @first */
void       cal_side_set_days       (CalSide *self, int first, int last);

G_END_DECLS
