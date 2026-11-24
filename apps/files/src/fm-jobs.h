/* The file operations: copy, move, to the Recycle Bin, delete, restore, empty the Recycle Bin, compress and rename
 * several. Each runs in a GTask thread with a GCancellable; the window never waits on I/O. A job that takes over
 * 500 ms shows the progress dialog (HIDE leaves a thin progress line at the window's bottom); an existing target
 * asks Overwrite / Rename / Skip; the end says what was done in a snackbar (with UNDO after a recycle) and lists the
 * folder again. */
#pragma once

#include <gio/gio.h>

#include "fm-window.h"

G_BEGIN_DECLS

/* @files (GFile) into the folder @dest; a copy into the folder they are in makes "name - Copy" */
void fm_jobs_copy        (FmShell *shell, GPtrArray *files, GFile *dest, gboolean move);
/* @files (GFile) to the Recycle Bin (@trash) or deleted for good */
void fm_jobs_delete      (FmShell *shell, GPtrArray *files, gboolean trash);
/* the Recycle Bin's @items (FmItem: the trash:/// file and its trash::orig-path) back where they were */
void fm_jobs_restore     (FmShell *shell, GPtrArray *items);
void fm_jobs_empty_trash (FmShell *shell);
/* Compress: @files (GFile) into the .zip @target, each under its own name, a folder with all it has */
void fm_jobs_compress    (FmShell *shell, GPtrArray *files, GFile *target);
/* Batch Rename: each of @files (GFile) to the display name at the same index of @names, in its own folder; a name
 * that is taken leaves its file as it was */
void fm_jobs_rename      (FmShell *shell, GPtrArray *files, char **names);

G_END_DECLS
