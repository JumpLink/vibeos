/* inst-catalog: what to show for a package: the app's name, icon and description.
 *
 * For a package in the software sources, the AppStream catalog says (the metadata Software shows, which apt
 * downloads with the package lists and libappstream reads from /var/lib/swcatalog, /usr/share/swcatalog and the apt
 * lists); a package without an app in it has none, and PackageKit's GetDetails gives its summary and description
 * instead (inst-job.c). For an installed package, its own desktop file says: the one the app drawer shows.
 *
 * Both are blocking (the catalog takes a moment to load, the first time): call them from a thread. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  char  *name;          /* the app's name, as the drawer shows it */
  char  *summary;
  char  *description;   /* plain text: paragraphs apart by an empty line, list items starting with "• " */
  char  *homepage;
  GIcon *icon;          /* a themed icon, or a file (the catalog's own icon cache) */
  char  *desktop_id;    /* foo.desktop: the app is installed and can be opened */
} InstAppInfo;

/* the app the AppStream catalog has in `package`, or NULL */
InstAppInfo *inst_catalog_lookup    (const char *package);
/* the app the installed package `package` puts in the drawer, or NULL */
InstAppInfo *inst_catalog_installed (const char *package);
/* the desktop file the drawer would show among `files` (paths), the one named after `package` first: its path or
 * NULL */
char        *inst_catalog_pick_desktop (const char *const *files, const char *package);

void         inst_app_info_free     (InstAppInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (InstAppInfo, inst_app_info_free)

G_END_DECLS
