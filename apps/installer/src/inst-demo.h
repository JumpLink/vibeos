/* inst-demo: the canned states App Installer shows without PackageKit, polkit or a real package to change, for
 * screenshots and tests.
 *
 * VIBEOS_INSTALLER_DEMO in the environment turns it on: a comma-separated list of settings, each key=value (a key
 * alone is key=yes). Nothing is installed or removed then: PackageKit is never called, and every transaction plays a
 * script. The settings (all optional):
 *
 *   package=NAME      the package the settings below are about (default: whichever is asked for)
 *   installed=VER     NAME is installed in this version (default: as dpkg says)
 *   candidate=VER     the repositories have this version of NAME (default 1.3-1)
 *   essential         NAME is Essential (cannot be uninstalled)
 *   name=TEXT         NAME's app is called TEXT (the software sources' catalog; without it NAME is no app)
 *   icon=ICON         and has the icon named ICON
 *   app=ID            and, once installed, is the desktop file ID (for Open)
 *   installed-size=B  NAME takes B bytes installed (default 9 MiB), and its package
 *   package-size=B    B bytes to download (default 2600000)
 *   deps=N            the check finds N other packages to install (default 0), DOWNLOAD bytes to download
 *   download=BYTES
 *   removes=A+B       the check finds packages A and B to remove too
 *   missing=TEXT      the check fails: a dependency cannot be met (TEXT is PackageKit's detail)
 *   refresh           the software sources' lists are old: they are refreshed first
 *   noreinstall       the same version cannot be installed again (PackageKit's apt backend)
 *   notfound          the repositories have no package NAME
 *   fail=WHAT         the real transaction fails: auth, lock, network, broken, space, conflict, cancelled, nopk,
 *                     setup
 *   hold=WHAT         stop there: checking (the check never ends), auth (waiting for the password), progress
 *                     (downloading at 45 %), installing (installing at 80 %)
 *   auto              answer yes as soon as App Installer asks (to reach the progress and the end)
 *   api=CALL          at start, act as if another app had called the D-Bus API: install:NAME, file:PATH, remove:NAME
 *   delay=MS          how long each scripted step takes (default 600) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean    inst_demo_active (void);
/* a setting's value ("yes" for a key alone), or NULL */
const char *inst_demo_get    (const char *key);
gboolean    inst_demo_has    (const char *key);
/* a setting about `package`: NULL when package= names another one */
const char *inst_demo_get_for (const char *key, const char *package);
guint64     inst_demo_number (const char *key, guint64 fallback);

G_END_DECLS
