/* navi-alerts: what the road says while driving: its speed limit, for the sign by the speedometer, and the speed
 * cameras ahead, for the approach alert (a card and, unless the sound is off, a word).
 *
 * On a route both come for the whole route at once when it starts (and each new route): the limits and countries
 * from Valhalla's trace_attributes (navi-roadinfo.h), the cameras from the overlay tiles (navi-cameras.h). Off a
 * route (browsing while moving, or off the route while a new one comes) the limit is the road's where the last half
 * minute of fixes ends, map-matched every 12 s. A limit nobody knows shows no sign.
 *
 * The map's camera and signal icons are navi-window.c's (navi_window_update_overlay): this only says the country. */
#pragma once

#include "navi-app.h"
#include "navi-position.h"
#include "navi-guidance.h"

G_BEGIN_DECLS

typedef struct _NaviAlerts NaviAlerts;

NaviAlerts *navi_alerts_new (Navi *navi);
void navi_alerts_free (NaviAlerts *alerts);

/* a route to drive from now on (NULL: none, the drive is over) */
void navi_alerts_set_route (NaviAlerts *alerts, NaviRoute *route);
/* each fix; `progress` along the route while driving one, else NULL */
void navi_alerts_fix (NaviAlerts *alerts, const NaviFix *fix, const NaviProgress *progress);

/* the country the car is in ("DE"), "" while not known */
const char *navi_alerts_country (NaviAlerts *alerts);

G_END_DECLS
