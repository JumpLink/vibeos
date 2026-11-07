/* TimerSetup: the keypad a new timer is typed on (DeskClock's TimerSetupView, L/timer_setup_view.xml,
 * L/timer_setup_digits.xml; spec/clock.md §7.2).
 *
 * "00h 00m 00s" over a 3 × 4 pad of round keys. Digits come in from the right, as on a calculator: 1, 3, 0 makes
 * 1m 30s. Six at most; a leading 0 does nothing; ⌫ takes the last one back and, held, clears them all. The
 * groups from the first one that is not zero show in the accent colour. The numbers are not normalised: "99m" is a
 * time like any other. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TIMER_TYPE_SETUP (timer_setup_get_type ())
G_DECLARE_FINAL_TYPE (TimerSetup, timer_setup, TIMER, SETUP, GtkWidget)

GtkWidget *timer_setup_new (void);

/* whether there is something to start (hasValidInput) */
gboolean   timer_setup_has_input (TimerSetup *self);
/* the time typed, in milliseconds */
gint64     timer_setup_get_length (TimerSetup *self);
/* back to "00h 00m 00s" */
void       timer_setup_reset (TimerSetup *self);
/* a digit or Backspace from the keyboard, as a tap on its key: TRUE when it was one (onKeyDown) */
gboolean   timer_setup_key_pressed (TimerSetup *self, guint keyval);

/* Signals: "input-changed" () when the input goes from nothing to something or back, or is cleared with a long
 * press (updateFab: the FAB shrinks and grows back, showing Start or not) */

G_END_DECLS
