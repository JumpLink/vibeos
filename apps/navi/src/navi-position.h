/* NaviPosition: where the car is, from GeoClue, the Location portal or a simulated drive.
 *
 * Outside a sandbox the position comes from GeoClue on the system bus; inside one (VibeOS's app sandbox or
 * Flatpak's, which have no system bus) from the XDG Location portal, which asks the user once. Either way a fix
 * comes with its accuracy, speed and heading where the source has them. A fix less accurate than 100 m (Wi-Fi or IP
 * location, no satellites) is "approximate": the app says so, as Waze does ("No GPS - showing your approximate
 * location"). Until the first fix the last known position stands in.
 *
 * The simulated drive replaces the real position for tests and demos: it drives a route at its streets' speeds
 * (Valhalla's times), slowing for the turns, and reports a fix a second, like a GPS receiver.
 *
 * Privacy: fixes are never logged; a debug build prints them to its own console. */
#pragma once

#include <gio/gio.h>

#include "navi-route.h"

G_BEGIN_DECLS

typedef enum {
  NAVI_FIX_NONE,
  NAVI_FIX_GPS,         /* a fix of 100 m or better */
  NAVI_FIX_APPROXIMATE, /* a worse one */
  NAVI_FIX_SIMULATED,
  NAVI_FIX_LAST_KNOWN,  /* no fix yet: where it was last time */
} NaviFixKind;

typedef struct {
  double lat, lng;
  double accuracy; /* metres */
  double speed;    /* m/s, < 0 unknown */
  double heading;  /* degrees, < 0 unknown */
  gint64 time_us;  /* monotonic, when it was received */
  NaviFixKind kind;
} NaviFix;

#define NAVI_TYPE_POSITION (navi_position_get_type ())
G_DECLARE_FINAL_TYPE (NaviPosition, navi_position, NAVI, POSITION, GObject)

NaviPosition *navi_position_new (void);

/* follows the real position until navi_position_stop () */
void navi_position_start (NaviPosition *self);
void navi_position_stop (NaviPosition *self);

/* the last fix (or the last known position); FALSE with none */
gboolean navi_position_get_fix (NaviPosition *self, NaviFix *fix);
/* where to stand in until the first fix (the position saved last time) */
void navi_position_set_last_known (NaviPosition *self, double lat, double lng);

/* Drives `route` (NULL: stops, and the real position comes back) at `speed_factor` x its speeds, a fix every
 * `interval_ms` (0: 1000). */
void navi_position_simulate (NaviPosition *self, NaviRoute *route, double speed_factor, guint interval_ms);
gboolean navi_position_is_simulating (NaviPosition *self);

/* Signals: "fix" (gpointer NaviFix *): a new fix. "denied" (): the user or Settings refused the position. */

G_END_DECLS
