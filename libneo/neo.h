/* libneo: the building blocks of the phone's own apps, drawn and moving the way their Android originals do.
 *
 *   neo-style.h        the palette, the font and stylesheet, the window with its overlay (neo_init first)
 *   neo-anim.h         one value animated on the frame clock (ValueAnimator)
 *   neo-ripple.h       the touch ripple, for widgets that draw their own
 *   neo-pressable.h    a tappable child with a ripple: rows, text and icon buttons, FABs
 *   neo-switch.h       the Material 3 switch with thumb icons
 *   neo-nav-bar.h      the bottom navigation bar
 *   neo-snackbar.h     the snackbar
 *   neo-dialog.h       the alert dialog, inside the window
 *   neo-time-picker.h  the clock-face time picker and its dialog
 *
 * Sizes are dp: on the phone (scale 3) one GTK pixel is one dp. Every value comes from the Android source or a
 * measurement of the reference phone, and the comments say which. */
#pragma once

#include "neo-style.h"
#include "neo-anim.h"
#include "neo-ripple.h"
#include "neo-pressable.h"
#include "neo-switch.h"
#include "neo-nav-bar.h"
#include "neo-snackbar.h"
#include "neo-dialog.h"
#include "neo-time-picker.h"
