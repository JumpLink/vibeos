/* navi-settings: what Navi remembers, in ~/.config/vibeos-navi/navi.ini (a key file: Navi runs from a test prefix
 * and in VibeOS's sandbox, where no GSettings schema of its own is installed).
 *
 * Home and Work are places the user saved; the last position is only the map's starting view, kept so the app
 * opens where it was. None of it leaves the phone. */
#pragma once

#include "navi-search.h"

G_BEGIN_DECLS

typedef enum {
  NAVI_DARK_AT_NIGHT, /* Waze's default: dark from sunset to sunrise where the car is */
  NAVI_DARK_ALWAYS,
  NAVI_DARK_NEVER,
  NAVI_DARK_SYSTEM,   /* as the phone's own light or dark mode */
} NaviDarkMode;

typedef enum {
  NAVI_VIEW_AUTO, /* flat while browsing, 3D while driving */
  NAVI_VIEW_3D,
  NAVI_VIEW_2D,
} NaviMapView;

typedef enum {
  NAVI_VOICE_ON,
  NAVI_VOICE_ALERTS, /* Waze's "alerts only": the road's alerts (speed cameras), no directions */
  NAVI_VOICE_OFF,
} NaviVoiceMode;

/* when the speedometer turns red: at the limit, or a margin over it */
typedef enum {
  NAVI_SPEEDING_AT_LIMIT,
  NAVI_SPEEDING_PLUS_5,   /* +5 km/h (+3 mph): the default */
  NAVI_SPEEDING_PLUS_10,  /* +10 km/h (+5 mph) */
  NAVI_SPEEDING_PERCENT_10, /* +10 % */
} NaviSpeeding;

/* what the routes are for: Valhalla's auto or motorcycle costing */
typedef enum {
  NAVI_VEHICLE_CAR,
  NAVI_VEHICLE_MOTORBIKE,
} NaviVehicle;

/* distances and speeds: the road's country's (miles in the UK and the US), or always one */
typedef enum {
  NAVI_UNITS_AUTO,
  NAVI_UNITS_KM,
  NAVI_UNITS_MI,
} NaviUnits;

typedef struct {
  NaviDarkMode dark;
  NaviMapView view;
  gboolean north_up;
  gboolean auto_zoom;
  NaviVoiceMode voice;
  char *voice_language;         /* what the voice speaks, two letters (navi_route_set_language); NULL: the system's */
  gboolean cameras;             /* speed camera alerts (and icons) where the country allows them; on by default */
  gboolean cameras_notice_seen; /* the one-time notice of where they are not allowed was shown */
  NaviSpeeding speeding;
  /* the routes: what they keep off where there is a way round (Valhalla's use_tolls, use_highways, use_ferry 0),
   * and for what */
  gboolean avoid_tolls, avoid_highways, avoid_ferries;
  NaviVehicle vehicle;
  NaviUnits units;
  NaviPlace *home, *work; /* NULL until set */
  gboolean have_last;
  double last_lat, last_lng; /* where the map opens (to a kilometre); its zoom is the browsing one, as the camera
                              * follows the car at that zoom from the first fix on */
  GPtrArray *recent;      /* NaviPlace, newest first, at most 20 */
  GPtrArray *saved;       /* NaviSaved: the places starred on their cards, in the order they were saved */
} NaviSettings;

/* a saved place: the place as it was found, and the name the user gave it */
typedef struct {
  NaviPlace *place;
  char *label; /* NULL: the place's own name */
} NaviSaved;

NaviSettings *navi_settings_load (void);
/* writes them out (a little later, so a burst of changes is one write) */
void navi_settings_save (NaviSettings *settings);
/* writes now, when the app quits */
void navi_settings_flush (NaviSettings *settings);
void navi_settings_add_recent (NaviSettings *settings, const NaviPlace *place);
/* the saved place `place` is (one within 30 m), or -1 */
int  navi_settings_find_saved   (NaviSettings *settings, const NaviPlace *place);
void navi_settings_add_saved    (NaviSettings *settings, const NaviPlace *place);
void navi_settings_remove_saved (NaviSettings *settings, guint index);
/* a new name for the saved place at `index` (NULL or empty: back to its own) */
void navi_settings_rename_saved (NaviSettings *settings, guint index, const char *label);
/* the name a saved place goes by */
const char *navi_saved_name (const NaviSaved *saved);

/* a place in a key file's `group`, as navi.ini keeps them (kind, name, detail, category, lat, lng); NULL without a
 * name */
NaviPlace *navi_settings_read_place (GKeyFile *kf, const char *group);
void navi_settings_write_place (GKeyFile *kf, const char *group, const NaviPlace *place);

G_END_DECLS
