/* Which packages run sandboxed, and what makes every way of starting them go through the sandbox.
 *
 * A package is registered when App Installer installs it from Software or from a .deb file (vibeos-sandbox add), or
 * found in PackageKit's history as installed by App Installer before there was a sandbox (vibeos-sandbox sync, once).
 * Its record is $XDG_DATA_HOME/vibeos-sandbox/packages/<package>; its apps keep their files in
 * $XDG_DATA_HOME/vibeos-sandbox/data/<package>, which is their home (deleted with the package: vibeos-sandbox sync).
 *
 * The package's own files stay as they are. On the user's side, which comes first:
 *   - each of its apps (a desktop file in /usr/share/applications) gets a copy in ~/.local/share/applications whose
 *     Exec lines run vibeos-sandbox (and with no DBusActivatable: GIO runs the Exec line, which forwards the files).
 *     An app whose desktop id is not a valid app id for the portals (firefox-esr, supertux2) shows under
 *     vibeos.app.<id> instead (GNOME Shell only matches a sandboxed app's windows to a desktop id that starts with
 *     the sandbox's app id); the copy under its own id, sandboxed too, is not shown (NoDisplay) but still opens
 *     the files it is the default app for, and whatever asks for the app by that id;
 *   - each D-Bus service it has on the session bus gets a copy in ~/.local/share/dbus-1/services that runs
 *     vibeos-sandbox (no SystemdService: the bus starts the sandbox itself);
 *   - each of its /etc/xdg/autostart entries gets a copy in ~/.config/autostart: Hidden=true, or run through the
 *     sandbox when the app may run in the background;
 *   - each of its systemd user units is masked (~/.config/systemd/user/<unit> -> /dev/null).
 * Every file written carries X-VibeOS-Sandbox=<app id>; a file of the user's own in the way is kept beside it as
 * <name>.vibeos-sandbox-saved and put back when the package goes. */
#pragma once

#include "vs-perms.h"

#include <gio/gio.h>

G_BEGIN_DECLS

#define VS_MARK_KEY "X-VibeOS-Sandbox"
/* a sandboxed app's desktop file: the package's own, for what asks which package the app is from */
#define VS_SOURCE_KEY "X-VibeOS-Sandbox-Source"

typedef struct {
  char *data_home;     /* ~/.local/share */
  char *config_home;   /* ~/.config */
  char *sysroot;       /* where /usr and /etc are ("" on the device; the tests use a tree of their own) */
  char *dpkg_admin;    /* /var/lib/dpkg */
  char *packagekit_db; /* PackageKit's history */
  char *launcher;      /* what the Exec lines run */
} VsDirs;

VsDirs  *vs_dirs_new_default (void);
void     vs_dirs_free        (VsDirs *dirs);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (VsDirs, vs_dirs_free)

/* ---- dpkg ---- */

typedef struct {
  char *name, *version, *arch;
} VsDpkgPackage;

/* the installed packages, by name */
GHashTable *vs_dpkg_installed (const VsDirs *dirs, GError **error);

/* where a package is: its files are there (installed), it is gone (not installed, or only its configuration files
 * are left), or dpkg is in the middle of unpacking, configuring or removing it */
typedef enum {
  VS_DPKG_GONE,
  VS_DPKG_INSTALLED,
  VS_DPKG_CHANGING,
} VsDpkgState;

/* each package's state, by name (a package not in it is gone) */
GHashTable *vs_dpkg_states    (const VsDirs *dirs, GError **error);
VsDpkgState vs_dpkg_state     (GHashTable *states, const char *name);
/* the files of an installed package */
char      **vs_dpkg_files     (const VsDirs *dirs, const VsDpkgPackage *package);

/* ---- what a package has ---- */

typedef struct {
  char     *desktop_id;   /* foo.desktop */
  char     *app_id;       /* the sandbox's app id: the desktop id without .desktop, or vibeos.app.<id> */
  gboolean  renamed;      /* the app shows under app_id.desktop, its own id hidden */
  char     *source;       /* its desktop file */
} VsApp;

typedef struct {
  char      *name;
  char      *version;
  char      *origin;      /* "store", "file", "history" */
  GPtrArray *apps;        /* VsApp */
  GPtrArray *services;    /* char *: its session D-Bus service files */
  GPtrArray *autostart;   /* char *: its /etc/xdg/autostart entries */
  GPtrArray *units;       /* char *: its systemd user units' names */
  GPtrArray *bus_names;   /* char *: the names its services have */
  /* in the record */
  GPtrArray *written;     /* char *: the files written for it */
  GPtrArray *masked;      /* char *: the units masked for it */
} VsPackage;

void       vs_package_free     (VsPackage *package);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (VsPackage, vs_package_free)

/* the permissions an app asks for, a bit per VsPerm: its desktop file's X-VibeOS-Permissions (network;camera;…),
 * or all of them when it has no such key. A permission it does not ask for is never granted. */
#define VS_REQUESTS_KEY "X-VibeOS-Permissions"
#define VS_REQUESTS_ALL ((1u << VS_N_PERMS) - 1)
guint      vs_app_requests     (const VsApp *app);
gboolean   vs_app_declares     (const VsApp *app);

/* the app id for a desktop id */
char      *vs_app_id_for       (const char *desktop_id, gboolean *renamed);
/* VibeOS's own packages and the session's never run sandboxed */
gboolean   vs_package_is_system (const char *name);
/* what the installed package has; NULL with an error when it is not installed, NULL with no error when it has
 * nothing to sandbox (no app, no service) */
VsPackage *vs_package_scan     (const VsDirs *dirs, const VsDpkgPackage *installed, GError **error);

/* ---- the records ---- */

/* the packages registered, by name */
GPtrArray *vs_registry_list    (const VsDirs *dirs);
VsPackage *vs_registry_load    (const VsDirs *dirs, const char *name);
/* the package an app id belongs to (NULL: none registered) */
VsPackage *vs_registry_find_app (const VsDirs *dirs, const char *app_id);

/* what the last call changed on the user's side, for the caller to tell the bus and systemd */
typedef struct {
  gboolean services;     /* D-Bus service files: the bus reloads its configuration */
  gboolean units;        /* masks: systemd reloads */
  gboolean apps;         /* desktop files */
} VsChanges;

/* writes the files for `package` (scanned) and its record; `autostart`: its autostart entries run (sandboxed)
 * instead of being hidden */
gboolean   vs_registry_write   (const VsDirs *dirs, VsPackage *package, gboolean autostart, VsChanges *changes,
                                GError **error);
/* takes the files and the record away (the app's own files in data/ stay) */
gboolean   vs_registry_remove  (const VsDirs *dirs, const char *name, VsChanges *changes, GError **error);

/* the app's home on the host */
char      *vs_app_data_dir     (const VsDirs *dirs, const char *package_or_app_id);
/* deletes that home and everything in it, as when the app is uninstalled: only data/<name> itself (no name with a
 * "/", no "." or ".."), never through a symbolic link nor into another file system mounted in it */
gboolean   vs_app_data_remove  (const VsDirs *dirs, const char *package_or_app_id, GError **error);
/* deletes the app's cache (.cache in its home, its XDG_CACHE_HOME), with the same care; the app makes it again */
gboolean   vs_app_cache_clear  (const VsDirs *dirs, const char *package_or_app_id, GError **error);

/* ---- PackageKit's history ---- */

/* the packages App Installer installed (newly, successfully) before; `client` is a part of the program's command
 * line that must be there ("vibeos-installer") */
char     **vs_history_installed (const VsDirs *dirs, const char *client, GError **error);

G_END_DECLS
