/* CalReminders: an event's reminders as rows to change, the editor's and the event info's: per reminder a spinner of
 * how long before ("10 minutes" … "4 weeks") and a remove button; "Add reminder" adds one.
 *
 * There is no second spinner for the method (Notification, Email, SMS, Alarm): the phone's reminder service
 * (neo-calendar-reminders) notifies for every reminder and sends no e-mail or SMS, so offering those would claim what
 * nothing does. A reminder read from the calendar keeps the method it has. */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_MAX_REMINDERS 5             /* the most reminders an event of a local calendar has */

#define CAL_TYPE_REMINDERS (cal_reminders_get_type ())
G_DECLARE_FINAL_TYPE (CalReminders, cal_reminders, CAL, REMINDERS, GtkWidget)

GtkWidget *cal_reminders_new   (void);
void       cal_reminders_set   (CalReminders *self, GArray *reminders);
/* a new GArray of CalReminder, in the rows' order */
GArray    *cal_reminders_get   (CalReminders *self);
void       cal_reminders_add   (CalReminders *self, int minutes);
guint      cal_reminders_count (CalReminders *self);
/* Signals: "changed" () a row was added, removed or set */

G_END_DECLS
