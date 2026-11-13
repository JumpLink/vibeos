/* The clocks the timers and the stopwatch run on, in milliseconds.
 *
 * elapsed_now() is the time since boot, counting the time the phone slept (CLOCK_BOOTTIME). A timer must run on through
 * the night, and this clock is the same in every process, so the app and the service agree on the time left without
 * telling each other. It starts again from zero at every boot, so each timer also keeps the wall clock of its last
 * start (elapsed_wall_now()): after a reboot that is the only clock left that says how long the phone was off. */
#pragma once

#include <glib.h>

gint64 elapsed_now (void);
gint64 elapsed_wall_now (void);

/* this boot's id (the kernel's boot_id); a store saved under another one needs its times updated after the reboot */
char  *elapsed_boot_id (void);
