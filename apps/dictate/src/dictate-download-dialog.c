/* dictate-download-dialog: see dictate-download-dialog.h.
 *
 * One Offer goes through the dialogs: asked, downloading, failed (and retried). Each dialog holds a reference to it
 * until it is gone; the progress dialog going (it is answered, or the window closes) stops its download. */
#include "dictate-download-dialog.h"
#include "dictate-download.h"
#include "dictate-models.h"
#include "dictate-recogniser.h"

#include <glib/gi18n.h>
#include <string.h>
#include <vibeos.h>

/* what a language lacks */
typedef struct {
  const DictatePack *packs[2];
  guint n;
  const DictateRecogniserClass *library;        /* its recogniser, whose library is not installed */
  gboolean nothing;                             /* something it lacks has no download */
} Needs;

static void
find_needs (const char *language, DictateTalk *talk, Needs *needs)
{
  g_autoptr (DictateModel) model = dictate_model_find (language);
  const DictatePack *model_pack = dictate_pack_find (language, DICTATE_PACK_MODEL);
  const DictateRecogniserClass *recogniser;

  memset (needs, 0, sizeof *needs);
  recogniser = dictate_recogniser_class (model ? model->kind : model_pack ? model_pack->recogniser
                                                                          : DICTATE_MOONSHINE);
  /* code only from its package: without it, nothing is worth downloading */
  if (!recogniser->installed ())
    {
      needs->library = recogniser;
      return;
    }
  if (!model)
    {
      if (model_pack)
        needs->packs[needs->n++] = model_pack;
      else
        needs->nothing = TRUE;
    }
  if (talk && !dictate_talk_has_voice (talk, language))
    {
      const DictatePack *voice = dictate_pack_find (language, DICTATE_PACK_VOICE);

      if (voice)
        needs->packs[needs->n++] = voice;
      else
        needs->nothing = TRUE;
    }
}

static guint64
needs_size (const Needs *needs)
{
  guint64 size = 0;

  for (guint i = 0; i < needs->n; i++)
    size += dictate_pack_size (needs->packs[i]);
  return size;
}

static const char *
language_name (const char *language)
{
  const DictateLanguage *known = dictate_language_lookup (language);

  return known ? known->name : language;
}

char *
dictate_download_note (const char *language, DictateTalk *talk)
{
  Needs needs;

  find_needs (language, talk, &needs);
  if (needs.library)
    /* TRANSLATORS: under a language in the list; %s is a speech recogniser (Vosk) */
    return g_strdup_printf (_("Needs %s, which is not installed"), needs.library->name);
  if (needs.nothing)
    return g_strdup (_("Not available on this phone"));
  if (needs.n)
    {
      g_autofree char *size = g_format_size (needs_size (&needs));
      /* TRANSLATORS: under a language in the list; %s is a size: 90.4 MB */
      return g_strdup_printf (_("%s to download"), size);
    }
  return NULL;
}

/* ---------------- the offer ---------------- */

typedef struct {
  GtkWidget *window;            /* weak */
  char *language;
  DictateTalk *talk;
  Needs needs;
  DictateLanguageReady ready;
  gpointer data;
  DictateDownload *download;
  VibeosDialog *progress;       /* weak */
  GtkWidget *status, *percent, *bar, *sizes;
} Offer;

static void
offer_clear (Offer *offer)
{
  if (offer->download)
    {
      g_signal_handlers_disconnect_by_data (offer->download, offer);
      dictate_download_cancel (offer->download);
      g_clear_object (&offer->download);
    }
  g_clear_weak_pointer (&offer->window);
  g_clear_weak_pointer (&offer->progress);
  g_clear_object (&offer->talk);
  g_free (offer->language);
}

static void
offer_release (Offer *offer)
{
  g_rc_box_release_full (offer, (GDestroyNotify) offer_clear);
}

