/* navi-guidance: where the car is on its route, and what to tell the driver.
 *
 * Each position fix (from GPS or the simulated drive) is matched to the route: how far along it the car is, how far
 * off it, which manoeuvre comes next and how far away, what is left to drive. A car off the route for a few seconds
 * (or driving it backwards) is "off route", which is when the app asks for a new one. The voice prompts come at
 * Waze's distances: an alert well before a turn ("In 400 meters, turn left onto ..."), the instruction itself right
 * before it, and on long stretches how far to keep going.
 *
 * Plain logic on numbers: the clock is the fixes' own times, so the tests drive it with made-up ones. */
#pragma once

#include "navi-route.h"

G_BEGIN_DECLS

typedef struct {
  double s;            /* metres along the route */
  double offset;       /* metres from the route line */
  double bearing;      /* the route's direction at s */
  gboolean on_route;   /* close enough to it */
  gboolean off_route;  /* off it (or the wrong way) long enough: time for a new route */
  gboolean arrived;
  guint current;       /* the manoeuvre whose stretch the car is on */
  guint next;          /* the one to do next */
  double to_next;      /* metres to it */
  double remaining;    /* metres to the destination */
  double time_left;    /* seconds to the destination */
} NaviProgress;

typedef struct _NaviGuidance NaviGuidance;

NaviGuidance *navi_guidance_new (NaviRoute *route);
void navi_guidance_free (NaviGuidance *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NaviGuidance, navi_guidance_free)

NaviRoute *navi_guidance_get_route (NaviGuidance *self);

/* A fix: where, how sure (metres), how fast (m/s, < 0 unknown), which way (degrees, < 0 unknown), when
 * (microseconds, monotonic). Returns the progress, which stays valid until the next update. */
const NaviProgress *navi_guidance_update (NaviGuidance *self, double lat, double lng, double accuracy, double speed,
                                          double heading, gint64 time_us);
const NaviProgress *navi_guidance_get_progress (NaviGuidance *self);

/* The prompt due after the last update, or NULL; each is given once. `speed` is the car's, m/s. */
char *navi_guidance_take_prompt (NaviGuidance *self, double speed);

/* What is likely to be said next, so that it can be made into sound ahead (navi_voice_prepare): the next two
 * manoeuvres' instructions, their alerts at the distance this speed's alert comes at, and what follows them. A
 * NULL-terminated array, for g_strfreev (). */
char **navi_guidance_upcoming_prompts (NaviGuidance *self, double speed);

/* the alert's words before an instruction: "In 400 meters, turn left ..." in the route's language
 * (`language` as Valhalla's, "de-DE"), for the prompt and the tests */
char *navi_guidance_alert_text (const char *language, double metres, const char *instruction);

G_END_DECLS
