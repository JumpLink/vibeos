#include "legacy-ids.h"

#include <string.h>

const LegacyId legacy_ids[] = {
  { "Dialer",        "vibeos.Dialer",        TRUE },
  { "Dialer.InCall", "vibeos.Dialer.InCall", TRUE },
  { "Dialer.Calls",  "vibeos.Dialer.Calls",  TRUE },
  { "Messaging",     "vibeos.Messaging",     TRUE },
  { "Messaging.Sms", "vibeos.Messaging.Sms", TRUE },
  { "Clock",         "vibeos.Clock",         TRUE },
  { "Clock.Ringing", "vibeos.Clock.Ringing", TRUE },
  { "Clock.Alarms",  "vibeos.Clock.Alarms",  TRUE },
  { "Calendar",      "vibeos.Calendar",      TRUE },
  { "Calendar.Reminders", "vibeos.Calendar.Reminders", TRUE },
  { "Contacts",      "vibeos.Contacts",      TRUE },
  { "Calculator",    "vibeos.Calculator",    TRUE },
  { "Camera",        "vibeos.Camera",        TRUE },
  { "Files",         "vibeos.Files",         TRUE },
  { "Compass",       "vibeos.Compass",       TRUE },
  { "NeoBrowser",    "vibeos.Browser",       TRUE },
  /* the GJS browser's id, which NeoBrowser replaced (0.1.3): its desktop id still maps, its old files stay put */
  { "Browser",       "vibeos.Browser",       FALSE },
  { "NeoWeather",    "vibeos.Weather",       TRUE },
  { "NeoSettings",   "vibeos.Settings",      TRUE },
  { "NeoLauncher",   "vibeos.Launcher",      FALSE },
  /* a ROM's "boot into Android" entry; it maps only once the ROM ships it under the new name */
  { "BootAndroid",   "vibeos.BootAndroid",   FALSE },
  /* from 0.1.9 only: no first-generation name */
  { NULL,            "vibeos.Dictate",       TRUE },
};
const guint legacy_n_ids = G_N_ELEMENTS (legacy_ids);

/* every schema that kept settings under LEGACY_PATH; "neo" was the phone's shared one (the launcher's layout, the
 * lock screen, the AOD), vibeos.Shell now */
const LegacyPath legacy_paths[] = {
  { "neo/",        "/vibeos/Shell/" },
  { "Calculator/", "/vibeos/Calculator/" },
  { "Calendar/",   "/vibeos/Calendar/" },
  { "Camera/",     "/vibeos/Camera/" },
  { "Clock/",      "/vibeos/Clock/" },
  { "Contacts/",   "/vibeos/Contacts/" },
  { "Dialer/",     "/vibeos/Dialer/" },
  { "Files/",      "/vibeos/Files/" },
  { "Messaging/",  "/vibeos/Messaging/" },
};
const guint legacy_n_paths = G_N_ELEMENTS (legacy_paths);

/* the same schemas from 0.1.7 to 0.1.11 (schema id = path), and Dictate's from 0.1.9 */
const LegacyPath neo_paths[] = {
  { "/neo/Phone/",      "/vibeos/Shell/" },
  { "/neo/Calculator/", "/vibeos/Calculator/" },
  { "/neo/Calendar/",   "/vibeos/Calendar/" },
  { "/neo/Camera/",     "/vibeos/Camera/" },
  { "/neo/Clock/",      "/vibeos/Clock/" },
  { "/neo/Contacts/",   "/vibeos/Contacts/" },
  { "/neo/Dialer/",     "/vibeos/Dialer/" },
  { "/neo/Dictate/",    "/vibeos/Dictate/" },
  { "/neo/Files/",      "/vibeos/Files/" },
  { "/neo/Messaging/",  "/vibeos/Messaging/" },
};
const guint neo_n_paths = G_N_ELEMENTS (neo_paths);

/* directories named after a program: the launcher's layout, cache and parallax sets, Files' bookmarks and remotes,
 * the calendar's reminders, the dialer's blocked numbers, Dictate's models and document, the extension's lock
 * screen and rotation state */
const LegacyPath neo_dirs[] = {
  { "neo-launcher",  "vibeos-launcher" },
  { "neo-calendar",  "vibeos-calendar" },
  { "neo-camera",    "vibeos-camera" },
  { "neo-dialer",    "vibeos-dialer" },
  { "neo-dictate",   "vibeos-dictate" },
  { "neo-files",     "vibeos-files" },
  { "neo-messaging", "vibeos-messaging" },
  { "neo-phone",     "vibeos-phone" },
  { "neo-settings",  "vibeos-settings" },
  { NEO_STATE_DIR,   "vibeos" },
};
const guint neo_n_dirs = G_N_ELEMENTS (neo_dirs);

/* "vibeos.Clock" -> "Clock": the part both generations of neo.* and vibeos.* ids share */
static const char *
suffix (const char *now)
{
  return now + strlen (APP_PREFIX);
}

char *
legacy_app_id (const char *id)
{
  if (!id)
    return NULL;
  if (g_str_has_prefix (id, LEGACY_PREFIX)) {
    const char *rest = id + strlen (LEGACY_PREFIX);
    for (guint i = 0; i < legacy_n_ids; i++)
      if (legacy_ids[i].old && !strcmp (rest, legacy_ids[i].old))
        return g_strdup (legacy_ids[i].now);
    return NULL;
  }
  if (g_str_has_prefix (id, NEO_PREFIX)) {
    const char *rest = id + strlen (NEO_PREFIX);
    for (guint i = 0; i < legacy_n_ids; i++)
      if (!strcmp (rest, suffix (legacy_ids[i].now)))
        return g_strdup (legacy_ids[i].now);
  }
  return NULL;
}

char *
legacy_desktop_id (const char *id)
{
  if (!id || !g_str_has_suffix (id, ".desktop"))
    return NULL;
  if (g_str_has_prefix (id, LEGACY_SHORTCUT_PREFIX))
    return g_strconcat (SHORTCUT_PREFIX, id + strlen (LEGACY_SHORTCUT_PREFIX), NULL);
  if (g_str_has_prefix (id, NEO_SHORTCUT_PREFIX))
    return g_strconcat (SHORTCUT_PREFIX, id + strlen (NEO_SHORTCUT_PREFIX), NULL);
  if (!g_str_has_prefix (id, LEGACY_PREFIX) && !g_str_has_prefix (id, NEO_PREFIX))
    return NULL;
  g_autofree char *app = g_strndup (id, strlen (id) - strlen (".desktop"));
  g_autofree char *now = legacy_app_id (app);
  return now ? g_strconcat (now, ".desktop", NULL) : NULL;
}

char *
legacy_stamp_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "vibeos", "renamed", NULL);
}
