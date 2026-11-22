/* dictate-download: see dictate-download.h.
 *
 * The fetching is libsoup's, asynchronous on the main loop: a file at a time, read in pieces, each piece written to
 * the .part file and added to its sum. Unpacking and moving into place, and removing a download's directory, run on
 * a thread of their own. */
#include "dictate-download.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIECE           (64 * 1024)
#define TIMEOUT_S       30              /* a server that says nothing for this long has failed */
#define PROGRESS_US     (100 * 1000)    /* "progress" at most this often */
#define ENTRIES_MAX     10000           /* in an archive */
#define UNPACKED_TIMES  4               /* an archive's contents at most this many times its size */

/* ---------------- the packs ---------------- */

/* Moonshine's tiny streaming models as Moonshine's v0.1.6 release lists them (core/moonshine-model-file-metadata
 * .generated.cpp); the sums are build-debs.sh's. decoder_kv_with_attention.ort is left out: it is only for word
 * timestamps, which Dictate does not ask for. */
#define MOONSHINE "https://download.moonshine.ai/model/"

static const DictateFile MOONSHINE_EN[] = {
  { "adapter.ort", 1319616, "c5f9f7cc98c27a965d8e200816f7e5c07b892712bb30cf07444bbde4ffd38199" },
  { "cross_kv.ort", 1287504, "5f68900da4ea20f40fbe67bbec36399f0d47faedbd7198b29abd9dc5f472ef9f" },
  { "decoder_kv.ort", 32748720, "6564f2c9a19de44195a53e336e07d05108f14babab57105d623bb68b79572d75" },
  { "encoder.ort", 7675392, "617e403b2aae48f2474e0b148bf835deb9510bc8692efeabfe27203959a3256e" },
  { "frontend.model.ort", 23392, "5662679f0e5758646d8eade3bc4b5d9ebda875feb6e696c0cc5f1a08fea2219e" },
  { "frontend.weights.ort", 2093416, "55c91d7efab9f7506d7d753131cba72145ff016be3447319fe414a31e13f25b4" },
  { "streaming_config.json", 509, "74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950" },
  { "tokenizer.bin", 249974, "6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d" },
  { NULL }
};

static const DictateFile MOONSHINE_DE[] = {
  { "adapter.ort", 1318472, "23b91142a35cfbc533a7f03ad8387d0b6d32856b465c6fe7a14ede44a254a345" },
  { "cross_kv.ort", 1288120, "25e6d9d4f16569c2ede7e23a381fe7c60cc4b0f141adc88580557d4530634f17" },
  { "decoder_kv.ort", 19717336, "768b1464074fc4fb7327e8b863fc13dcd12cb7bbe8d8e9428fa7292f7fd9ca6e" },
  { "encoder.ort", 7772792, "74610b818058e396f4b9d9ffd730cf8e36772122464d989dd3011dfa76bd6baf" },
  { "frontend.model.ort", 23176, "4445bb4c2f41b0dd993999e06dc9367a0dde0d42a2e777116e7d93be102dc43d" },
  { "frontend.weights.ort", 2093280, "233fda61d583403bbf13ea71b6ba2335fdf57eac71be85a97dcbb90a6c2f4186" },
  { "streaming_config.json", 509, "d31afd7c2dd5ee9ae3fa232312c5b42cb3e4fe1c6e9c53d885f2bea63023790a" },
  { "tokenizer.bin", 103319, "5725ad3771a1339802f8cebeda0fa8f7723f5282b1ee726c2544774d7399b9a7" },
  { NULL }
};

static const DictateFile MOONSHINE_ES[] = {
  { "adapter.ort", 1318472, "2efd97cd2ee7d89578dd1d429a849be9fe37ac81dd9c4e1d31c8a2e7f85d43e6" },
  { "cross_kv.ort", 1288120, "9e268b1b3bb0f9ff79bca6e76055d41298de0a5ba52505ee04907a2642efa3ae" },
  { "decoder_kv.ort", 19717336, "7adf56b982df98e385d71823b9a40cf9296ae3a746efcf03f3d7bb4604032db0" },
  { "encoder.ort", 7772792, "ec9eb43a01fbfe242a2c6a86050cffe4a31050e1c9f084e98560d6c100004505" },
  { "frontend.model.ort", 23176, "9d19434b861de0c1d2aff52556911ae565cf3212ebe6d7f797c76c5c5066c2fc" },
  { "frontend.weights.ort", 2093280, "5e4d5471cd5d984d4b94a603d861539ebf1dc72de11b0cf13f38316725d5ce48" },
  { "streaming_config.json", 509, "d31afd7c2dd5ee9ae3fa232312c5b42cb3e4fe1c6e9c53d885f2bea63023790a" },
  { "tokenizer.bin", 102888, "5fbb7d4314dcb18e03c5f975609e4a4cd572b22b01d2b2accb1b3e6830696f36" },
  { NULL }
};

/* Vosk's small models, 0.22 (alphacephei.com/vosk/models lists them as Apache-2.0); the sums are build-debs.sh's */
#define VOSK "https://alphacephei.com/vosk/models"

static const DictateFile VOSK_FR[] = {
  { "vosk-model-small-fr-0.22.zip", 42233323, "cabf6180e177eb9b3a9a9d43a437bd5e549f3a7d09525e5d69a3fed787be12ad" },
  { NULL }
};

static const DictateFile VOSK_IT[] = {
  { "vosk-model-small-it-0.22.zip", 49665141, "9ec65e75861d1c6c2e457cccd932705340dcdf233f5b239f00733b4de0bf3267" },
  { NULL }
};

static const DictateFile VOSK_RU[] = {
  { "vosk-model-small-ru-0.22.zip", 46236750, "961d5ff98a17f4aa6de69864d0aa71fa5bac682301d2b5d17a3f24c5c99a46d4" },
  { NULL }
};

