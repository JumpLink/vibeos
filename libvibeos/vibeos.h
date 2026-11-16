/* libvibeos: the building blocks of the phone's own apps: Material 3 widgets on GTK 4, with their motion.
 *
 *   vibeos-style.h        the palette, the font and stylesheet, the window with its overlay (vibeos_init first)
 *   vibeos-anim.h         one value animated on the frame clock
 *   vibeos-ripple.h       the touch ripple, for widgets that draw their own
 *   vibeos-pressable.h    a tappable child with a ripple: rows, text and icon buttons, FABs
 *   vibeos-switch.h       the Material 3 switch with thumb icons
 *   vibeos-nav-bar.h      the bottom navigation bar
 *   vibeos-snackbar.h     the snackbar
 *   vibeos-dialog.h       the alert dialog, inside the window
 *   vibeos-time-picker.h  the clock-face time picker and its dialog
 *
 * Sizes are dp: on the phone (scale 3) one GTK pixel is one dp. */
#pragma once

#include "vibeos-style.h"
#include "vibeos-anim.h"
#include "vibeos-ripple.h"
#include "vibeos-pressable.h"
#include "vibeos-switch.h"
#include "vibeos-nav-bar.h"
#include "vibeos-snackbar.h"
#include "vibeos-dialog.h"
#include "vibeos-time-picker.h"
