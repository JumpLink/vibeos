#include "legacy-ids.h"

#include <string.h>

const LegacyId legacy_ids[] = {
  { "Dialer",        "neo.Dialer",        TRUE },
  { "Dialer.InCall", "neo.Dialer.InCall", TRUE },
  { "Dialer.Calls",  "neo.Dialer.Calls",  TRUE },
  { "Messaging",     "neo.Messaging",     TRUE },
  { "Messaging.Sms", "neo.Messaging.Sms", TRUE },
  { "Clock",         "neo.Clock",         TRUE },
  { "Clock.Ringing", "neo.Clock.Ringing", TRUE },
  { "Clock.Alarms",  "neo.Clock.Alarms",  TRUE },
  { "Calendar",      "neo.Calendar",      TRUE },
  { "Calendar.Reminders", "neo.Calendar.Reminders", TRUE },
  { "Contacts",      "neo.Contacts",      TRUE },
  { "Calculator",    "neo.Calculator",    TRUE },
  { "Camera",        "neo.Camera",        TRUE },
  { "Files",         "neo.Files",         TRUE },
  { "Compass",       "neo.Compass",       TRUE },
  { "NeoBrowser",    "neo.Browser",       TRUE },
  /* the GJS browser's id, which NeoBrowser replaced (0.1.3): its desktop id still maps, its old files stay put */
  { "Browser",       "neo.Browser",       FALSE },
  { "NeoWeather",    "neo.Weather",       TRUE },
  { "NeoSettings",   "neo.Settings",      TRUE },
  { "NeoLauncher",   "neo.Launcher",      FALSE },
  /* a ROM's "boot into Android" entry; it maps only once the ROM ships it under the new name */
  { "BootAndroid",   "neo.BootAndroid",   FALSE },
};
const guint legacy_n_ids = G_N_ELEMENTS (legacy_ids);

/* every schema that kept settings under LEGACY_PATH; "neo" was the phone's shared one (the launcher's layout, the
 * lock screen, the AOD), neo.Phone now */
const LegacyPath legacy_paths[] = {
  { "neo/",        "/neo/Phone/" },
  { "Calculator/", "/neo/Calculator/" },
  { "Calendar/",   "/neo/Calendar/" },
  { "Camera/",     "/neo/Camera/" },
  { "Clock/",      "/neo/Clock/" },
  { "Contacts/",   "/neo/Contacts/" },
  { "Dialer/",     "/neo/Dialer/" },
  { "Files/",      "/neo/Files/" },
  { "Messaging/",  "/neo/Messaging/" },
};
const guint legacy_n_paths = G_N_ELEMENTS (legacy_paths);

char *
legacy_app_id (const char *id)
{
  if (!id || !g_str_has_prefix (id, LEGACY_PREFIX))
    return NULL;
  const char *rest = id + strlen (LEGACY_PREFIX);
  for (guint i = 0; i < legacy_n_ids; i++)
    if (!strcmp (rest, legacy_ids[i].old))
      return g_strdup (legacy_ids[i].now);
  return NULL;
}

char *
legacy_desktop_id (const char *id)
{
  if (!id || !g_str_has_prefix (id, LEGACY_PREFIX) || !g_str_has_suffix (id, ".desktop"))
    return NULL;
  if (g_str_has_prefix (id, LEGACY_SHORTCUT_PREFIX))
    return g_strconcat (SHORTCUT_PREFIX, id + strlen (LEGACY_SHORTCUT_PREFIX), NULL);
  g_autofree char *app = g_strndup (id, strlen (id) - strlen (".desktop"));
  g_autofree char *now = legacy_app_id (app);
  return now ? g_strconcat (now, ".desktop", NULL) : NULL;
}

char *
legacy_stamp_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "gnome-phone", "ids-migrated", NULL);
}
