/* NeoTimePicker: a clock-face time picker, and the dialog that holds it.
 *
 *   header  the hours and minutes, the selected one bright; AM and PM beside them in 12-hour mode.
 *           Tapping the hours or the minutes shows that dial.
 *   dial    a disc. Hours on a ring just inside its edge (in 24-hour mode 1-12 there and 00, 13-23 on an inner
 *           ring); minutes every five. A selector circle on a line from a centre dot marks the value, and the number
 *           under it shows through in the dark colour.
 *
 * Dragging on the dial picks a value (hours snap to the hour, minutes to the minute); lifting the finger off the
 * hours goes on to the minutes, cross-fading the dials over 500 ms. */
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