/* Piper's voices at one revision of rhasspy/piper-voices (2026-11-20), a medium one per language, none of them
 * under a non-commercial licence (each voice's MODEL_CARD names its dataset's): the voices are MIT, as the
 * repository is. The .onnx sums are Hugging Face's (its LFS ids), the .onnx.json ones taken of the files whose git
 * ids are the revision's. Piper 1.5's voices merge vowel clusters (dictate-voice.c). */
#define PIPER "https://huggingface.co/rhasspy/piper-voices/resolve/e9c8a39444359388adaa94255becb37a5a2b8e51/"

static const DictateFile PIPER_DE[] = {
  { "de_DE-thorsten-medium.onnx", 63221984, "fd270eb3d1ebf91d763f8a0f949b1b1bebdfefaea18c3aaf8577dd34536e064f" },
  { "de_DE-thorsten-medium.onnx.json", 6455, "e5bb7410dcbbcc0be6f4544f6bc66612b315ab5cb3099ec44bb46d860bc7cca9" },
  { NULL }
};

static const DictateFile PIPER_EN[] = {
  { "en_US-ljspeech-medium.onnx", 63531379, "6f52a751e2349abe7a76735eb09dc1875298c77ea2342ffd2fef79ff81b87f22" },
  { "en_US-ljspeech-medium.onnx.json", 4972, "141d612cc0a95ed7efc1ca936b845c2364967f2e9217c5dbfcf69fc4d6c65860" },
  { NULL }
};

static const DictateFile PIPER_ES[] = {
  { "es_ES-davefx-medium.onnx", 63221984, "d2c9224e122321aac3c33343957fff4d0f465226cb74c66b0f520ca3f8c0dd07" },
  { "es_ES-davefx-medium.onnx.json", 6175, "7d82d069eccca0f87963bd23c33166fe1ef6d24a6b231543931bfe692397f256" },
  { NULL }
};

static const DictateFile PIPER_FR[] = {
  { "fr_FR-siwis-medium.onnx", 63221984, "b1d946fd68d0347155fa59bce0a018924abbef3c1561181db45ca463d83963ef" },
  { "fr_FR-siwis-medium.onnx.json", 6369, "3b3efdbcdcae930c40d8255527170ec5c87bf05e510b67f53d75fd028df40f43" },
  { NULL }
};

static const DictateFile PIPER_IT[] = {
  { "it_IT-paola-medium.onnx", 63221984, "f9b8658aa8c01f8280aa88488f6e7643a639b6225a81514c40440a01f6364a20" },
  { "it_IT-paola-medium.onnx.json", 6114, "10e46e8534dd2d4051f1534ba6542f189f854454acea44a6d45ff464a83260fa" },
  { NULL }
};

static const DictateFile PIPER_RU[] = {
  { "ru_RU-denis-medium.onnx", 63221984, "879bc365cb14d1380e3f5ed6044162ccf54c99f6161871569b7f830aec3ce841" },
  { "ru_RU-denis-medium.onnx.json", 6196, "5891b94580ae739a8cdc5d17591db6800333f85164d13599aca893883d99b9eb" },
  { NULL }
};

#define MOONSHINE_PACK(code, revision, files) \
  { code, DICTATE_PACK_MODEL, DICTATE_MOONSHINE, "Moonshine tiny streaming model", "MIT License", \
    MOONSHINE "tiny-streaming-" code "/" revision, "models/tiny-streaming-" code, NULL, files }
#define VOSK_PACK(code, files) \
  { code, DICTATE_PACK_MODEL, DICTATE_VOSK, "Vosk small model", "Apache License 2.0", VOSK, \
    "models/vosk-model-small-" code "-0.22", "vosk-model-small-" code "-0.22", files }
#define PIPER_PACK(code, title, licence, dir, files) \
  { code, DICTATE_PACK_VOICE, 0, title, licence, PIPER dir, "voices/" code, NULL, files }

static const DictatePack PACKS[] = {
  MOONSHINE_PACK ("de", "quantized_26_08_24", MOONSHINE_DE),
  MOONSHINE_PACK ("en", "quantized_26_11_02", MOONSHINE_EN),
  MOONSHINE_PACK ("es", "quantized_26_08_24", MOONSHINE_ES),
  VOSK_PACK ("fr", VOSK_FR),
  VOSK_PACK ("it", VOSK_IT),
  VOSK_PACK ("ru", VOSK_RU),
  PIPER_PACK ("de", "Piper voice Thorsten", "MIT; Thorsten-Voice recordings, CC0", "de/de_DE/thorsten/medium",
              PIPER_DE),
  PIPER_PACK ("en", "Piper voice LJ Speech", "MIT; LJ Speech recordings, public domain",
              "en/en_US/ljspeech/medium", PIPER_EN),
  PIPER_PACK ("es", "Piper voice Davefx", "MIT; recordings CC0", "es/es_ES/davefx/medium", PIPER_ES),
  PIPER_PACK ("fr", "Piper voice SIWIS", "MIT; SIWIS recordings, CC BY 4.0; trained from Hi-Fi TTS, CC BY 4.0",
              "fr/fr_FR/siwis/medium", PIPER_FR),
  PIPER_PACK ("it", "Piper voice Paola", "MIT; recordings CC0; trained from Hi-Fi TTS, CC BY 4.0",
              "it/it_IT/paola/medium", PIPER_IT),
  PIPER_PACK ("ru", "Piper voice Denis", "MIT; recordings CC0", "ru/ru_RU/denis/medium", PIPER_RU),
};

const DictatePack *
dictate_packs (guint *n)
{
  *n = G_N_ELEMENTS (PACKS);
  return PACKS;
}