/* `dialog` holds the offer while it is there */
static void
hold (VibeosDialog *dialog, Offer *offer, GDestroyNotify release)
{
  g_object_set_data_full (G_OBJECT (dialog), "dictate-offer", g_rc_box_acquire (offer), release);
}

static void start (Offer *offer);

/* a title, a detail under it, and a size at the end */
static GtkWidget *
item (const char *title, const char *detail, const char *size)
{
  GtkWidget *row = gtk_grid_new ();
  GtkWidget *name = gtk_label_new (title), *under = gtk_label_new (detail), *bytes = gtk_label_new (size);

  gtk_widget_add_css_class (row, "dictate-download-item");
  gtk_widget_add_css_class (name, "dictate-download-name");
  gtk_widget_add_css_class (under, "dictate-download-detail");
  gtk_widget_add_css_class (bytes, "dictate-download-size");
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_xalign (GTK_LABEL (under), 0);
  gtk_label_set_wrap (GTK_LABEL (under), TRUE);
  gtk_label_set_wrap (GTK_LABEL (name), TRUE);
  gtk_widget_set_hexpand (name, TRUE);
  gtk_widget_set_valign (bytes, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (row), name, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (row), bytes, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (row), under, 0, 1, 2, 1);
  return row;
}

static void
message_dialog (Offer *offer, const char *title, const char *message, const char *ok)
{
  VibeosDialog *dialog;

  if (!offer->window)
    return;
  dialog = vibeos_dialog_new (title);
  vibeos_dialog_set_message (dialog, message);
  vibeos_dialog_add_button (dialog, ok, VIBEOS_RESPONSE_OK);
  vibeos_dialog_present (dialog, offer->window);
}

/* --- failed */

static void
on_failed_response (VibeosDialog *dialog, int response, Offer *offer)
{
  if (response == VIBEOS_RESPONSE_OK)
    start (offer);
}

static void
show_failure (Offer *offer, const char *why)
{
  /* TRANSLATORS: a dialog's title; %s is the language's name for itself (Deutsch) */
  g_autofree char *title = g_strdup_printf (_("%s was not downloaded"), language_name (offer->language));
  /* TRANSLATORS: %s is what went wrong */
  g_autofree char *message = g_strdup_printf (_("%s. Nothing of it was kept on the phone."), why);
  VibeosDialog *dialog;

  if (!offer->window)
    return;
  dialog = vibeos_dialog_new (title);
  vibeos_dialog_set_message (dialog, message);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Retry"), VIBEOS_RESPONSE_OK);
  hold (dialog, offer, (GDestroyNotify) offer_release);
  g_signal_connect (dialog, "response", G_CALLBACK (on_failed_response), offer);
  vibeos_dialog_present (dialog, offer->window);
}

/* --- downloading */

static void
show_progress (DictateDownload *download, Offer *offer)
{
  guint64 size = dictate_download_get_size (download), received = dictate_download_get_received (download);
  const DictatePack *pack = dictate_download_get_pack (download);
  g_autofree char *percent = g_strdup_printf ("%d%%", size ? (int) (received * 100 / size) : 0);
  g_autofree char *got = g_format_size (received), *all = g_format_size (size);
  /* TRANSLATORS: the progress, two sizes: 12.3 MB of 90.4 MB */
  g_autofree char *sizes = g_strdup_printf (_("%s of %s"), got, all);
  const char *status;

  if (!offer->progress)
    return;
  if (dictate_download_get_state (download) == DICTATE_DOWNLOAD_INSTALLING)
    status = pack->archive_top ? _("Unpacking and installing…") : _("Installing…");
  else
    status = pack->kind == DICTATE_PACK_VOICE ? _("Voice") : _("Speech recognition");
  gtk_label_set_text (GTK_LABEL (offer->status), status);
  gtk_label_set_text (GTK_LABEL (offer->percent), percent);
  gtk_label_set_text (GTK_LABEL (offer->sizes), sizes);
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (offer->bar), size ? (double) received / size : 0);
}

