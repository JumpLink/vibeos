/* AlarmCard: one alarm in the Alarm tab, collapsed or expanded (DeskClock's alarm_time_collapsed.xml,
 * alarm_time_expanded.xml and their view holders; spec/clock.md §2.2-2.7).
 *
 * Collapsed: the label (if any), the time, the repeat days and the on/off switch. Expanded, in addition: the label
 * row (with "Add label"), the seven day buttons, the sound, Vibrate and Delete. Between the two the card's bounds
 * move over 150 ms (fast-out-slow-in) and the extra rows fade in or out in a short stagger.
 *
 * The card makes its changes through the alarm service itself. It asks its page for what is not its own:
 * "toggle-expanded" (a tap on the card or its arrow; the page keeps one card open), "message" (a snackbar text)
 * and "deleted" (the alarm as it was, for the page's undo). */
#pragma once

#include <gtk/gtk.h>

#include "alarms-client.h"

G_BEGIN_DECLS

#define ALARM_TYPE_CARD (alarm_card_get_type ())
G_DECLARE_FINAL_TYPE (AlarmCard, alarm_card, ALARM, CARD, GtkWidget)

GtkWidget *alarm_card_new          (const ClockAlarm *alarm, GSettings *settings);
/* the service's new view of the alarm */
void       alarm_card_update       (AlarmCard *self, const ClockAlarm *alarm);
guint      alarm_card_get_id       (AlarmCard *self);
const ClockAlarm *alarm_card_get_alarm (AlarmCard *self);

void       alarm_card_set_expanded (AlarmCard *self, gboolean expanded, gboolean animate);
gboolean   alarm_card_get_expanded (AlarmCard *self);

/* the list's item animations (ItemAnimator): a new card opens its space (150 ms) then fades in (120 ms); a
 * removed one fades out (120 ms) then closes its space (150 ms) and calls `gone` */
void       alarm_card_appear       (AlarmCard *self);
void       alarm_card_disappear    (AlarmCard *self, GCallback gone, gpointer data);

/* Signals: "toggle-expanded" (), "message" (const char *text), "deleted" (gpointer ClockAlarm), "pick-ringtone" ()
 * (the ringtone row was tapped) */

G_END_DECLS
