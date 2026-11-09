/* The time picker of Etar's editor: Material 3's MaterialTimePicker in 24-hour clock mode (EditEventView:1714;
 * still:cal-dlg-time). libneo's NeoTimePicker is the platform TimePickerDialog, which Etar does not use.
 *
 * "Select time"; the hour and the minute in two boxes, the one being chosen in primaryContainer; a 256 dp clock
 * face: 0–11 outside and 12–23 inside for the hour, 00–55 for the minute; Cancel, OK.
 *
 * Left out: the keyboard button (the text input mode) at the bottom left: not built.
 *
 * A libneo candidate, next to NeoTimePicker. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CalTimeChosen) (int hour, int minute, gpointer data);

void cal_time_picker_show (GtkWidget *parent, int hour, int minute, CalTimeChosen chosen, gpointer data);

G_END_DECLS
