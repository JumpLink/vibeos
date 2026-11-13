/* The editor's time picker, its clock 24- or 12-hour as the phone's (libneo's NeoTimePicker, a plainer dialog, is the
 * snooze delay's).
 *
 * "Select time"; the hour and the minute in two boxes, the one being chosen filled; a clock face: 0–11 outside and
 * 12–23 inside for the hour (a 12-hour clock: one ring, 12 1 … 11, and an AM/PM toggle), 00–55 for the minute; the
 * keyboard button, Cancel, OK. The keyboard button switches to the text input mode: two fields, Hour and Minute (and
 * AM/PM), typed from the keyboard; the clock button switches back.
 *
 * A libneo candidate, next to NeoTimePicker. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CalTimeChosen) (int hour, int minute, gpointer data);

void cal_time_picker_show (GtkWidget *parent, int hour, int minute, CalTimeChosen chosen, gpointer data);

G_END_DECLS
