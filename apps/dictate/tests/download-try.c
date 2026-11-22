/* download-try: dictate-download.c's packs fetched for real, into the user's data directory (XDG_DATA_HOME to put
 * them elsewhere), as the app fetches them; not run by meson test, it needs the network.
 *
 *   download-try [--voice-only | --model-only] [--bad-sum] [--cancel-after BYTES] LANGUAGE…
 *
 * For each LANGUAGE, its speech model's pack and its voice's, in one download. --bad-sum changes the last file's
 * pinned sum, so the download must fail at the end with nothing left; --cancel-after cancels it once that many bytes
 * have come. Afterwards: what is in vibeos-dictate (models, voices, and .downloads, which must be empty). The exit
 * status is 0 when the download ended as asked (done, failed for --bad-sum, cancelled for --cancel-after) and left
 * the directories so. */
#include "dictate-download.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <stdio.h>
#include <string.h>

typedef struct {
  GMainLoop *loop;
  DictateDownload *download;
  guint64 cancel_after;
  gint64 started, shown;
} Try;

static void
on_progress (DictateDownload *download, Try *t)
{
  gint64 now = g_get_monotonic_time ();
  guint64 received = dictate_download_get_received (download), size = dictate_download_get_size (download);

  if (now - t->shown >= G_USEC_PER_SEC || dictate_download_get_state (download) != DICTATE_DOWNLOAD_FETCHING)
    {
      t->shown = now;
      printf ("  %5.1f s  %3d%%  %" G_GUINT64_FORMAT " of %" G_GUINT64_FORMAT " bytes  %s%s\n",
              (now - t->started) / 1e6, size ? (int) (received * 100 / size) : 0, received, size,
              dictate_download_get_pack (download)->title,
              dictate_download_get_state (download) == DICTATE_DOWNLOAD_INSTALLING ? " (installing)" : "");
      fflush (stdout);
    }
  if (t->cancel_after && received >= t->cancel_after)
    {
      printf ("  cancelling at %" G_GUINT64_FORMAT " bytes\n", received);
      t->cancel_after = 0;
      dictate_download_cancel (download);
    }
}

static void
on_finished (DictateDownload *download, Try *t)
{
  g_main_loop_quit (t->loop);
}

/* every file under `dir`, with its size */
static void
show_tree (const char *dir, const char *indent, guint *count)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      g_autofree char *path = g_build_filename (dir, names->pdata[i], NULL);
      g_autofree char *deeper = g_strconcat (indent, "  ", NULL);
      GStatBuf st;

      (*count)++;
      if (g_lstat (path, &st) == 0 && S_ISDIR (st.st_mode))
        {
          printf ("%s%s/\n", indent, (char *) names->pdata[i]);
          if (strlen (indent) < 4)
            show_tree (path, deeper, count);
        }
      else
        printf ("%s%s  %" G_GUINT64_FORMAT "\n", indent, (char *) names->pdata[i], (guint64) st.st_size);
    }
}

