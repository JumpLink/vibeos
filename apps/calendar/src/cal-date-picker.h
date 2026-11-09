/* The date dialog: Material 3's MaterialDatePicker as Etar shows it for "Go to…" and the editor's dates
 * (spec/calendar.md §8.1; measured still:cal-dlg-date). A libneo candidate.
 *
 * The header (the title, the chosen date large), the month with ‹ › and a swipe between months, a tap on
 * "November 2026 ▾" for a list of years, the days in 40 dp circles (today ringed, the choice filled), Cancel and OK.
 * Left out: the pencil that switches to typing the date (MaterialDatePicker's text input mode): not built. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CalDateChosen) (int day, gpointer data);

/* @day and the result are day numbers (cal_day_number); @week_start 0 Sunday … 6 Saturday; @chosen runs on OK */
void cal_date_picker_show (GtkWidget *parent, const char *title, int day, int week_start, CalDateChosen chosen,
                           gpointer data);

G_END_DECLS
