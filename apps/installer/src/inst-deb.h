/* inst-deb: what a .deb file is, read from the file itself.
 *
 * The control fields come from dpkg-deb --field; the app's name and icon from the package's own files, read in one
 * pass over dpkg-deb --fsys-tarfile without writing anything to disk: the desktop files in usr/share/applications,
 * and the icons in usr/share/icons and usr/share/pixmaps. A package with a launchable desktop file (an app) is shown
 * by that file's name and icon; any other by its package name. No GTK here: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define INST_DEB_ERROR (inst_deb_error_quark ())
GQuark inst_deb_error_quark (void);

typedef enum {
  INST_DEB_ERROR_NOT_A_PACKAGE,   /* dpkg-deb cannot read it, or it has no Package or Version field */
  INST_DEB_ERROR_NO_DPKG,         /* dpkg-deb cannot be run */
} InstDebError;

typedef struct {
  char    *package;           /* the control fields */
  char    *version;
  char    *architecture;
  char    *maintainer;
  char    *homepage;
  char    *summary;           /* the description's first line */
  char    *description;       /* the rest of it, as paragraphs separated by blank lines */
  guint64  installed_size;    /* bytes (Installed-Size is KiB); 0 when the field is missing */
  guint64  file_size;         /* the .deb's own size */

  char    *app_name;          /* the launchable desktop file's Name (in the user's language), or NULL */
  char    *desktop_path;      /* where that file will be installed (/usr/share/applications/foo.desktop), or NULL */
  GBytes  *icon;              /* its icon's file, or NULL */
  char    *icon_type;         /* "png", "svg" or "xpm" */
} InstDeb;

/* Reads the package at `path`: its control fields, then its files. Blocking (dpkg-deb runs twice and the data
 * archive is decompressed once): run it in a thread. */
InstDeb *inst_deb_inspect (const char *path, GCancellable *cancellable, GError **error);
void     inst_deb_free    (InstDeb *deb);

/* The two halves of inst_deb_inspect: the control fields and sizes (quick: the control archive only), then the app's
 * name, desktop file and icon from the data archive (as long as decompressing the whole package takes). FALSE with
 * `error` set when the data archive cannot be read; the fields stay as they were. */
InstDeb *inst_deb_read_control (const char *path, GCancellable *cancellable, GError **error);
gboolean inst_deb_read_files   (InstDeb *deb, const char *path, GCancellable *cancellable, GError **error);

/* The parts of the inspection on their own, for the tests */

/* a Description field (synopsis and extended lines) as a summary and paragraphs */
void     inst_deb_split_description (const char *field, char **summary, char **description);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (InstDeb, inst_deb_free)

G_END_DECLS
