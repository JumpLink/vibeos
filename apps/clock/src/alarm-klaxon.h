/* The klaxon: an alarm's sound and vibration while it rings (DeskClock's AlarmKlaxon and AsyncRingtonePlayer).
 *
 * The sound loops until stopped, optionally rising from near silence over the "Gradually increase volume" time.
 * Android rings alarms on their own volume (STREAM_ALARM), loud even with the media volume down or muted; this phone
 * has one speaker and no volume per kind of sound, so while an alarm rings the speaker is unmuted and set to the
 * "Alarm volume", and put back as it was after. The vibration is feedbackd's alarm-clock-elapsed event. */
#pragma once

#include <gio/gio.h>

typedef struct _AlarmKlaxon AlarmKlaxon;

/* reads the alarm settings from @settings (de.yesman.Clock) each time it starts */
AlarmKlaxon *alarm_klaxon_new (GSettings *settings);
void         alarm_klaxon_free (AlarmKlaxon *klaxon);

/* @ringtone: a sound file's URI; NULL for the default alarm sound; "" for none (vibration only) */
void         alarm_klaxon_start (AlarmKlaxon *klaxon, const char *ringtone, gboolean vibrate);
void         alarm_klaxon_stop (AlarmKlaxon *klaxon);

/* the file of a sound found in the data dirs ("sounds/android/alarms/Hassium.ogg"), as a URI, or NULL */
char        *alarm_klaxon_find_sound (const char *relative_path);