const DictatePack *
dictate_pack_find (const char *language, DictatePackKind kind)
{
  for (guint i = 0; language && i < G_N_ELEMENTS (PACKS); i++)
    if (PACKS[i].kind == kind && g_str_equal (PACKS[i].language, language))
      return &PACKS[i];
  return NULL;
}

guint64
dictate_pack_size (const DictatePack *pack)
{
  guint64 size = 0;

  for (const DictateFile *f = pack->files; f->name; f++)
    size += f->size;
  return size;
}

char *
dictate_pack_host (const DictatePack *pack)
{
  g_autoptr (GUri) uri = g_uri_parse (pack->source, G_URI_FLAGS_NONE, NULL);

  return g_strdup (uri ? g_uri_get_host (uri) : pack->source);
}

/* ---------------- removing a tree, unpacking an archive ---------------- */

/* `name` in the directory `dir` and all in it; a link is removed, never followed */
static gboolean
remove_at (int dir, const char *name, GError **error)
{
  struct stat st;

  if (fstatat (dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    {
      if (errno == ENOENT)
        return TRUE;
      goto failed;
    }
  if (S_ISDIR (st.st_mode))
    {
      int fd = openat (dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      DIR *list = fd >= 0 ? fdopendir (fd) : NULL;
      struct dirent *entry;
      gboolean ok = TRUE;

      if (!list)
        {
          int saved = errno;
          if (fd >= 0)
            close (fd);
          errno = saved;
          goto failed;
        }
      while (ok && (entry = readdir (list)))
        if (strcmp (entry->d_name, ".") != 0 && strcmp (entry->d_name, "..") != 0)
          ok = remove_at (dirfd (list), entry->d_name, error);
      closedir (list);
      if (!ok)
        return FALSE;
      if (unlinkat (dir, name, AT_REMOVEDIR) != 0)
        goto failed;
      return TRUE;
    }
  if (unlinkat (dir, name, 0) != 0)
    goto failed;
  return TRUE;

failed:
  {
    int saved = errno;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), "%s: %s", name, g_strerror (saved));
    return FALSE;
  }
}

gboolean
dictate_download_remove_tree (const char *path, GError **error)
{
  g_autofree char *parent = g_path_get_dirname (path);
  g_autofree char *name = g_path_get_basename (path);
  int dir = open (parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  gboolean ok;

  if (dir < 0)
    {
      int saved = errno;
      if (saved == ENOENT)
        return TRUE;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), "%s: %s", parent, g_strerror (saved));
      return FALSE;
    }
  ok = remove_at (dir, name, error);
  close (dir);
  return ok;
}

static gboolean
unpack_error (GError **error, const char *format, ...) G_GNUC_PRINTF (2, 3);

static gboolean
unpack_error (GError **error, const char *format, ...)
{
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
  return FALSE;
}

/* an entry's path as parts, each a plain name: none empty, "." or "..", the first `top`; NULL if it is not one */
static char **
entry_parts (const char *path, const char *top)
{
  g_auto (GStrv) parts = NULL;
  guint n;

  if (!path || !*path || path[0] == '/' || strchr (path, '\\') || !g_utf8_validate (path, -1, NULL))
    return NULL;
  for (const char *p = path; *p; p++)
    if ((guchar) *p < 0x20 || *p == 0x7f)
      return NULL;
  parts = g_strsplit (path, "/", -1);
  n = g_strv_length (parts);
  /* "top/dir/" ends with an empty part */
  if (n > 1 && !*parts[n - 1])
    {
      g_free (parts[n - 1]);
      parts[--n] = NULL;
    }
  for (guint i = 0; i < n; i++)
    if (!*parts[i] || g_str_equal (parts[i], ".") || g_str_equal (parts[i], ".."))
      return NULL;
  if (!g_str_equal (parts[0], top))
    return NULL;
  return g_steal_pointer (&parts);
}

/* the directory `parts[0..n)` under `root`, made if it is not there; its descriptor, or -1. Each part is opened
 * without following a link, so nothing is reached outside `root`. */
