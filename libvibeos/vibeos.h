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
#include "vibeos-layout.h"
#include "vibeos-page-stack.h"
#include "vibeos-nav-rail.h"
#include "vibeos-list-detail.h"
#include "vibeos-fab-column.h"
#include "vibeos-list-row.h"
#include "vibeos-card-list.h"
#include "vibeos-top-bar.h"
#include "vibeos-search.h"

/* The layouts, for phones and tablets:
 *
 *   vibeos-layout.h       what the window is on: a phone or a tablet, and how wide it is
 *   vibeos-page-stack.h   pages pushed and popped, with their transitions
 *   vibeos-nav-rail.h     the navigation rail, the nav bar's place on a tablet
 *   vibeos-list-detail.h  a list and the page an item opens: side by side on a tablet, one at a time on a phone
 *   vibeos-fab-column.h   the floating action buttons, in a column beside the content or a row under it
 *   vibeos-list-row.h     a settings-style row on its card
 *   vibeos-card-list.h    rows on cards, in groups
 *   vibeos-top-bar.h      the top app bar: small, large and in selection mode
 *   vibeos-search.h       the search bar and the search view it opens */
