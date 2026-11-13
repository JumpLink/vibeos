/* The timers that ran out, full screen.
 *
 * The ringing timers as cards (timer-card.h, without the close and reset buttons, the label only if there is one): a
 * single one in the middle of the screen, more from the top. Each card's Stop resets its timer and "+ 1:00" sets it
 * running again; either takes it off the screen, and the window goes when no timer rings. A volume key stops them
 * all.
 *
 * Like the ringing alarm (ringing-window.h) it belongs to the Ringing app, which the timer service has the shell
 * bring up, over the lock screen too, when a timer runs out. It keeps the screen on. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The window, once the timers are known (the application is held until then): opened if a timer rings and it is not
 * open, raised if a timer started ringing since it last was. From then on, as long as the application runs, every
 * timer that starts ringing opens or raises it the same way. */
void expired_window_show (GtkApplication *app);

/* every ringing timer stopped: what a volume key does on the screen */
void expired_window_stop_all (void);
/* a volume key the shell passed on while the window was in front: the same, if the window is still there */
void expired_window_volume_key (void);

G_END_DECLS
