/* RingtonePicker: the alarm and timer sound picker, opened from an alarm's ringtone row or Settings ▸ Timer sound.
 *
 * "Your sounds" (the files the user added, and "Add new"), then "Device sounds": Silent, the default sound, and every
 * alarm sound the system has. Tapping a sound selects it and plays it at the alarm volume; tapping the selected one
 * stops or plays it again. The choice is saved as the page goes: for an alarm, as that alarm's sound and the default
 * for new alarms; for timers, as the timer sound.
 *
 * Signals: "closed" (): Up was pressed. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_RINGTONE_PICKER (ringtone_picker_get_type ())
G_DECLARE_FINAL_TYPE (RingtonePicker, ringtone_picker, CLOCK, RINGTONE_PICKER, GtkWidget)

GtkWidget *ringtone_picker_new_for_alarm (GSettings *settings, guint alarm_id);
GtkWidget *ringtone_picker_new_for_timer (GSettings *settings);

G_END_DECLS
