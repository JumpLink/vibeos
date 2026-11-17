/* sw-dpkg: what is installed, from dpkg's status file.
 *
 * Software reads the file itself (in a worker thread) rather than asking App Installer one package at a time: the
 * Mine tab and every row need the whole list at once. Only packages dpkg reports "install ok installed" count;
 * half-installed or removed-with-config packages are not installed. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the status file: VIBEOS_SOFTWARE_DPKG_STATUS in the environment (tests, pictures), else dpkg's */
const char *sw_dpkg_status_path (void);

/* package -> version, from a status file's text. A package installed for two architectures (Multi-Arch: same) is
 * listed once, with the version of whichever comes last (they are the same version). */
GHashTable *sw_dpkg_parse_status (const char *text, gsize length);

/* reads and parses `path` in a thread */
void        sw_dpkg_read_async   (const char *path, GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer data);
GHashTable *sw_dpkg_read_finish  (GAsyncResult *result, GError **error);

/* the desktop file a package installs (/var/lib/dpkg/info/PACKAGE[:ARCH].list), as an id ("org.gnome.Chess.desktop"),
 * or NULL: blocking, for a thread */
char       *sw_dpkg_desktop_id   (const char *package);

G_END_DECLS