static int
open_dirs (int root, char **parts, guint n)
{
  int fd = dup (root);

  for (guint i = 0; fd >= 0 && i < n; i++)
    {
      int next;

      if (mkdirat (fd, parts[i], 0755) != 0 && errno != EEXIST)
        {
          close (fd);
          return -1;
        }
      next = openat (fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close (fd);
      fd = next;
    }
  return fd;
}

static gboolean
write_all (int fd, const char *data, gsize length)
{
  while (length > 0)
    {
      ssize_t done = write (fd, data, length);

      if (done < 0 && errno == EINTR)
        continue;
      if (done <= 0)
        return FALSE;
      data += done, length -= done;
    }
  return TRUE;
}

gboolean
dictate_download_unpack (const char *archive, const char *dir, const char *top, guint64 limit,
                         GCancellable *cancellable, GError **error)
{
  struct archive *a = archive_read_new ();
  struct archive_entry *entry;
  g_autofree char *buffer = g_malloc (PIECE);
  guint64 total = 0;
  guint entries = 0;
  gboolean ok = TRUE;
  int root = -1, r = ARCHIVE_EOF;

  archive_read_support_format_zip (a);
  archive_read_support_format_tar (a);
  archive_read_support_filter_gzip (a);
  archive_read_support_filter_xz (a);
  root = open (dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0)
    ok = unpack_error (error, "%s: %s", dir, g_strerror (errno));
  else if (archive_read_open_filename (a, archive, PIECE) != ARCHIVE_OK)
    ok = unpack_error (error, "%s", archive_error_string (a));
  while (ok && (r = archive_read_next_header (a, &entry)) == ARCHIVE_OK)
    {
      const char *path = archive_entry_pathname_utf8 (entry);
      mode_t type = archive_entry_filetype (entry);
      g_auto (GStrv) parts = NULL;
      guint n;
      int parent, fd;

      if (!path)
        path = archive_entry_pathname (entry);
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        {
          ok = FALSE;
          break;
        }
      if (++entries > ENTRIES_MAX)
        {
          ok = unpack_error (error, "more than %d entries", ENTRIES_MAX);
          break;
        }
      /* only directories and plain files: no links of either kind, no devices */
      if (archive_entry_hardlink (entry) || archive_entry_symlink (entry) || (type != AE_IFREG && type != AE_IFDIR))
        {
          ok = unpack_error (error, "%s is not a plain file or a directory", path ? path : "an entry");
          break;
        }
      if (archive_entry_is_encrypted (entry))
        {
          ok = unpack_error (error, "%s is encrypted", path);
          break;
        }
      parts = entry_parts (path, top);
      if (!parts)
        {
          ok = unpack_error (error, "%s is outside %s/", path ? path : "an entry", top);
          break;
        }
      n = g_strv_length (parts);
      if (type == AE_IFDIR)
        {
          fd = open_dirs (root, parts, n);
          if (fd < 0)
            ok = unpack_error (error, "%s: %s", path, g_strerror (errno));
          else
            close (fd);
          continue;
        }
      /* a file, in the top directory or under it, never the top itself */
      if (n < 2 || (parent = open_dirs (root, parts, n - 1)) < 0)
        {
          ok = unpack_error (error, "%s: %s", path, n < 2 ? "not in a directory" : g_strerror (errno));
          break;
        }
      fd = openat (parent, parts[n - 1], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
      close (parent);
      if (fd < 0)
        {
          ok = unpack_error (error, "%s: %s", path, g_strerror (errno));
          break;
        }
      for (;;)
        {
          la_ssize_t got = archive_read_data (a, buffer, PIECE);

          if (got == 0)
            break;
          if (got < 0)
            {
              ok = unpack_error (error, "%s: %s", path, archive_error_string (a));
              break;
            }
          total += got;
          if (total > limit)
            {
              ok = unpack_error (error, "it unpacks to more than %" G_GUINT64_FORMAT " bytes", limit);
              break;
            }
          if (!write_all (fd, buffer, got))
            {
              ok = unpack_error (error, "%s: %s", path, g_strerror (errno));
              break;
            }
        }
      if (close (fd) != 0 && ok)
        ok = unpack_error (error, "%s: %s", path, g_strerror (errno));
    }
  if (ok && r != ARCHIVE_EOF)
    ok = unpack_error (error, "%s", archive_error_string (a));
  if (ok && entries == 0)
    ok = unpack_error (error, "it is empty");
  if (root >= 0)
    close (root);
  archive_read_free (a);
  return ok;
}

/* ---------------- the download ---------------- */

struct _DictateDownload {
  GObject parent;
  GPtrArray *packs;             /* const DictatePack * */
  DictateDownloadState state;
  char *error;
  char *root;                   /* vibeos-dictate in the user's data directory */
  char *dir;                    /* this download's, in root/.downloads */
  guint64 size, received;
  gint64 progressed;            /* when "progress" was last emitted */
  /* the file coming */
  guint pack, file;
  SoupSession *session;
  SoupMessage *message;
  GCancellable *cancellable;
  GInputStream *in;
  GOutputStream *out;
  GChecksum *sum;
  guint64 file_received;
  GBytes *piece;                /* being written */
};

enum { SIGNAL_PROGRESS, SIGNAL_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (DictateDownload, dictate_download, G_TYPE_OBJECT)

static const DictatePack *
pack_at (DictateDownload *self, guint i)
{
  return self->packs->pdata[i];
}

/* a pack's files come into a directory of their own: the download's, then the pack's place in it */
static char *
pack_dir (const char *dir, guint i)
{
  g_autofree char *name = g_strdup_printf ("%u", i);

  return g_build_filename (dir, name, NULL);
}

static void
drop_file (DictateDownload *self)
{
  g_clear_object (&self->message);
  g_clear_object (&self->in);
  /* a file's stream closes as it goes: the .part file is removed with the directory */
  g_clear_object (&self->out);
  g_clear_pointer (&self->sum, g_checksum_free);
  g_clear_pointer (&self->piece, g_bytes_unref);
}

static void
emit_progress (DictateDownload *self, gboolean now)
{
  gint64 time = g_get_monotonic_time ();

  if (!now && time - self->progressed < PROGRESS_US)
    return;
  self->progressed = time;
  g_signal_emit (self, signals[SIGNAL_PROGRESS], 0);
}

/* --- the end: on a thread, the download's directory goes (and any download left from a process that is gone) */

typedef struct {
  char *root, *dir;
} Sweep;

static void
sweep_free (Sweep *sweep)
{
  g_free (sweep->root);
  g_free (sweep->dir);
  g_free (sweep);
}

/* the downloads' directories are NAME-PID-XXXXXX: those of a process that is no more */
static gboolean
left_over (const char *name)
{
  g_auto (GStrv) parts = g_strsplit (name, "-", -1);
  guint n = g_strv_length (parts);
  guint64 pid;

  if (n < 3 || !g_ascii_string_to_unsigned (parts[n - 2], 10, 1, G_MAXINT, &pid, NULL))
    return FALSE;
  return kill ((pid_t) pid, 0) != 0 && errno == ESRCH;
}

static void
sweep_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Sweep *sweep = data;
  g_autofree char *downloads = g_build_filename (sweep->root, ".downloads", NULL);
  g_autoptr (GDir) list = NULL;
  g_autoptr (GError) error = NULL;
  const char *name;

  if (sweep->dir && !dictate_download_remove_tree (sweep->dir, &error))
    g_warning ("download: %s: %s", sweep->dir, error->message);
  list = g_dir_open (downloads, 0, NULL);
  while (list && (name = g_dir_read_name (list)))
    if (left_over (name))
      {
        g_autofree char *path = g_build_filename (downloads, name, NULL);
        g_autoptr (GError) left = NULL;

        g_debug ("download: removing %s, left by a process that is gone", path);
        if (!dictate_download_remove_tree (path, &left))
          g_warning ("download: %s: %s", path, left->message);
      }
  g_task_return_boolean (task, TRUE);
}

static void
on_swept (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = DICTATE_DOWNLOAD (source);

  g_debug ("download: %s", self->state == DICTATE_DOWNLOAD_DONE ? "done"
                           : self->state == DICTATE_DOWNLOAD_CANCELLED ? "cancelled" : self->error);
  g_signal_emit (self, signals[SIGNAL_FINISHED], 0);
}

/* the download is over (its state set): its directory goes, then "finished" */
static void
finish (DictateDownload *self)
{
  GTask *task = g_task_new (self, NULL, on_swept, NULL);
  Sweep *sweep = g_new0 (Sweep, 1);

  drop_file (self);
  g_clear_object (&self->session);
  sweep->root = g_strdup (self->root);
  sweep->dir = g_strdup (self->dir);
  g_task_set_task_data (task, sweep, (GDestroyNotify) sweep_free);
  g_task_run_in_thread (task, sweep_thread);
  g_object_unref (task);
}

static void
fail (DictateDownload *self, char *message)
{
  if (self->state != DICTATE_DOWNLOAD_FETCHING && self->state != DICTATE_DOWNLOAD_INSTALLING)
    {
      g_free (message);
      return;
    }
  /* called between operations: nothing is going on */
  self->state = DICTATE_DOWNLOAD_FAILED;
  self->error = message;
  g_warning ("download: %s", message);
  finish (self);
}

/* after an operation, with its error: TRUE when the download goes no further. While fetching, one operation is
 * going on at a time: a cancel is seen when it ends, and the download finishes then, with nothing more being written
 * into its directory. */
static gboolean
stopped (DictateDownload *self, GError *error)
{
  if (self->state == DICTATE_DOWNLOAD_CANCELLED)
    {
      finish (self);
      return TRUE;
    }
  if (self->state != DICTATE_DOWNLOAD_FETCHING)
    return TRUE;
  if (!error)
    return FALSE;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->state = DICTATE_DOWNLOAD_CANCELLED;
      finish (self);
    }
  else
    /* TRANSLATORS: %s is what went wrong, from the network or the file system */
    fail (self, g_strdup_printf (_("The download failed: %s"), error->message));
  return TRUE;
}

/* --- installing, on a thread: archives unpacked, then everything moved to its place */

typedef struct {
  GPtrArray *packs;
  char *root, *dir;
} Install;

static void
install_free (Install *install)
{
  g_ptr_array_unref (install->packs);
  g_free (install->root);
  g_free (install->dir);
  g_free (install);
}

/* a model already in its place, whole (another window's download, say): it stays */
static gboolean
model_whole (const DictatePack *pack, const char *dest)
{
  static const char *const VOSK_FILES[] = { "am/final.mdl", "conf/model.conf", NULL };

  if (pack->archive_top)
    {
      for (guint i = 0; VOSK_FILES[i]; i++)
        {
          g_autofree char *path = g_build_filename (dest, VOSK_FILES[i], NULL);
          if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
            return FALSE;
        }
      return TRUE;
    }
  for (const DictateFile *f = pack->files; f->name; f++)
    {
      g_autofree char *path = g_build_filename (dest, f->name, NULL);
      struct stat st;

      if (stat (path, &st) != 0 || !S_ISREG (st.st_mode) || (guint64) st.st_size != f->size)
        return FALSE;
    }
  return TRUE;
}

static gboolean
install_error (GError **error, const char *path)
{
  int saved = errno;

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), "%s: %s", path, g_strerror (saved));
  return FALSE;
}

