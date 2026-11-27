/* navi-inhibit: logind's sleep inhibitor, in "block" mode, while a route is being driven.
 *
 * A phone that suspends in the middle of a drive stops the guidance, the voice and the GPS with it. So while a route
 * is driven (and only then: arrived, ended, cancelled or quit, the phone may sleep again) Navi holds the inhibitor
 * logind hands out (org.freedesktop.login1.Manager.Inhibit "sleep", "VibeOS Navi", "Guiding to a destination",
 * "block"): a file descriptor, released by closing it. Before 0.1.6 the GNSS daemon held a keep-awake while the
 * screen was off; it no longer does, so a phone without a route sleeps. The screen's own idle (the screen staying
 * on) is a separate thing: gtk_application_inhibit, in navi-window.c. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NaviSleepBlock NaviSleepBlock;

NaviSleepBlock *navi_sleep_block_new (void);
/* holds the inhibitor, or lets it go; asked of logind on the system bus, so it is held once logind answers */
void navi_sleep_block_set (NaviSleepBlock *self, gboolean held);
/* whether the inhibitor's file descriptor is held now */
gboolean navi_sleep_block_is_held (NaviSleepBlock *self);
/* lets it go, and frees it (an answer still to come is closed when it comes) */
void navi_sleep_block_free (NaviSleepBlock *self);

G_END_DECLS
