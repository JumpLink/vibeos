/* dictate-download-dialog: a language switched to that is not on the phone yet, downloaded when the user agrees.
 *
 * A language is ready when its speech model is installed (dictate-models.h) with its recogniser's library and, in
 * Agent, there is a voice for it (dictate-talk.h). What it lacks of the models and voices comes from
 * dictate-download.h: a dialog names the language, what would be downloaded, from where and how much, with Cancel
 * and Download; then a dialog with the progress and Cancel; on a failure one that says why, with Retry. A
 * recogniser's library that is not installed (Vosk's) is code: it comes only as its signed package from the VibeOS
 * archive, installed by the system's package manager (dictate-package.h) in the same run, before the files. */
#pragma once

#include "dictate-talk.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*DictateLanguageReady) (const char *language, gpointer data);

/* the language list's line under `language` when it is not ready (with Agent's `talk`, its voice counts): what it
 * lacks; NULL when it is ready */
char *dictate_download_note   (const char *language, DictateTalk *talk);
/* `ready` at once when `language` is ready. Otherwise the user is asked to download what it lacks, sees it come, and
 * `ready` is called once it is all in place; or is told why it cannot be. A cancel, a failure that is not retried,
 * or the window closing leave everything as it was, and `ready` is not called. The dialogs are over `window`. */
void  dictate_download_ensure (GtkWidget *window, const char *language, DictateTalk *talk,
                               DictateLanguageReady ready, gpointer data);

G_END_DECLS