/* a model's directory into its place: what is there already, not a whole model, is moved aside into the download's
 * directory, and goes with it */
static gboolean
install_model (const DictatePack *pack, const char *from, const char *dest, const char *aside, GError **error)
{
  g_autofree char *parent = g_path_get_dirname (dest);

  if (g_mkdir_with_parents (parent, 0755) != 0)
    return install_error (error, parent);
  if (g_file_test (dest, G_FILE_TEST_EXISTS) || g_file_test (dest, G_FILE_TEST_IS_SYMLINK))
    {
      if (model_whole (pack, dest))
        return TRUE;
      if (rename (dest, aside) != 0)
        return install_error (error, dest);
    }
  if (rename (from, dest) != 0)
    return install_error (error, dest);
  return TRUE;
}

/* a voice's files into its language's directory, the model last: a voice is found by its model and config, so it is
 * there once both are */
static gboolean
install_voice (const DictatePack *pack, const char *from, const char *dest, GError **error)
{
  if (g_mkdir_with_parents (dest, 0755) != 0)
    return install_error (error, dest);
  for (guint last = 0; last < 2; last++)
    for (const DictateFile *f = pack->files; f->name; f++)
      if (g_str_has_suffix (f->name, ".onnx") == (last == 1))
        {
          g_autofree char *source = g_build_filename (from, f->name, NULL);
          g_autofree char *target = g_build_filename (dest, f->name, NULL);

          if (rename (source, target) != 0)
            return install_error (error, target);
        }
  return TRUE;
}

