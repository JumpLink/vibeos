/* The names VibeOS had before, and what each is called now (vibeos.*). vibeos-migrate-ids carries a user's
 * settings and files across with these tables, and the launcher reads old desktop ids in layouts and in
 * hidden-from-home with them.
 *
 * Two generations of old names:
 *  - up to 0.1.6 of gnome-phone (the project VibeOS was called before), every ID began with the reverse-DNS prefix
 *    of the project gnome-phone came out of. That name is not to appear in this tree again: this is the one place
 *    that has to know it, so it is put together from pieces here, and it is only ever compared with what an older
 *    install left behind;
 *  - gnome-phone 0.1.7 to 0.1.11 named its IDs neo.* ("neo.Clock"), its programs and their files neo-* and
 *    gnome-phone, and its extension gnome-phone@neo. */
#pragma once

#include <glib.h>

#define LEGACY_VENDOR "yes" "man"
#define LEGACY_PREFIX "de." LEGACY_VENDOR "."   /* app, desktop, D-Bus and schema ids up to 0.1.6 */
#define LEGACY_PATH   "/de/" LEGACY_VENDOR "/"  /* GSettings paths up to 0.1.6 */
#define LEGACY_EXTENSION "gnome-phone@" LEGACY_VENDOR ".de"

#define NEO_PREFIX    "neo."                    /* app, desktop, D-Bus and schema ids from 0.1.7 to 0.1.11 */
#define NEO_EXTENSION "gnome-phone@neo"
/* $XDG_STATE_HOME/<NEO_STATE_DIR>/ids-migrated: the stamp gnome-phone 0.1.7 left once a user was carried over from
 * the first generation */
#define NEO_STATE_DIR "gnome-phone"

#define APP_PREFIX     "vibeos."
#define EXTENSION_UUID "shell@vibeos"

typedef struct {
  const char *old;   /* the id without LEGACY_PREFIX, up to 0.1.6 ("Clock" for the clock) */
  const char *now;   /* the id today ("vibeos.Clock"); from 0.1.7 to 0.1.11 it was the same with NEO_PREFIX */
  gboolean    data;  /* its per-user directories ($XDG_*_HOME/<id>) move with it */
} LegacyId;

extern const LegacyId legacy_ids[];
extern const guint    legacy_n_ids;

/* GSettings paths: the old directory and the new one. For the first generation the old one is under LEGACY_PATH
 * ("Clock/"), for the second it is the whole path ("/neo/Clock/"). */
typedef struct {
  const char *old;
  const char *now;
} LegacyPath;

extern const LegacyPath legacy_paths[];
extern const guint      legacy_n_paths;
extern const LegacyPath neo_paths[];
extern const guint      neo_n_paths;

/* per-user directories named after a program rather than an app id ($XDG_*_HOME/neo-launcher, .../gnome-phone):
 * old name, new name */
extern const LegacyPath neo_dirs[];
extern const guint      neo_n_dirs;

/* the browser's home screen shortcuts are desktop files named <browser id>.shortcut-<n>.desktop */
#define LEGACY_SHORTCUT_PREFIX LEGACY_PREFIX "NeoBrowser.shortcut-"
#define NEO_SHORTCUT_PREFIX    NEO_PREFIX "Browser.shortcut-"
#define SHORTCUT_PREFIX        APP_PREFIX "Browser.shortcut-"

/* What an old app id ("<prefix>Clock") or desktop id ("<prefix>Clock.desktop", a browser shortcut's), of either
 * generation, is called now, or NULL when it is not one of the old ids. Free the result. */
char *legacy_app_id     (const char *id);
char *legacy_desktop_id (const char *id);

/* the file whose presence says the user's settings and files were carried across: $XDG_STATE_HOME/vibeos/renamed */
char *legacy_stamp_path (void);
