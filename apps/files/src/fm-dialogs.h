/* The dialogs, a dark alert dialog on libvibeos's VibeosDialog: a card with the title, the content,
 * and CANCEL / OK in the accent at the end. */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

#include "fm-places.h"

G_BEGIN_DECLS

/* a dialog with this title (NULL for none) and a content box to fill; buttons go on with fm_dialog_add_button */
VibeosDialog  *fm_dialog_new         (const char *title, GtkWidget **content);
/* a button, its label upper-cased */
GtkWidget  *fm_dialog_add_button  (VibeosDialog *dialog, const char *label, int response);
/* the underlined text field */
GtkWidget  *fm_entry_new          (const char *text);
/* a form row: the label (weight 3) and the field (weight 7), as the server and Properties dialogs have */
GtkWidget  *fm_form_row           (const char *label, GtkWidget *field);

/* the name field: Create Folder, Rename, a bookmark's or a remote's name. @check gives the message to show under
 * the field for a text that is not good (OK stays disabled meanwhile), or NULL. @select_end: the characters
 * selected from the start (the stem of a file's name), -1 for all. @done gets the text, or NULL when cancelled. */
typedef const char *(*FmTextCheck) (const char *text, gpointer data);
typedef void (*FmTextDone) (const char *text, gpointer data);
void        fm_text_dialog        (GtkWidget *parent, const char *title, const char *text, int select_end,
                                   FmTextCheck check, FmTextDone done, gpointer data);

/* the same with @suffix after the field on its line (Compress: ".zip") */
void        fm_text_dialog_suffix (GtkWidget *parent, const char *title, const char *text, int select_end,
                                   const char *suffix, FmTextCheck check, FmTextDone done, gpointer data);

/* Batch Rename: @names as they are, the new name's field, "." and the extension's field (@extension, "" for none);
 * MORE shows the buttons that put the old name and the number at the cursor. While a pattern is typed the list shows
 * the names @preview makes of it (n_names of them). @done gets the pattern and the extension, or NULLs when
 * cancelled. fm_batch_name makes an item's name from the pattern. */
#define FM_BATCH_NAME   "%%"
#define FM_BATCH_NUMBER "##"
typedef GStrv (*FmBatchNames) (const char *pattern, const char *extension, gpointer data);
typedef void (*FmBatchDone) (const char *pattern, const char *extension, gpointer data);
void        fm_batch_rename_dialog (GtkWidget *parent, const char *const *names, guint n_names,
                                    const char *extension, FmBatchNames preview, FmBatchDone done, gpointer data);
char       *fm_batch_name         (const char *pattern, const char *stem, guint number);

typedef void (*FmConfirmDone) (gboolean ok, gpointer data);
void        fm_confirm_dialog     (GtkWidget *parent, const char *title, const char *message, FmConfirmDone done,
                                   gpointer data);

/* a list to pick from (Add Remote, Open with): rows of an icon and a label, no buttons; -1 when cancelled */
typedef struct {
  FmGlyph glyph;                /* drawn with fm-icon, unless @icon is set */
  GIcon *icon;                  /* an app's icon (Open with) */
  const char *label;
} FmListRow;
typedef void (*FmChosen) (int index, gpointer data);
void        fm_list_dialog        (GtkWidget *parent, const char *title, const FmListRow *rows, int n_rows,
                                   FmChosen chosen, gpointer data);

/* Properties of the items (FmItem), their size and contents counted in a worker */
void        fm_properties_dialog  (GtkWidget *parent, GPtrArray *items);
/* View: the sort order, Show hidden files, Show thumbnails, written to @settings on OK */
void        fm_view_dialog        (GtkWidget *parent, GSettings *settings);

/* Delete: the question for the items (FmItem), the count line, and "Permanently delete" when they can go to the
 * Recycle Bin (@can_trash), checked unless @use_trash. @done gets whether to go on and whether permanently. */
typedef void (*FmDeleteDone) (gboolean ok, gboolean permanently, gpointer data);
void        fm_delete_dialog      (GtkWidget *parent, GPtrArray *items, gboolean can_trash, gboolean use_trash,
                                   FmDeleteDone done, gpointer data);

G_END_DECLS
