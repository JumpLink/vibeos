/* Formatting and name rules the screens share. No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* "9.8 GB": decimal units, as g_format_size gives them */
char       *fm_format_size        (guint64 size);
/* the locale's short date ("10/15/2026" in en_US) */
char       *fm_format_date        (gint64 unix_time);
/* the date and time for Properties */
char       *fm_format_date_time   (gint64 unix_time);
/* "5 items", "1 item", "- items" for a count not known */
char       *fm_format_items       (int count);
/* "1:05" / "1:02:03" for the progress dialog's remaining time */
char       *fm_format_duration    (guint seconds);
/* "rwxr-xr-x" */
char       *fm_format_permissions (guint32 mode);
/* a dot-file, a backup or what the backend calls hidden; a backend that leaves the flags out (WebDAV) hides
 * only dot-files */
gboolean    fm_info_is_hidden     (GFileInfo *info);

typedef enum {
  FM_NAME_OK,
  FM_NAME_EMPTY,
  FM_NAME_BAD_CHARACTER,       /* a '/' (or a NUL), or "." / ".." */
} FmNameCheck;

FmNameCheck fm_check_name         (const char *name);
/* the message for a check that failed, or NULL */
const char *fm_name_check_message (FmNameCheck check);

/* "name - Copy.ext", then "name - Copy (2).ext" for @n 2 and up: the extension stays at the end of a
 * file's name; a folder's whole name is the stem */
char       *fm_copy_name          (const char *name, gboolean folder, guint n);
/* the characters of the base name, the extension left out (Rename selects them): 0 .. *end */
void        fm_stem_range         (const char *name, gboolean folder, int *end);

/* a path under a location's root as the drawer shows it: "/VIDEO", "/" at the root */
char       *fm_relative_path      (GFile *root, GFile *file);
/* the folder @file is in, NULL at the top; for a server's URI by the URI (smb://host/share/ is in smb://host/) */
GFile      *fm_parent             (GFile *file);
/* whether @file is @root or under it, by the paths or the URIs */
gboolean    fm_file_is_under      (GFile *file, GFile *root);
/* @file's display path: the local path, else the URI without the password, unescaped */
char       *fm_display_path       (GFile *file);

/* what a delete or a copy covers ("Folder : %d, File : %d, Size : %s") */
typedef struct {
  guint folders, files;
  guint64 bytes;
} FmCount;
/* adds @file and everything under it (links not followed); blocking: a worker's */
void        fm_count_tree         (GFile *file, GCancellable *cancellable, FmCount *count);

/* case-insensitive "contains", on case-folded UTF-8 (the search) */
gboolean    fm_name_matches       (const char *name, const char *folded_needle);

G_END_DECLS
