/* The ringing alarm, full screen (DeskClock's AlarmActivity, L/alarm_activity.xml; spec/clock.md §4.3–4.5).
 *
 * The time, the alarm's label, and a pulsing alarm handle between Snooze (left) and Dismiss (right). Dragging the
 * handle onto one of them snoozes or dismisses the alarm; a grey circle then grows from that button and leaves
 * "Snoozed / 10 min" or "Alarm off" for two seconds before the window closes. Tapping the handle or a button only
 * shows which way to swipe.
 *
 * The alarm service starts it (as the app neo.Clock.Ringing) when the phone was asleep or locked; over the
 * lock screen the shell shows it because its desktop file says X-GnomePhone-ShowWhenLocked. It keeps the screen on,
 * Back does not close it, and it closes by itself when the alarm stops ringing some other way (the notification,
 * the auto-silence timeout). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the window for the alarm ringing now, or NULL when none is */
GtkWidget *ringing_window_new (GtkApplication *app, GSettings *settings);
/* a volume key the shell passed on: snoozes or dismisses the alarm if the "Volume buttons" setting says so and the
 * window is in front; TRUE when the window took it */
gboolean   ringing_window_volume_key (GtkWidget *window);

G_END_DECLS
