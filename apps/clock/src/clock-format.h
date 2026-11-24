/* How the Clock writes times, days and durations. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the week's first day: the "Start week on" setting, or the locale's */
GDateWeekday clock_week_start (GSettings *settings);

/* the language's AM (@hour < 12) or PM, for a 12-hour clock; owned by the Clock */
const char *clock_day_period (int hour);
/* "08:30" in 24-hour mode ("Hm"), "8:30 AM" in 12-hour mode ("hma") */
char *clock_time_text (int hour, int minute, gboolean use_24h);
/* the same as Pango markup with the AM/PM at `ampm_ratio` of the size, bold */
char *clock_time_markup (int hour, int minute, gboolean use_24h, double ampm_ratio);
/* "Sat 8:30" / "Sat 8:30 AM" */
char *clock_alarm_time_text (GDateTime *time, gboolean use_24h);

/* the "Alarm set for 23 hours and 59 minutes from now." snackbar */
char *clock_alarm_set_text (gint64 seconds_until);

/* the one-letter name of a weekday, Monday 0 .. Sunday 6 ("ccccc": M T W T F S S) */
char *clock_weekday_letter (int index);

/* "default-alarm-ringtone" and "timer-ringtone" hold a sound's URI, "" for the default sound, or this for none
 * (an alarm's own ringtone is "" for none and NULL for the default) */
#define CLOCK_SILENT "silent"

/* what the ringtone row says: "Silent", a sound's name, or "Default (Hassium)" for the default sound */
char *clock_ringtone_title (const char *ringtone, GSettings *settings);
/* what "Timer sound" says: "Timer Expired", "Silent" or the sound's name */
char *clock_timer_sound_title (GSettings *settings);
/* a sound file's name, as the media scanner titles it: its TITLE tag (Ogg files), else its basename without the
 * extension */
char *clock_sound_name (const char *uri);

/* whether the phone can vibrate: "Vibrate" and "Timer vibrate" are hidden when it cannot */
gboolean clock_has_vibrator (void);

G_END_DECLS
