/* How the Clock writes times, days and durations: DeskClock's Utils, AlarmUtils and Weekdays, on GLib. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the week's first day: the "Start week on" setting, or the locale's (Weekdays.Order) */
GDateWeekday clock_week_start (GSettings *settings);

/* "08:30" in 24-hour mode ("Hm"), "8:30 AM" in 12-hour mode ("hma") */
char *clock_time_text (int hour, int minute, gboolean use_24h);
/* the same as Pango markup with the AM/PM at `ampm_ratio` of the size, bold (Utils.get12ModeFormat) */
char *clock_time_markup (int hour, int minute, gboolean use_24h, double ampm_ratio);
/* "Sat 8:30" / "Sat 8:30 AM" (AlarmUtils.getFormattedTime) */
char *clock_alarm_time_text (GDateTime *time, gboolean use_24h);

/* the "Alarm set for 23 hours and 59 minutes from now." snackbar (AlarmUtils.formatElapsedTimeUntilAlarm) */
char *clock_alarm_set_text (gint64 seconds_until);

/* the one-letter name of a weekday, Monday 0 .. Sunday 6 ("ccccc": M T W T F S S) */
char *clock_weekday_letter (int index);

/* what the ringtone row says: "Silent", a sound's name, or "Default (Hassium)" for the default sound */
char *clock_ringtone_title (const char *ringtone, GSettings *settings);

G_END_DECLS