static void
on_finished (DictateDownload *download, Offer *offer)
{
  DictateDownloadState state = dictate_download_get_state (download);
  /* the progress dialog goes, and with it its hold on the offer: this one keeps it for what follows */
  Offer *keep = g_rc_box_acquire (offer);
  Needs left;

  if (offer->progress)
    vibeos_dialog_respond (offer->progress, VIBEOS_RESPONSE_OK);
  if (state == DICTATE_DOWNLOAD_FAILED)
    show_failure (offer, dictate_download_get_error (download));
  else if (state == DICTATE_DOWNLOAD_DONE && offer->window)
    {
      find_needs (offer->language, offer->talk, &left);
      if (left.n || left.library || left.nothing)
        show_failure (offer, _("The files came, but the language is still not ready"));
      else
        offer->ready (offer->language, offer->data);
    }
  offer_release (keep);
}

static void
on_progress_response (VibeosDialog *dialog, int response, Offer *offer)
{
  /* Cancel, a tap outside or Back: the download stops, and what came of it goes */
  if (response != VIBEOS_RESPONSE_OK && offer->download)
    dictate_download_cancel (offer->download);
}

/* a progress dialog gone, answered or with its window: its download (not a retry's) stops if it has not finished */
static void
progress_gone (DictateDownload *download)
{
  dictate_download_cancel (download);
  g_object_unref (download);
}

