/* navi-back: what Back does in Navi, decided without GTK (navi-main.c does it).
 *
 * Back comes as Escape, the Back key or Alt+Left, or from VibeOS's back swipe. The swipe asks the app what Back means
 * over the accessibility bus (extension/a11y.js). It presses a Back button showing at the window's top, or sends
 * Escape over a dialog. With neither, it takes the app home: minimized, still running, in recents. Navi's unseen
 * one-pixel Back button says "there is somewhere to go back to". It shows in every mode but browsing
 * (navi_back_button_shown), so browsing is the one mode a swipe leaves.
 *
 *   browsing         the shell takes Navi home (a key: the car again, if the map was moved off it)
 *   a place's card   it closes: browsing
 *   the routes       back to the destination's card, as the routes' own Back does; the countdown to Go stops
 *   driving          "End navigation?": Cancel (the drive goes on) or End navigation (it stops, Navi stays on the map)
 *   arrived          the drive ends: browsing
 *
 * Over all of these, the search closes first, and a dialog showing is answered Cancel. A second Back over
 * "End navigation?" keeps the drive. Back never quits Navi. Closing it from recents is not Back: that is the window
 * asked to close (a close request), which Navi does not refuse. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the window's modes (navi-app.h has what each shows) */
typedef enum {
  NAVI_MODE_BROWSE,
  NAVI_MODE_PLACE,
  NAVI_MODE_ROUTES,
  NAVI_MODE_DRIVE,
  NAVI_MODE_ARRIVED,
} NaviMode;

typedef enum {
  NAVI_BACK_NOTHING,       /* nothing to go back from */
  NAVI_BACK_RECENTER,      /* browsing with the map moved off the car: the car again */
  NAVI_BACK_CANCEL_DIALOG, /* a dialog over the map ("End navigation?" among them) is answered Cancel */
  NAVI_BACK_CLOSE_SEARCH,  /* the search over the map closes */
  NAVI_BACK_CLOSE_PLACE,   /* the place's card closes: browsing */
  NAVI_BACK_TO_PLACE,      /* the routes' preview goes back to the destination's card */
  NAVI_BACK_ASK_END,       /* driving: "End navigation?" */
  NAVI_BACK_END_DRIVE,     /* the drive stops; Navi stays on the map, browsing */
} NaviBack;

typedef struct {
  NaviMode mode;
  gboolean dialog;         /* a dialog shows over the map */
  gboolean search;         /* the search shows over the map */
  gboolean off_car;        /* the camera is not on the car, and where the car is is known */
} NaviBackState;

NaviBack navi_back_decide (const NaviBackState *state);
/* "End navigation?" answered in `mode` (the mode at the time of the answer): NAVI_BACK_END_DRIVE when End navigation
 * was chosen (`end`) while the drive is still on, else NAVI_BACK_NOTHING. Cancel, a tap outside and Back all
 * keep the drive. */
NaviBack navi_back_end_answered (NaviMode mode, gboolean end);
/* whether the unseen Back the shell's swipe presses shows in `mode`: in every mode but browsing */
gboolean navi_back_button_shown (NaviMode mode);
/* the action's name, for logs and tests */
const char *navi_back_name (NaviBack back);

G_END_DECLS