static void
install_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Install *install = data;
  GError *error = NULL;

  /* the archives first: until they are all unpacked, nothing has moved, and it can still stop */
  for (guint i = 0; i < install->packs->len; i++)
    {
      const DictatePack *pack = install->packs->pdata[i];
      g_autofree char *from = pack_dir (install->dir, i);
      g_autofree char *archive = NULL, *into = NULL;

      if (!pack->archive_top)
        continue;
      archive = g_build_filename (from, pack->files[0].name, NULL);
      into = g_build_filename (from, "unpacked", NULL);
      if (g_mkdir (into, 0700) != 0)
        {
          install_error (&error, into);
          goto out;
        }
      if (!dictate_download_unpack (archive, into, pack->archive_top, pack->files[0].size * UNPACKED_TIMES,
                                    cancellable, &error))
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_prefix_error (&error, "%s: ", pack->files[0].name);
          goto out;
        }
      g_debug ("download: unpacked %s", archive);
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, &error))
    goto out;
  for (guint i = 0; i < install->packs->len; i++)
    {
      const DictatePack *pack = install->packs->pdata[i];
      g_autofree char *from = pack_dir (install->dir, i);
      g_autofree char *dest = g_build_filename (install->root, pack->place, NULL);
      g_autofree char *aside = g_strconcat (from, ".old", NULL);
      gboolean ok;

      if (pack->kind == DICTATE_PACK_VOICE)
        ok = install_voice (pack, from, dest, &error);
      else if (pack->archive_top)
        {
          g_autofree char *top = g_build_filename (from, "unpacked", pack->archive_top, NULL);
          ok = install_model (pack, top, dest, aside, &error);
        }
      else
        ok = install_model (pack, from, dest, aside, &error);
      if (!ok)
        goto out;
      g_debug ("download: %s in its place", dest);
    }

out:
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
on_installed (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = DICTATE_DOWNLOAD (source);
  g_autoptr (GError) error = NULL;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    {
      self->state = DICTATE_DOWNLOAD_DONE;
      finish (self);
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      self->state = DICTATE_DOWNLOAD_CANCELLED;
      finish (self);
    }
  else
    /* TRANSLATORS: %s is what went wrong */
    fail (self, g_strdup_printf (_("The files could not be put in place: %s"), error->message));
}

static void
install (DictateDownload *self)
{
  GTask *task = g_task_new (self, self->cancellable, on_installed, NULL);
  Install *install = g_new0 (Install, 1);

  self->state = DICTATE_DOWNLOAD_INSTALLING;
  emit_progress (self, TRUE);
  install->packs = g_ptr_array_ref (self->packs);
  install->root = g_strdup (self->root);
  install->dir = g_strdup (self->dir);
  /* the thread's answer, also when a cancel came too late to stop it */
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, install, (GDestroyNotify) install_free);
  g_task_run_in_thread (task, install_thread);
  g_object_unref (task);
}

/* --- fetching, a file at a time */

static void next_file (DictateDownload *self);
static void read_piece (DictateDownload *self);

static const DictateFile *
current_file (DictateDownload *self)
{
  return &pack_at (self, self->pack)->files[self->file];
}

static char *
current_path (DictateDownload *self, const char *suffix)
{
  g_autofree char *dir = pack_dir (self->dir, self->pack);
  g_autofree char *name = g_strconcat (current_file (self)->name, suffix, NULL);

  return g_build_filename (dir, name, NULL);
}

static void
on_closed (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = data;
  g_autoptr (GError) error = NULL;
  const DictateFile *file;
  g_autofree char *part = NULL, *done = NULL;

  g_output_stream_close_finish (G_OUTPUT_STREAM (source), result, &error);
  if (stopped (self, error))
    {
      g_object_unref (self);
      return;
    }
  file = current_file (self);
  if (self->file_received != file->size)
    {
      /* TRANSLATORS: %s is a file's name */
      fail (self, g_strdup_printf (_("%s did not come whole"), file->name));
      g_object_unref (self);
      return;
    }
  if (g_ascii_strcasecmp (g_checksum_get_string (self->sum), file->sha256) != 0)
    {
      g_warning ("download: %s: SHA-256 %s, not %s", file->name, g_checksum_get_string (self->sum), file->sha256);
      /* TRANSLATORS: %s is a file's name */
      fail (self, g_strdup_printf (_("%s is not the file it should be: its checksum does not match"), file->name));
      g_object_unref (self);
      return;
    }
  part = current_path (self, ".part");
  done = current_path (self, "");
  if (g_rename (part, done) != 0)
    {
      int saved = errno;
      fail (self, g_strdup_printf (_("The download failed: %s"), g_strerror (saved)));
      g_object_unref (self);
      return;
    }
  g_debug ("download: %s: %" G_GUINT64_FORMAT " bytes, SHA-256 %s as pinned", done, self->file_received,
           file->sha256);
  drop_file (self);
  self->file++;
  next_file (self);
  g_object_unref (self);
}

static void
on_written (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = data;
  g_autoptr (GError) error = NULL;

  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), result, NULL, &error);
  g_clear_pointer (&self->piece, g_bytes_unref);
  if (!stopped (self, error))
    read_piece (self);
  g_object_unref (self);
}

static void
on_read (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) piece = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);
  const DictateFile *file;
  gsize length;
  const guchar *bytes;

  if (stopped (self, error))
    {
      g_object_unref (self);
      return;
    }
  file = current_file (self);
  bytes = g_bytes_get_data (piece, &length);
  if (length == 0)
    {
      /* the end: closed, then checked */
      g_output_stream_close_async (self->out, G_PRIORITY_DEFAULT, self->cancellable, on_closed, self);
      return;
    }
  if (self->file_received + length > file->size)
    {
      /* TRANSLATORS: %s is a file's name */
      fail (self, g_strdup_printf (_("%s is larger than it should be"), file->name));
      g_object_unref (self);
      return;
    }
  g_checksum_update (self->sum, bytes, length);
  self->file_received += length;
  self->received += length;
  emit_progress (self, FALSE);
  self->piece = g_steal_pointer (&piece);
  g_output_stream_write_all_async (self->out, bytes, length, G_PRIORITY_DEFAULT, self->cancellable, on_written,
                                   self);
}

static void
read_piece (DictateDownload *self)
{
  g_input_stream_read_bytes_async (self->in, PIECE, G_PRIORITY_DEFAULT, self->cancellable, on_read,
                                   g_object_ref (self));
}

