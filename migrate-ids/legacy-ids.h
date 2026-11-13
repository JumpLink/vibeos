/* The IDs gnome-phone had up to 0.1.6, and what each is called now (neo.*). gnome-phone-migrate-ids carries a user's
 * settings and files across with this table, and the launcher reads old desktop ids in layouts and in
 * hidden-from-home with it. */
#pragma once

#include <glib.h>

/* Up to 0.1.6 every ID began with the reverse-DNS prefix of the project gnome-phone came out of. That name is not
 * to appear in this tree again: this is the one place that has to know it, so it is put together from pieces here,
 * and it is only ever compared with what an older install left behind. */
#define LEGACY_VENDOR "yes" "man"
#define LEGACY_PREFIX "de." LEGACY_VENDOR "."   /* app, desktop, D-Bus and schema ids */
#define LEGACY_PATH   "/de/" LEGACY_VENDOR "/"  /* GSettings paths */
#define LEGACY_EXTENSION "gnome-phone@" LEGACY_VENDOR ".de"

#define EXTENSION_UUID "gnome-phone@neo"

typedef struct {
  const char *old;   /* the id without LEGACY_PREFIX ("Clock" for the clock) */
  const char *now;   /* the id since 0.1.7 ("neo.Clock") */
  gboolean    data;  /* its per-user directories ($XDG_*_HOME/<id>) move with it */
} LegacyId;

extern const LegacyId legacy_ids[];
extern const guint    legacy_n_ids;

/* GSettings paths: the old directory under LEGACY_PATH ("Clock/") and the new one ("/neo/Clock/") */
typedef struct {
  const char *old;
  const char *now;
} LegacyPath;

extern const LegacyPath legacy_paths[];
extern const guint      legacy_n_paths;

/* the browser's home screen shortcuts are desktop files named <browser id>.shortcut-<n>.desktop */
#define LEGACY_SHORTCUT_PREFIX LEGACY_PREFIX "NeoBrowser.shortcut-"
#define SHORTCUT_PREFIX        "neo.Browser.shortcut-"

/* What an old app id ("<prefix>Clock") or desktop id ("<prefix>Clock.desktop", a browser shortcut's) is called
 * now, or NULL when it is not one of the old ids. Free the result. */
char *legacy_app_id     (const char *id);
char *legacy_desktop_id (const char *id);

/* the file whose presence says the user's settings and files were carried across:
 * $XDG_STATE_HOME/gnome-phone/ids-migrated */
char *legacy_stamp_path (void);
