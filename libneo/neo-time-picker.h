/* NeoTimePicker: Android's clock-face time picker (android.widget.TimePicker in its material mode:
 * TimePickerClockDelegate and RadialTimePickerView), and the dialog that holds it (TimePickerDialog).
 *
 *   header  100 dp: the hours and minutes, 60 dp, the selected one bright; AM and PM beside them in 12-hour mode.
 *           Tapping the hours or the minutes shows that dial.
 *   dial    a 296 dp disc. Hours on a ring 22 dp inside its edge (in 24-hour mode 1-12 there and 00, 13-23 on an
 *           inner ring 58 dp in); minutes every five. A 20 dp selector circle on a 2 dp line from a 3 dp centre dot
 *           marks the value, and the number under it shows through in the dark colour.
 *
 * Dragging on the dial picks a value (hours snap to the hour, minutes to the minute); lifting the finger off the
 * hours goes on to the minutes, cross-fading the dials over 500 ms. Colours are DeskClock's Widget.TimePicker:
 * header and dial surfaceVariant on colorSurface, the selector colorAccent. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_TYPE_TIME_PICKER (neo_time_picker_get_type ())
G_DECLARE_FINAL_TYPE (NeoTimePicker, neo_time_picker, NEO, TIME_PICKER, GtkWidget)

GtkWidget *neo_time_picker_new      (int hour, int minute, gboolean use_24h);
void       neo_time_picker_get_time (NeoTimePicker *self, int *hour, int *minute);

/* The dialog: the picker over Cancel and OK. `chosen` runs on OK only; `destroy` (or NULL) frees `data` once the
 * dialog is answered either way. */
typedef void (*NeoTimeChosen) (int hour, int minute, gpointer data);
void       neo_time_picker_dialog   (GtkWidget *parent, int hour, int minute, NeoTimeChosen chosen, gpointer data,
                                     GDestroyNotify destroy);

/* the 24-hour setting the system clock uses (org.gnome.desktop.interface clock-format) */
gboolean   neo_clock_uses_24h       (void);

G_END_DECLS