static void
on_created (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = data;
  g_autoptr (GError) error = NULL;
  GFileOutputStream *out = g_file_create_finish (G_FILE (source), result, &error);

  if (!stopped (self, error))
    {
      self->out = G_OUTPUT_STREAM (out);
      read_piece (self);
    }
  else
    g_clear_object (&out);
  g_object_unref (self);
}

static void
on_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  DictateDownload *self = data;
  g_autoptr (GError) error = NULL;
  GInputStream *in = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  const DictateFile *file;
  guint status;
  GUri *uri;
  goffset length;

  if (stopped (self, error))
    {
      g_clear_object (&in);
      g_object_unref (self);
      return;
    }
  self->in = in;
  file = current_file (self);
  status = soup_message_get_status (self->message);
  uri = soup_message_get_uri (self->message);
  length = soup_message_headers_get_content_length (soup_message_get_response_headers (self->message));
  if (status != SOUP_STATUS_OK)
    /* TRANSLATORS: %s is a file's name, %u and the last %s the server's answer (404, Not Found) */
    fail (self, g_strdup_printf (_("The server did not give %s: %u %s"), file->name, status,
                                 soup_message_get_reason_phrase (self->message)));
  /* redirected, as Hugging Face's files are: still HTTPS (libsoup checks the certificate) */
  else if (g_strcmp0 (g_uri_get_scheme (uri), "https") != 0)
    fail (self, g_strdup_printf (_("The server did not give %s: %u %s"), file->name, status, "not HTTPS"));
  else if (length > 0 && (guint64) length != file->size)
    /* TRANSLATORS: %s is a file's name */
    fail (self, g_strdup_printf (_("%s is not the size it should be: it has changed on the server"), file->name));
  else
    {
      g_autofree char *part = current_path (self, ".part");
      g_autoptr (GFile) target = g_file_new_for_path (part);

      g_debug ("download: %s from %s", file->name, g_uri_get_host (uri));
      g_file_create_async (target, G_FILE_CREATE_PRIVATE, G_PRIORITY_DEFAULT, self->cancellable, on_created,
                           g_object_ref (self));
    }
  g_object_unref (self);
}

static void
next_file (DictateDownload *self)
{
  const DictatePack *pack;
  g_autofree char *url = NULL, *dir = NULL;

  if (self->state != DICTATE_DOWNLOAD_FETCHING)
    return;
  while (self->pack < self->packs->len && !pack_at (self, self->pack)->files[self->file].name)
    self->pack++, self->file = 0;
  if (self->pack == self->packs->len)
    {
      install (self);
      return;
    }
  pack = pack_at (self, self->pack);
  dir = pack_dir (self->dir, self->pack);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int saved = errno;
      fail (self, g_strdup_printf (_("The download failed: %s"), g_strerror (saved)));
      return;
    }
  url = g_strconcat (pack->source, "/", current_file (self)->name, NULL);
  self->message = soup_message_new (SOUP_METHOD_GET, url);
  if (!self->message)
    {
      fail (self, g_strdup_printf (_("The download failed: %s"), url));
      return;
    }
  /* the bytes as the server has them, not unpacked on the way: their size and sum are pinned */
  soup_message_headers_replace (soup_message_get_request_headers (self->message), "Accept-Encoding", "identity");
  self->sum = g_checksum_new (G_CHECKSUM_SHA256);
  self->file_received = 0;
  emit_progress (self, TRUE);
  soup_session_send_async (self->session, self->message, G_PRIORITY_DEFAULT, self->cancellable, on_sent,
                           g_object_ref (self));
}

/* --- the object */

guint64
dictate_download_free_space (void)
{
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileInfo) info = NULL;

  g_mkdir_with_parents (root, 0700);
  file = g_file_new_for_path (root);
  info = g_file_query_filesystem_info (file, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
  return info ? g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) : G_MAXUINT64;
}

void
dictate_download_start (DictateDownload *self)
{
  g_autofree char *downloads = NULL, *template = NULL;
  guint64 needed = 0, free_space;

  g_return_if_fail (self->state == DICTATE_DOWNLOAD_WAITING);
  self->state = DICTATE_DOWNLOAD_FETCHING;
  /* the files, and what the archives unpack to beside them */
  for (guint i = 0; i < self->packs->len; i++)
    needed += dictate_pack_size (pack_at (self, i)) * (pack_at (self, i)->archive_top ? 3 : 1);
  free_space = dictate_download_free_space ();
  if (free_space < needed)
    {
      g_autofree char *need = g_format_size (needed), *have = g_format_size (free_space);
      /* TRANSLATORS: the %s are sizes: 90.4 MB */
      fail (self, g_strdup_printf (_("There is not enough space on the phone: %s are needed, %s are free"),
                                   need, have));
      return;
    }
  downloads = g_build_filename (self->root, ".downloads", NULL);
  template = g_strdup_printf ("%s/%s-%d-XXXXXX", downloads, pack_at (self, 0)->language, (int) getpid ());
  if (g_mkdir_with_parents (downloads, 0700) != 0 || !g_mkdtemp_full (template, 0700))
    {
      int saved = errno;
      fail (self, g_strdup_printf (_("The download failed: %s"), g_strerror (saved)));
      return;
    }
  self->dir = g_steal_pointer (&template);
  self->session = soup_session_new_with_options ("timeout", TIMEOUT_S, "idle-timeout", TIMEOUT_S,
                                                 "user-agent", "vibeos-dictate", NULL);
  soup_session_remove_feature_by_type (self->session, SOUP_TYPE_CONTENT_DECODER);
  g_debug ("download: into %s, %" G_GUINT64_FORMAT " bytes", self->dir, self->size);
  next_file (self);
}

