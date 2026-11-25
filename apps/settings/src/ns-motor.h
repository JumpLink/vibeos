/* Whether the device has a vibration motor, and so what the shell's keyboard and volume sounds default to.
 * extension/sounds.js has the same rule in JavaScript (hasMotor): keep them in step. */
#pragma once

#include <glib.h>

/* The motor, by any of three signs, in the trees given (NULL for the system's own: /sys/class/input, /sys/class/leds
 * and /run/udev/data):
 *  - an input event node that plays force feedback (a non-zero "ff" capability, which feedbackd's own rules tag as a
 *    vibra) and is not a game controller (one with absolute axes rumbles too, and comes and goes);
 *  - an LED class device named for a vibrator (some phones drive the motor as the LED "vibrator");
 *  - an input device in udev's database that feedbackd's rules tagged FEEDBACKD_TYPE=vibra. */
gboolean ns_motor_present_in (const char *input_dir, const char *leds_dir, const char *udev_dir);

/* the system's, read once: a motor does not come and go */
gboolean ns_motor_present (void);
