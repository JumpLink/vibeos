/* navi-awake: what of Navi runs, by whether a route is being driven and whether its window is seen.
 *
 * The owner, 2026-11-27: "probably anything on background should use less cpu and not drain battery, navigation
 * should work if there's destination set, if not - can just go to sleep". (A Navi left minimized after a drive kept
 * its GeoClue client, and the phone's GNSS with it, for hours.) So:
 *
 *                    seen                     not seen (minimized, covered,          closed
 *                                             the screen locked or off)
 *   driving a route  everything               the position, the guidance and the     nothing (closing ends the
 *                                             voice; nothing drawn, no compass       drive, and Navi quits)
 *   no route         everything               nothing: no GeoClue client, no         nothing
 *                                             compass, no timers, no frames
 *
 * "Seen" is the window mapped, not suspended (the shell suspends a window it hides) and in front (active): Recents,
 * the shade and the lock screen take the focus. A window that has never been active (a seat with no keyboard) counts
 * as in front while shown. While a route is driven the phone may not sleep either (navi-inhibit.h), seen or not.
 *
 * NaviAwake does it: it starts and stops the position's real source, keeps the app's repeating timers, holds the
 * sleep inhibitor, and tells the window when to draw, to use the compass and to keep the screen on. */
#pragma once

#include "navi-back.h"
#include "navi-position.h"

G_BEGIN_DECLS

typedef enum {
  NAVI_SEEN,
  NAVI_UNSEEN,
  NAVI_CLOSED,
} NaviSeen;

typedef struct {
  gboolean position;    /* the real position (GeoClue's client, or the portal's session) */
  gboolean compass;     /* the compass and the accelerometer may be claimed (navi_compass_wanted says when) */
  gboolean drawing;     /* the frame loop, the map's frames, the car and what floats over the map */
  gboolean timers;      /* the app's repeating timers (navi_awake_add_timer) */
  gboolean screen_on;   /* the screen kept on (driving, or arrived, while seen) */
  gboolean sleep_block; /* the phone kept from sleeping: a route is being driven */
} NaviRun;

/* what runs in `mode` while the window is `seen` */
NaviRun navi_run_for (NaviMode mode, NaviSeen seen);
/* whether a window is seen: `open` (not closed), `mapped`, `suspended` by the shell, `active` (in front), and
 * whether it has `ever_active` been */
NaviSeen navi_seen_from (gboolean open, gboolean mapped, gboolean suspended, gboolean active, gboolean ever_active);

typedef struct _NaviAwake NaviAwake;

/* the window's part: each called with what it should do now, when that changes (and once at the first update) */
typedef struct {
  void (*drawing) (gboolean on, gpointer data);
  void (*compass) (gboolean on, gpointer data);
  void (*screen_on) (gboolean on, gpointer data);
} NaviAwakeHooks;

/* `position`: the one whose real source it starts and stops; NULL when there is none to follow (a test drive's
 * --from: the simulated position only) */
NaviAwake *navi_awake_new (NaviPosition *position, const NaviAwakeHooks *hooks, gpointer data);
/* everything stopped, the inhibitor let go */
void navi_awake_free (NaviAwake *self);

/* the mode or the window changed */
void navi_awake_update (NaviAwake *self, NaviMode mode, NaviSeen seen);
NaviRun navi_awake_get (NaviAwake *self);
NaviSeen navi_awake_get_seen (NaviAwake *self);

/* `fn` every `interval_ms` while the timers run (a whole number of seconds: on GLib's second timers, which the
 * system groups with others); returning G_SOURCE_REMOVE drops it */
void navi_awake_add_timer (NaviAwake *self, guint interval_ms, GSourceFunc fn, gpointer data);
/* how many of its timers are on the main loop now */
guint navi_awake_timers_attached (NaviAwake *self);
/* whether the sleep inhibitor is held now */
gboolean navi_awake_sleep_blocked (NaviAwake *self);

G_END_DECLS
