/* TimerCard: one timer as a card (DeskClock's TimerItem, L/timer_item.xml; spec/clock.md §7.4).
 *
 * The label at the top with the close button beside it; under it the time left in a ring that empties as it runs,
 * with a reset button at the ring's foot; on the right "+ 1:00" over play/pause. A timer that ran out shows its
 * overtime ("−0:12") with a stop button, and its ring blinks.
 *
 * The card only shows a timer and passes its buttons on to the timers client; the list (timer-page.c) or the
 * expired-timers window updates it as time goes by. */
#pragma once

#include "timer-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define TIMER_TYPE_CARD (timer_card_get_type ())
G_DECLARE_FINAL_TYPE (TimerCard, timer_card, TIMER, CARD, GtkWidget)

/* `ringing`: the card on the expired-timers screen (ExpiredTimersActivity.addTimer), with no close or reset button,
 * no label unless the timer has one, and Stop leaving the screen */
GtkWidget *timer_card_new (guint id, gboolean ringing);
guint      timer_card_get_id (TimerCard *self);

/* TimerItem.update: the time left at `now` (the elapsed clock), the ring, the label and the buttons for the
 * timer's state */
void       timer_card_update (TimerCard *self, const Timer *timer, gint64 now);

/* the height of the ring's box in a card `width` wide: how far the Timer tab slides when it changes view
 * (TimerFragment.animateToView: the timer_time view's height plus its y, which is 0 in its frame) */
double     timer_card_get_ring_height (TimerCard *self, int width);

G_END_DECLS