static void
start (Offer *offer)
{
  /* TRANSLATORS: a dialog's title; %s is the language's name for itself (Deutsch) */
  g_autofree char *title = g_strdup_printf (_("Downloading %s"), language_name (offer->language));
  VibeosDialog *dialog;
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  if (!offer->window)
    return;
  if (offer->download)
    {
      g_signal_handlers_disconnect_by_data (offer->download, offer);
      g_clear_object (&offer->download);
    }
  dialog = vibeos_dialog_new (title);
  offer->status = gtk_label_new (NULL);
  offer->percent = gtk_label_new (NULL);
  offer->sizes = gtk_label_new (NULL);
  offer->bar = gtk_progress_bar_new ();
  gtk_widget_add_css_class (offer->status, "dictate-download-status");
  gtk_widget_add_css_class (offer->percent, "dictate-download-percent");
  gtk_widget_add_css_class (offer->sizes, "dictate-download-detail");
  gtk_widget_add_css_class (offer->bar, "dictate-progress");
  gtk_label_set_xalign (GTK_LABEL (offer->status), 0);
  gtk_label_set_xalign (GTK_LABEL (offer->sizes), 0);
  gtk_label_set_ellipsize (GTK_LABEL (offer->status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (offer->status, TRUE);
  gtk_box_append (GTK_BOX (head), offer->status);
  gtk_box_append (GTK_BOX (head), offer->percent);
  gtk_box_append (GTK_BOX (content), head);
  gtk_box_append (GTK_BOX (content), offer->bar);
  gtk_box_append (GTK_BOX (content), offer->sizes);
  vibeos_dialog_set_content (dialog, content);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  hold (dialog, offer, (GDestroyNotify) offer_release);
  g_set_weak_pointer (&offer->progress, dialog);
  g_signal_connect (dialog, "response", G_CALLBACK (on_progress_response), offer);

  offer->download = dictate_download_new (offer->needs.packs, offer->needs.n);
  g_object_set_data_full (G_OBJECT (dialog), "dictate-download", g_object_ref (offer->download),
                          (GDestroyNotify) progress_gone);
  g_signal_connect (offer->download, "progress", G_CALLBACK (show_progress), offer);
  g_signal_connect (offer->download, "finished", G_CALLBACK (on_finished), offer);
  show_progress (offer->download, offer);
  vibeos_dialog_present (dialog, offer->window);
  dictate_download_start (offer->download);
}

/* --- asked */

static void
on_offer_response (VibeosDialog *dialog, int response, Offer *offer)
{
  if (response == VIBEOS_RESPONSE_OK)
    start (offer);
}

static void
ask (Offer *offer)
{
  const char *name = language_name (offer->language);
  /* TRANSLATORS: a dialog's title; %s is the language's name for itself (Deutsch) */
  g_autofree char *title = g_strdup_printf (_("Download %s?"), name);
  g_autofree char *message = NULL, *total = g_format_size (needs_size (&offer->needs));
  /* TRANSLATORS: the sum of the sizes in the download dialog; %s is a size: 90.4 MB */
  g_autofree char *total_line = g_strdup_printf (_("%s in all"), total);
  VibeosDialog *dialog = vibeos_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *sum;

  if (offer->talk)
    /* TRANSLATORS: %s is the language's name for itself (Deutsch) */
    message = g_strdup_printf (_("To hear and speak %s, Agent needs these files on the phone:"), name);
  else
    /* TRANSLATORS: %s is the language's name for itself (Deutsch) */
    message = g_strdup_printf (_("To hear %s, Dictate needs these files on the phone:"), name);
  for (guint i = 0; i < offer->needs.n; i++)
    {
      const DictatePack *pack = offer->needs.packs[i];
      g_autofree char *host = dictate_pack_host (pack), *size = g_format_size (dictate_pack_size (pack));
      /* TRANSLATORS: under what is downloaded: what it is, its licence and where it comes from */
      g_autofree char *detail = g_strdup_printf (_("%s · %s · from %s"), pack->title, pack->licence, host);

      gtk_box_append (GTK_BOX (list), item (pack->kind == DICTATE_PACK_VOICE ? _("Voice") : _("Speech recognition"),
                                            detail, size));
    }
  sum = gtk_label_new (total_line);
  gtk_label_set_xalign (GTK_LABEL (sum), 1);
  gtk_widget_add_css_class (sum, "dictate-download-total");
  gtk_box_append (GTK_BOX (list), sum);
  gtk_widget_add_css_class (list, "dictate-download-list");
  vibeos_dialog_set_message (dialog, message);
  vibeos_dialog_set_content (dialog, list);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Download"), VIBEOS_RESPONSE_OK);
  hold (dialog, offer, (GDestroyNotify) offer_release);
  g_signal_connect (dialog, "response", G_CALLBACK (on_offer_response), offer);
  vibeos_dialog_present (dialog, offer->window);
}

void
dictate_download_ensure (GtkWidget *window, const char *language, DictateTalk *talk, DictateLanguageReady ready,
                         gpointer data)
{
  const char *name = language_name (language);
  Offer *offer;

  offer = g_rc_box_new0 (Offer);
  find_needs (language, talk, &offer->needs);
  g_set_weak_pointer (&offer->window, window);
  offer->language = g_strdup (language);
  offer->talk = talk ? g_object_ref (talk) : NULL;
  offer->ready = ready;
  offer->data = data;

  if (offer->needs.library)
    {
      /* TRANSLATORS: a dialog's title; %s is the language's name for itself (Français) */
      g_autofree char *title = g_strdup_printf (_("%s is not available"), name);
      /* TRANSLATORS: the first %s is the language's name for itself, the second a speech recogniser (Vosk), the
       * third its Debian package (vibeos-dictate-vosk) */
      g_autofree char *message = g_strdup_printf (_("%s is heard by %s, which is not installed on this phone. "
                                                    "Install the %s package, then choose %s again."),
                                                  name, offer->needs.library->name, offer->needs.library->package,
                                                  name);
      message_dialog (offer, title, message, _("OK"));
    }
  else if (offer->needs.nothing)
    {
      /* TRANSLATORS: a dialog's title; %s is the language's name for itself */
      g_autofree char *title = g_strdup_printf (_("%s is not available"), name);
      message_dialog (offer, title, _("There is nothing to download for it."), _("OK"));
    }
  else if (offer->needs.n == 0)
    ready (language, data);
  else
    ask (offer);
  offer_release (offer);
}