void
dictate_download_cancel (DictateDownload *self)
{
  /* installing has gone too far to stop once the archives are unpacked: the thread checks */
  if (self->state == DICTATE_DOWNLOAD_INSTALLING)
    {
      g_cancellable_cancel (self->cancellable);
      return;
    }
  if (self->state == DICTATE_DOWNLOAD_WAITING)
    {
      self->state = DICTATE_DOWNLOAD_CANCELLED;
      return;
    }
  if (self->state != DICTATE_DOWNLOAD_FETCHING)
    return;
  self->state = DICTATE_DOWNLOAD_CANCELLED;
  /* the operation going on ends with its "cancelled", and the download finishes then (stopped ()) */
  g_cancellable_cancel (self->cancellable);
}

DictateDownloadState
dictate_download_get_state (DictateDownload *self)
{
  return self->state;
}

guint64
dictate_download_get_size (DictateDownload *self)
{
  return self->size;
}

guint64
dictate_download_get_received (DictateDownload *self)
{
  return self->received;
}

const DictatePack *
dictate_download_get_pack (DictateDownload *self)
{
  return pack_at (self, MIN (self->pack, self->packs->len - 1));
}

const char *
dictate_download_get_error (DictateDownload *self)
{
  return self->error;
}

DictateDownload *
dictate_download_new (const DictatePack *const *packs, guint n)
{
  DictateDownload *self = g_object_new (DICTATE_TYPE_DOWNLOAD, NULL);

  g_return_val_if_fail (n > 0, self);
  for (guint i = 0; i < n; i++)
    {
      g_ptr_array_add (self->packs, (gpointer) packs[i]);
      self->size += dictate_pack_size (packs[i]);
    }
  return self;
}

static void
dictate_download_finalize (GObject *object)
{
  DictateDownload *self = DICTATE_DOWNLOAD (object);

  drop_file (self);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  g_ptr_array_unref (self->packs);
  g_free (self->error);
  g_free (self->root);
  g_free (self->dir);
  G_OBJECT_CLASS (dictate_download_parent_class)->finalize (object);
}

static void
dictate_download_class_init (DictateDownloadClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dictate_download_finalize;
  signals[SIGNAL_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 0);
}

static void
dictate_download_init (DictateDownload *self)
{
  self->packs = g_ptr_array_new ();
  self->cancellable = g_cancellable_new ();
  self->root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
}

/* ---------------- what was downloaded ---------------- */

/* the bytes of `name` in `dir` and all in it, links not followed (nor counted) */
static guint64
size_at (int dir, const char *name)
{
  struct stat st;
  guint64 size = 0;

  if (fstatat (dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return 0;
  if (S_ISREG (st.st_mode))
    return st.st_size;
  if (S_ISDIR (st.st_mode))
    {
      int fd = openat (dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      DIR *list = fd >= 0 ? fdopendir (fd) : NULL;
      struct dirent *entry;

      if (!list)
        {
          if (fd >= 0)
            close (fd);
          return 0;
        }
      while ((entry = readdir (list)))
        if (strcmp (entry->d_name, ".") != 0 && strcmp (entry->d_name, "..") != 0)
          size += size_at (dirfd (list), entry->d_name);
      closedir (list);
    }
  return size;
}

guint64
dictate_download_downloaded_size (const char *language)
{
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
  guint64 size = 0;

  for (int kind = DICTATE_PACK_MODEL; kind <= DICTATE_PACK_VOICE; kind++)
    {
      const DictatePack *pack = dictate_pack_find (language, kind);
      g_autofree char *dest = pack ? g_build_filename (root, pack->place, NULL) : NULL;

      if (!pack)
        continue;
      if (kind == DICTATE_PACK_MODEL)
        size += size_at (AT_FDCWD, dest);
      else
        /* a voice's directory may hold others the user put there: only the pack's files are its */
        for (const DictateFile *f = pack->files; f->name; f++)
          {
            g_autofree char *path = g_build_filename (dest, f->name, NULL);
            size += size_at (AT_FDCWD, path);
          }
    }
  return size;
}

gboolean
dictate_download_remove (const char *language, GError **error)
{
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
  const DictatePack *model = dictate_pack_find (language, DICTATE_PACK_MODEL);
  const DictatePack *voice = dictate_pack_find (language, DICTATE_PACK_VOICE);

  if (model)
    {
      g_autofree char *dest = g_build_filename (root, model->place, NULL);
      g_autofree char *downloads = g_build_filename (root, ".downloads", NULL);
      g_autofree char *aside = g_strdup_printf ("%s/%s-%d-XXXXXX", downloads, language, (int) getpid ());
      g_autofree char *moved = NULL;
      struct stat st;

      /* out of where it is found first, in one rename: a removal cut short leaves no half model there (and what
       * is left in .downloads goes with the next download's sweep) */
      if (lstat (dest, &st) == 0)
        {
          if (g_mkdir_with_parents (downloads, 0700) != 0 || !g_mkdtemp_full (aside, 0700))
            return install_error (error, downloads);
          moved = g_build_filename (aside, "model", NULL);
          if (rename (dest, moved) != 0)
            return install_error (error, dest);
          g_debug ("download: %s removed", dest);
          if (!dictate_download_remove_tree (aside, error))
            return FALSE;
        }
    }
  if (voice)
    {
      g_autofree char *dest = g_build_filename (root, voice->place, NULL);

      /* the model first: without it the voice is not found */
      for (guint last = 0; last < 2; last++)
        for (const DictateFile *f = voice->files; f->name; f++)
          if (g_str_has_suffix (f->name, ".onnx") == (last == 0))
            {
              g_autofree char *path = g_build_filename (dest, f->name, NULL);

              if (unlink (path) != 0 && errno != ENOENT)
                return install_error (error, path);
            }
      /* its language's directory, if nothing else is in it */
      if (rmdir (dest) == 0)
        g_debug ("download: %s removed", dest);
    }
  return TRUE;
}
