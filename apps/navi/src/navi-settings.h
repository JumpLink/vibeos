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
  NAVI_VOICE_ALERTS, /* the turns only, no "continue for ..." */
  NAVI_VOICE_OFF,
} NaviVoiceMode;

typedef struct {
  NaviDarkMode dark;
  NaviMapView view;
  gboolean north_up;
  gboolean auto_zoom;
  NaviVoiceMode voice;
  NaviPlace *home, *work; /* NULL until set */
  gboolean have_last;
  double last_lat, last_lng, last_zoom;
  GPtrArray *recent;      /* NaviPlace, newest first, at most 20 */
} NaviSettings;

NaviSettings *navi_settings_load (void);
/* writes them out (a little later, so a burst of changes is one write) */
void navi_settings_save (NaviSettings *settings);
/* writes now, when the app quits */
void navi_settings_flush (NaviSettings *settings);
void navi_settings_add_recent (NaviSettings *settings, const NaviPlace *place);

G_END_DECLS
