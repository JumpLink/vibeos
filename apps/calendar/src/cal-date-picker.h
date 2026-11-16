/* The date dialog, for "Go to…" and the editor's dates. A libvibeos candidate.
 *
 * The header (the title, the chosen date large), the month with ‹ › and a swipe between months, a tap on
 * "November 2026 ▾" for a list of years, the days in circles (today ringed, the choice filled), Cancel and OK.
 * The pencil in the header switches to typing the date (the text input mode: an outlined "Date" field in the
 * locale's short pattern, its errors under it), the calendar icon back. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CalDateChosen) (int day, gpointer data);

/* @day and the result are day numbers (cal_day_number); @week_start 0 Sunday … 6 Saturday; @chosen runs on OK */
void cal_date_picker_show (GtkWidget *parent, const char *title, int day, int week_start, CalDateChosen chosen,
                           gpointer data);

G_END_DECLS
