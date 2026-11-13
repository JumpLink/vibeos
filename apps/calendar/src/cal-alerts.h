/* When reminders are due, as pure rules over the occurrences the store reads. The reminder service (reminders-main.c)
 * keeps the state and does the notifying; this decides what and when, so the tests can check it.
 *
 * A reminder of an occurrence is due at its start less its minutes. One that came due while nothing ran (the phone
 * was off, the service was not running) still notifies while its event has not ended;
 * after the end it is dropped. Only reminders that notify count: Notification (VALARM DISPLAY) and Alarm (AUDIO);
 * an Email one is the server's to send. */
#pragma once

#include "cal-model.h"

G_BEGIN_DECLS

/* the key of one occurrence (its calendar, UID, RECURRENCE-ID and start), and of one of its reminders: short
 * strings, safe as GKeyFile keys */
char    *cal_alert_occurrence_key (const CalEvent *event);
char    *cal_alert_key            (const CalEvent *event, int minutes);

/* the reminders of @event that notify */
gboolean cal_alert_notifies (const CalReminder *reminder);

typedef struct {
  CalEvent *event;              /* a reference */
  int minutes;
  gint64 time;                  /* when it is due, Unix seconds */
  GPtrArray *keys;              /* the keys of every due reminder of the occurrence: all are notified by this one */
} CalAlert;

void cal_alert_free (CalAlert *alert);

/* a declined event never notifies; with Skip Reminders "not_responded" ("If declined or not responded") neither does
 * one not answered (the owner a guest who has said nothing, or none at all). An event of the owner's own (no guests, no
 * organizer) counts as accepted. */
gboolean cal_alert_skipped (const CalEvent *event, const char *skip_reminders);

/* Of @events (CalEvent), the reminders due at @now that are not in @fired (keys from cal_alert_key) and whose
 * event has not ended, into @due (CalAlert, latest per occurrence only: one notification per occurrence); the
 * earliest reminder after @now in @next (0 if none). */
void cal_alerts_compute (GPtrArray *events, GHashTable *fired, gint64 now, GPtrArray *due, gint64 *next);

/* --- the notification's buttons */

/* the first map link ("geo:0,0?q=…")
 * and the first phone link ("tel:+4930…") in an event's location, each NULL when there is none. @region is the
 * phone's region ("DE", "US"; "US" when unknown). Outside the US a location that is one link
 * from end to end is that link (a phone number gives tel:, a web or mail address neither), and anything else is
 * looked up on the map as a whole. In the US, web and mail addresses, coordinates and North American numbers are
 * found within the text, and a location with none of them is looked up on the map. */
void cal_alert_location_links (const char *location, const char *region, char **geo, char **tel);

/* the region the links are read for: the territory of the locale's LC_TELEPHONE (de_DE -> "DE"), else "US" */
char *cal_alert_region (void);

typedef enum {
  CAL_ALERT_ACTION_MAP,
  CAL_ALERT_ACTION_CALL,
  CAL_ALERT_ACTION_EMAIL,
  CAL_ALERT_ACTION_SNOOZE,
} CalAlertAction;

#define CAL_ALERT_MAX_ACTIONS 3         /* the most buttons a notification gets */

/* the buttons in this order, Map, Call, Email guests, Snooze, those that apply, at most three: with all four,
 * Snooze is the one left off. Returns how many went into @actions. */
int cal_alert_actions (gboolean map, gboolean call, gboolean email, gboolean snooze,
                       CalAlertAction actions[CAL_ALERT_MAX_ACTIONS]);

/* the reminders page's "when": an all-day event "Sunday, November 8" (a range when it lasts days), a timed one
 * "November 8, 10:00 – 11:00" ("November 8, 22:00 – November 9, 01:00" over midnight); the year when it is not @now's;
 * a timed one in the home time zone gets the zone's short name after it */
char *cal_alert_when (gint64 start, gint64 end, gboolean all_day, gint64 now);

/* --- how a reminder sounds and vibrates */

/* the phone can vibrate: an input device that plays force feedback (feedbackd, which vibrates, cannot say whether
 * there is anything to vibrate). The Vibrate row and the service's vibration need it. */
gboolean cal_alerts_can_vibrate (void);

typedef struct {
  char *name;                   /* "Message new instant" */
  char *path;                   /* the file */
} CalSound;

void       cal_sound_free   (CalSound *sound);
/* The sounds a reminder can play, as the ringtone picker lists the phone's notification sounds: the files of the
 * installed sound themes' stereo directories ($XDG_DATA_DIRS/sounds/THEME/stereo), by name, each name once */
GPtrArray *cal_alerts_sounds (void);
/* the name of the sound a "reminder-sound" value plays: "default" the theme's message-new-instant, "" none */
char      *cal_alerts_sound_label (const char *value);

G_END_DECLS