int
main (int argc, char **argv)
{
  gboolean voice_only = FALSE, model_only = FALSE, bad_sum = FALSE;
  gint64 cancel_after = 0;
  GOptionEntry options[] = {
    { "voice-only", 0, 0, G_OPTION_ARG_NONE, &voice_only, "Only the voices", NULL },
    { "model-only", 0, 0, G_OPTION_ARG_NONE, &model_only, "Only the speech models", NULL },
    { "bad-sum", 0, 0, G_OPTION_ARG_NONE, &bad_sum, "Pin a wrong sum for the last file", NULL },
    { "cancel-after", 0, 0, G_OPTION_ARG_INT64, &cancel_after, "Cancel after BYTES", "BYTES" },
    { NULL }
  };
  g_autoptr (GOptionContext) context = g_option_context_new ("LANGUAGE…");
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) packs = g_ptr_array_new ();
  g_autofree char *root = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
  g_autofree char *downloads = g_build_filename (root, ".downloads", NULL);
  DictatePack *changed = NULL;
  DictateFile *files = NULL;
  char *changed_sum = NULL;
  DictateDownloadState state, expected;
  Try t = { 0 };
  guint left = 0, count = 0;

  g_option_context_add_main_entries (context, options, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error) || argc < 2)
    {
      fprintf (stderr, "%s\n", error ? error->message : "usage: download-try [OPTION…] LANGUAGE…");
      return 2;
    }
  for (int i = 1; i < argc; i++)
    for (int kind = 0; kind < 2; kind++)
      {
        const DictatePack *pack;

        if ((kind == DICTATE_PACK_MODEL && voice_only) || (kind == DICTATE_PACK_VOICE && model_only))
          continue;
        pack = dictate_pack_find (argv[i], kind);
        if (!pack)
          {
            fprintf (stderr, "no pack for %s\n", argv[i]);
            return 2;
          }
        g_ptr_array_add (packs, (gpointer) pack);
      }
  if (bad_sum)
    {
      /* the last pack, its last file's sum with its last digit changed */
      const DictatePack *last = packs->pdata[packs->len - 1];
      guint n = 0;

      while (last->files[n].name)
        n++;
      files = g_new0 (DictateFile, n + 1);
      memcpy (files, last->files, n * sizeof (DictateFile));
      changed_sum = g_strdup (files[n - 1].sha256);
      changed_sum[63] = changed_sum[63] == '0' ? '1' : '0';
      files[n - 1].sha256 = changed_sum;
      printf ("pinned %s as %s\n", files[n - 1].name, changed_sum);
      changed = g_memdup2 (last, sizeof *last);
      changed->files = files;
      packs->pdata[packs->len - 1] = changed;
    }

  printf ("into %s\n", root);
  t.loop = g_main_loop_new (NULL, FALSE);
  t.cancel_after = cancel_after;
  t.download = dictate_download_new ((const DictatePack *const *) packs->pdata, packs->len);
  g_signal_connect (t.download, "progress", G_CALLBACK (on_progress), &t);
  g_signal_connect (t.download, "finished", G_CALLBACK (on_finished), &t);
  t.started = g_get_monotonic_time ();
  dictate_download_start (t.download);
  if (dictate_download_get_state (t.download) == DICTATE_DOWNLOAD_FETCHING)
    g_main_loop_run (t.loop);
  state = dictate_download_get_state (t.download);
  printf ("ended: %s after %.1f s%s%s\n",
          state == DICTATE_DOWNLOAD_DONE ? "done" : state == DICTATE_DOWNLOAD_CANCELLED ? "cancelled" : "failed",
          (g_get_monotonic_time () - t.started) / 1e6, dictate_download_get_error (t.download) ? ": " : "",
          dictate_download_get_error (t.download) ? dictate_download_get_error (t.download) : "");

  printf ("%s:\n", root);
  show_tree (root, "  ", &count);
  {
    g_autoptr (GDir) d = g_dir_open (downloads, 0, NULL);
    while (d && g_dir_read_name (d))
      left++;
  }
  printf ("left in .downloads: %u\n", left);
  expected = bad_sum ? DICTATE_DOWNLOAD_FAILED : cancel_after ? DICTATE_DOWNLOAD_CANCELLED : DICTATE_DOWNLOAD_DONE;
  if (state == DICTATE_DOWNLOAD_DONE)
    for (guint i = 0; i < packs->len; i++)
      {
        const DictatePack *pack = packs->pdata[i];

        if (pack->kind == DICTATE_PACK_MODEL)
          {
            g_autoptr (DictateModel) model = dictate_model_find (pack->language);
            printf ("model for %s: %s\n", pack->language, model ? model->dir : "NONE");
            if (!model || !g_str_has_prefix (model->dir, root))
              state = DICTATE_DOWNLOAD_FAILED;
          }
      }
  g_clear_object (&t.download);
  g_main_loop_unref (t.loop);
  g_free (changed_sum);
  g_free (files);
  g_free (changed);
  return state == expected && left == 0 ? 0 : 1;
}
