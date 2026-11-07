/* TimerPage: the Timer tab (DeskClock's TimerFragment, L/timer_fragment.xml; spec/clock.md §7.1–7.3).
 *
 * Either the keypad for a new timer (timer-setup.h) or the timers as cards, the newest first (timer-card.h). It
 * opens on the keypad when there are no timers and on the list when there are; "+" goes to the keypad, Start and
 * Cancel come back. Each change slides both views by the height of a card's ring and cross-fades them, while the
 * buttons under them shrink away and come back as the new view's. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TIMER_TYPE_PAGE (timer_page_get_type ())
G_DECLARE_FINAL_TYPE (TimerPage, timer_page, TIMER, PAGE, GtkWidget)

GtkWidget *timer_page_new (void);
/* the list, at once (a tap on a timer's notification) */
void       timer_page_show_timer (TimerPage *self, guint id);

G_END_DECLS
