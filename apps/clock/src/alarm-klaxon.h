/* The klaxon: an alarm's or a timer's sound and vibration while it rings.
 *
 * The sound loops until stopped, optionally rising from near silence over the "Gradually increase volume" time. An
 * alarm rings loud even with the media volume down or muted. This phone has one speaker and no volume per kind of
 * sound, so while an alarm rings the speaker is unmuted and set to the "Alarm volume", and put back as it was after.
 * The vibration is feedbackd's alarm-clock-elapsed event. */
#pragma once

#include <gio/gio.h>

typedef struct _AlarmKlaxon AlarmKlaxon;

/* what it rings for: each has its own default sound and "Gradually increase volume"; both ring at the alarm volume */
typedef enum {
  ALARM_KLAXON_ALARM,
  ALARM_KLAXON_TIMER,
  /* a sound tried out: the ringtone picker's and the alarm volume slider's, at the alarm volume, without a crescendo.
   * It gives way to an alarm or a timer: one starting stops it, and it does not start while one rings */
  ALARM_KLAXON_PREVIEW,
} AlarmKlaxonKind;

/* reads @kind's settings from @settings (vibeos.Clock) each time it starts */
AlarmKlaxon *alarm_klaxon_new (GSettings *settings, AlarmKlaxonKind kind);
void         alarm_klaxon_free (AlarmKlaxon *klaxon);

/* @ringtone: a sound file's URI; NULL for the default sound of its kind; "" for none (vibration only) */
void         alarm_klaxon_start (AlarmKlaxon *klaxon, const char *ringtone, gboolean vibrate);
void         alarm_klaxon_stop (AlarmKlaxon *klaxon);

/* whether any alarm or timer klaxon is ringing */
gboolean     alarm_klaxon_any_ringing (void);
/* @kind's default sound as a URI (Hassium, "Timer Expired"), or NULL if it is not installed */
char        *alarm_klaxon_default_sound (AlarmKlaxonKind kind);

/* the file of a sound found in the data dirs ("sounds/android/alarms/Hassium.ogg"), as a URI, or NULL */
char        *alarm_klaxon_find_sound (const char *relative_path);
