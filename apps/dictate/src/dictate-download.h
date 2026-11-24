/* dictate-download: the speech models and voices a language needs, fetched from their makers' servers.
 *
 * A pack is what one language lacks for one thing: its speech model (dictate-models.h: Moonshine's tiny streaming
 * models from download.moonshine.ai, Vosk's small models from alphacephei.com, the same files and sums as Dictate's
 * language packages, tools/vibeos-dictate-speech/build-debs.sh) or Agent's voice for it (a Piper voice from
 * rhasspy/piper-voices on Hugging Face, at one revision). Every file is pinned here, with its size and SHA-256.
 * Only data comes this way: a recogniser's library (dictate-recogniser.h) is code, and comes from its package.
 *
 * A download has a directory of its own in vibeos-dictate/.downloads under the user's data directory. Each file comes
 * into NAME.part, its sum taken as it comes, and is renamed NAME once its size and sum are right; a zip is then
 * unpacked there, only its directories and plain files, each inside its one top directory. Only once every file is
 * in and checked does it all move to its place in vibeos-dictate/models or vibeos-dictate/voices, by renames on the
 * same file system. After a failure or a cancel the directory goes, with everything in it: nothing is left half
 * there. Nothing that comes is ever run. */
#pragma once

#include "dictate-models.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  const char *name;     /* its name at the pack's source, and in its place */
  guint64 size;
  const char *sha256;
} DictateFile;

typedef enum {
  DICTATE_PACK_MODEL,   /* a speech model: a directory in vibeos-dictate/models */
  DICTATE_PACK_VOICE,   /* Agent's voice: files in vibeos-dictate/voices/LANGUAGE */
} DictatePackKind;

typedef struct {
  const char *language;
  DictatePackKind kind;
  DictateKind recogniser;       /* a model's: the library it needs */
  const char *title;            /* for the user: "Moonshine", "Piper voice Thorsten" */
  const char *licence;          /* its licence, for the user */
  const char *source;           /* https://…: the URL its files' names follow, after a / */
  const char *place;            /* under vibeos-dictate: a model's directory, "models/tiny-streaming-de"; a voice's,
                                 * "voices/de" */
  const char *archive_top;      /* a zip (its one file), unpacked: its one top directory, which is `place`'s last
                                 * part; NULL for plain files */
  const DictateFile *files;     /* up to one with a NULL name */
} DictatePack;

/* the pack for `language`'s speech model or voice; NULL if there is none to download */
const DictatePack *dictate_pack_find   (const char *language, DictatePackKind kind);
guint64            dictate_pack_size   (const DictatePack *pack);
/* its source's host, for the user: "huggingface.co" */
char              *dictate_pack_host   (const DictatePack *pack);
/* every pack, for the tests: `n` of them */
const DictatePack *dictate_packs       (guint *n);

typedef enum {
  DICTATE_DOWNLOAD_WAITING,     /* not started */
  DICTATE_DOWNLOAD_FETCHING,
  DICTATE_DOWNLOAD_INSTALLING,  /* all in: unpacked and moved to their places */
  DICTATE_DOWNLOAD_DONE,
  DICTATE_DOWNLOAD_FAILED,      /* dictate_download_get_error says why */
  DICTATE_DOWNLOAD_CANCELLED,
} DictateDownloadState;

#define DICTATE_TYPE_DOWNLOAD (dictate_download_get_type ())
G_DECLARE_FINAL_TYPE (DictateDownload, dictate_download, DICTATE, DOWNLOAD, GObject)

/* the packs (kept: they are this file's table, or the caller's for as long as the download) */
DictateDownload     *dictate_download_new          (const DictatePack *const *packs, guint n);
/* fetches the packs; their files move to their places once all of them are in and checked */
void                 dictate_download_start        (DictateDownload *self);
/* stops, and what was fetched goes; once the files are being moved to their places it is too late, and they are */
void                 dictate_download_cancel       (DictateDownload *self);
DictateDownloadState dictate_download_get_state    (DictateDownload *self);
/* the packs' bytes, and how many of them have come */
guint64              dictate_download_get_size     (DictateDownload *self);
guint64              dictate_download_get_received (DictateDownload *self);
/* the pack coming now */
const DictatePack   *dictate_download_get_pack     (DictateDownload *self);
/* what went wrong, for the user */
const char          *dictate_download_get_error    (DictateDownload *self);

/* Signals, on the main thread:
 *   "progress" (): more has come, at most ten times a second
 *   "finished" (): DONE, FAILED or CANCELLED, with nothing left of the download but what is in its place */

/* the free bytes where the downloads go */
guint64   dictate_download_free_space  (void);

/* for the tests: `archive` (a zip or a tar) unpacked into the directory `dir`, every entry a directory or a plain
 * file inside `top`, `limit` bytes in all at most */
gboolean  dictate_download_unpack      (const char *archive, const char *dir, const char *top, guint64 limit,
                                        GCancellable *cancellable, GError **error);
/* `path` and all in it, without following a link */
gboolean  dictate_download_remove_tree (const char *path, GError **error);

/* what was downloaded for `language` and is in its place under the user's data directory (its model's directory, its
 * voice's files), in bytes; 0 when nothing (what the system has is not counted) */
guint64   dictate_download_downloaded_size (const char *language);
/* takes that off the phone again, without following a link */
gboolean  dictate_download_remove          (const char *language, GError **error);

G_END_DECLS
