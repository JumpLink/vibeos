/* The Dictate window: see dictate-window.h.
 *
 * The dictation point is the stretch between two marks: `from` (left gravity) and `to` (right gravity). The sentence
 * being spoken fills it, tagged "partial"; a finished sentence takes its place untagged, and both marks move past
 * it, so the next one follows it. Each finished sentence or update is one irreversible change: the undo history is
 * the user's typing, never half a sentence.
 *
 * The language listened in is the app bar's button, showing its code: the user's choice (GSettings neo.Dictate
 * language) while its model is installed, else the first of the user's languages with one, else English. */
#include "dictate-window.h"
#include "dictate-engine.h"
#include "dictate-mic.h"
#include "dictate-models.h"
#include "dictate-text.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#define FAB_ROW         120     /* the FAB's row at the bottom: the document's last line scrolls up out of it */
#define FAB_SIZE        88
#define SAVE_DELAY_MS   1000    /* after the last change, the document is written */

typedef struct {
  GtkWidget *window;
  GtkWidget *scroller, *view, *hint, *mic, *language, *copy, *clear;
  NeoSnackbar *snackbar;
  GSettings *settings;
  GtkTextBuffer *buffer;
  GtkTextMark *from, *to;
  GtkTextTag *partial;
  DictateEngine *engine;
  char *path;                   /* the document's file */
  guint save_id;
  guint inhibit;                /* the phone kept awake and its screen on while listening */
  char *speaking;               /* the language of the dictation going on, for its casing */
  gboolean close_when_idle;     /* closed while listening: it closes once the last words are in */
  gboolean restart_when_idle;   /* the language changed while listening: listening goes on in the new one */
} Window;

/* --- the document's file */

static char *
document_text (Window *self)
{
  GtkTextIter start, from, to, end;

  /* without the sentence still being spoken: it is not what was said yet */
  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  gtk_text_buffer_get_iter_at_mark (self->buffer, &from, self->from);
  gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
  if (gtk_text_iter_equal (&from, &to))
    return gtk_text_buffer_get_text (self->buffer, &start, &end, FALSE);
  {
    g_autofree char *before = gtk_text_buffer_get_text (self->buffer, &start, &from, FALSE);
    g_autofree char *after = gtk_text_buffer_get_text (self->buffer, &to, &end, FALSE);

    return g_strconcat (before, after, NULL);
  }
}

static void
save (Window *self)
{
  g_autofree char *text = document_text (self);
  g_autofree char *dir = g_path_get_dirname (self->path);
  g_autoptr (GError) error = NULL;

  g_clear_handle_id (&self->save_id, g_source_remove);
  if (g_mkdir_with_parents (dir, 0700) != 0 || !g_file_set_contents_full (self->path, text, -1,
                                                                           G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                                                           &error))
    g_warning ("%s: %s", self->path, error ? error->message : g_strerror (errno));
}

static gboolean
on_save_timeout (gpointer data)
{
  Window *self = data;

  self->save_id = 0;
  save (self);
  return G_SOURCE_REMOVE;
}

static void
load (Window *self)
{
  g_autofree char *text = NULL;
  GtkTextIter end;

  if (g_file_get_contents (self->path, &text, NULL, NULL) && g_utf8_validate (text, -1, NULL))
    {
      gtk_text_buffer_begin_irreversible_action (self->buffer);
      gtk_text_buffer_set_text (self->buffer, text, -1);
      gtk_text_buffer_end_irreversible_action (self->buffer);
    }
  /* dictation goes on at the end */
  gtk_text_buffer_get_end_iter (self->buffer, &end);
  gtk_text_buffer_place_cursor (self->buffer, &end);
}

/* --- what the screen shows */

static void
show_state (Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);
  gboolean listening = state == DICTATE_STARTING || state == DICTATE_LISTENING;
  gboolean empty = gtk_text_buffer_get_char_count (self->buffer) == 0;

  dictate_mic_set_listening (DICTATE_MIC (self->mic), listening || state == DICTATE_STOPPING);
  gtk_widget_set_sensitive (self->mic, state != DICTATE_STOPPING);
  gtk_label_set_text (GTK_LABEL (self->hint), listening ? _("Listening…") : _("Tap the microphone and start talking"));
  gtk_widget_set_visible (self->hint, empty);
  gtk_widget_set_sensitive (self->copy, !empty);
  gtk_widget_set_sensitive (self->clear, !empty);
}

static void
on_changed (GtkTextBuffer *buffer, Window *self)
{
  show_state (self);
  g_clear_handle_id (&self->save_id, g_source_remove);
  self->save_id = g_timeout_add (SAVE_DELAY_MS, on_save_timeout, self);
}

/* --- the dictation point */

/* `text` as it goes in at the dictation point: after a space, unless it starts the document or a line, or follows
 * one */
static char *
spaced (Window *self, const char *text)
{
  GtkTextIter at;

  gtk_text_buffer_get_iter_at_mark (self->buffer, &at, self->from);
  if (!gtk_text_iter_is_start (&at))
    {
      GtkTextIter before = at;

      gtk_text_iter_backward_char (&before);
      if (!g_unichar_isspace (gtk_text_iter_get_char (&before)))
        return g_strconcat (" ", text, NULL);
    }
  return g_strdup (text);
}

/* text at the dictation point starts a sentence (dictate-text.h) */
static gboolean
starts_sentence (Window *self)
{
  GtkTextIter from, back;
  g_autofree char *before = NULL;

  gtk_text_buffer_get_iter_at_mark (self->buffer, &from, self->from);
  back = from;
  gtk_text_iter_backward_chars (&back, 80);
  before = gtk_text_buffer_get_text (self->buffer, &back, &from, FALSE);
  return dictate_text_ends_sentence (before);
}

/* the dictation point's text: `text` as heard, cased by its sentences in the document, tagged `tag` (NULL for
 * none) */
static void
fill (Window *self, const char *text, GtkTextTag *tag)
{
  GtkTextIter from, to;

  gtk_text_buffer_get_iter_at_mark (self->buffer, &from, self->from);
  gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
  gtk_text_buffer_delete (self->buffer, &from, &to);
  if (*text)
    {
      g_autofree char *tidy = dictate_text_tidy (text, starts_sentence (self), self->speaking);
      g_autofree char *spaced_text = spaced (self, tidy);

      gtk_text_buffer_get_iter_at_mark (self->buffer, &from, self->from);
      if (tag)
        gtk_text_buffer_insert_with_tags (self->buffer, &from, spaced_text, -1, tag, NULL);
      else
        gtk_text_buffer_insert (self->buffer, &from, spaced_text, -1);
    }
}

/* the dictation point in sight, above the FAB's row */
static void
follow (Window *self)
{
  int height = gtk_widget_get_height (self->scroller);
  double margin = height > 0 ? MIN (0.45, (double) (FAB_ROW + 16) / height) : 0;

  gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (self->view), self->to, margin, FALSE, 0, 0);
}

static void
on_text (DictateEngine *engine, char **finished, const char *partial, Window *self)
{
  GtkTextIter to;

  gtk_text_buffer_begin_irreversible_action (self->buffer);
  for (guint i = 0; finished[i]; i++)
    {
      fill (self, finished[i], NULL);
      /* past it: the next goes after it */
      gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
      gtk_text_buffer_move_mark (self->buffer, self->from, &to);
    }
  fill (self, partial, self->partial);
  gtk_text_buffer_end_irreversible_action (self->buffer);
  follow (self);
}

/* --- the language */

static void
show_language (Window *self)
{
  const DictateLanguage *language = dictate_language_lookup (dictate_engine_get_language (self->engine));
  GtkWidget *label = neo_pressable_get_child (NEO_PRESSABLE (self->language));
  g_autofree char *code = language ? g_ascii_strup (language->code, -1) : NULL;
  /* TRANSLATORS: the language button's name for screen readers; %s is the language's name for itself */
  g_autofree char *name = language ? g_strdup_printf (_("Language: %s"), language->name) : NULL;

  /* without a model there is nothing to choose: the microphone says what to install */
  gtk_widget_set_visible (self->language, language != NULL);
  if (!language)
    return;
  gtk_label_set_text (GTK_LABEL (label), code);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->language), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

/* the language to listen in, as the models installed now allow */
static void
choose_language (Window *self)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "language");
  g_autoptr (DictateModel) model = *chosen ? dictate_model_find (chosen) : NULL;
  g_autofree char *code = model ? g_strdup (chosen) : dictate_language_default ();

  dictate_engine_set_language (self->engine, code);
  show_language (self);
}

static void start (Window *self);

static void
on_language_chosen (NeoDialog *dialog, int response, Window *self)
{
  GPtrArray *models = g_object_get_data (G_OBJECT (dialog), "dictate-models");
  const DictateModel *model;
  DictateState state;

  if (response < 100 || response - 100 >= (int) models->len)
    return;
  model = models->pdata[response - 100];
  g_settings_set_string (self->settings, "language", model->language);
  if (g_strcmp0 (model->language, dictate_engine_get_language (self->engine)) == 0)
    return;
  dictate_engine_set_language (self->engine, model->language);
  show_language (self);
  /* listening: what was said goes in in the old language, then listening goes on in the new one */
  state = dictate_engine_get_state (self->engine);
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
    {
      self->restart_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
}

static void
on_language_row (NeoPressable *row, NeoDialog *dialog)
{
  /* a row answers with its index, offset past the standard responses */
  neo_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "dictate-index")));
}

/* Android's single-choice list: a radio row for each language with a model, the current one checked; a tap on one
 * chooses it */
static void
on_language (NeoPressable *button, Window *self)
{
  GPtrArray *models = dictate_models_installed ();
  NeoDialog *dialog = neo_dialog_new (_("Language"));
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  const char *current = dictate_engine_get_language (self->engine);

  for (guint i = 0; i < models->len; i++)
    {
      const DictateModel *model = models->pdata[i];
      gboolean checked = g_strcmp0 (model->language, current) == 0;
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *mark = gtk_image_new_from_icon_name (checked ? "dictate-radio-on-symbolic"
                                                              : "dictate-radio-off-symbolic");
      GtkWidget *text = gtk_label_new (dictate_language_lookup (model->language)->name);
      GtkWidget *row;

      gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
      gtk_widget_add_css_class (mark, "dictate-radio");
      if (checked)
        gtk_widget_add_css_class (mark, "dictate-radio-checked");
      gtk_box_append (GTK_BOX (box), mark);
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_widget_set_hexpand (text, TRUE);
      gtk_box_append (GTK_BOX (box), text);
      row = neo_pressable_new (box);
      gtk_widget_add_css_class (row, "dictate-dialog-item");
      neo_pressable_set_ripple (NEO_PRESSABLE (row), NEO_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "dictate-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_language_row), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "dictate-dialog-list");
  neo_dialog_set_content (dialog, list);
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  g_object_set_data_full (G_OBJECT (dialog), "dictate-models", models, (GDestroyNotify) g_ptr_array_unref);
  g_signal_connect (dialog, "response", G_CALLBACK (on_language_chosen), self);
  neo_dialog_present (dialog, self->window);
}

static void
set_inhibit (Window *self, gboolean on)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self->window));

  if (on && !self->inhibit && app)
    self->inhibit = gtk_application_inhibit (app, GTK_WINDOW (self->window),
                                             GTK_APPLICATION_INHIBIT_IDLE | GTK_APPLICATION_INHIBIT_SUSPEND,
                                             _("Dictating"));
  else if (!on && self->inhibit)
    {
      if (app)
        gtk_application_uninhibit (app, self->inhibit);
      self->inhibit = 0;
    }
}

static void
on_state_changed (DictateEngine *engine, Window *self)
{
  DictateState state = dictate_engine_get_state (engine);

  set_inhibit (self, state != DICTATE_IDLE);
  if (state == DICTATE_IDLE)
    {
      GtkTextIter to;

      /* typing goes on after what was said */
      gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
      gtk_text_buffer_move_mark (self->buffer, self->from, &to);
      gtk_text_buffer_place_cursor (self->buffer, &to);
      follow (self);
      if (self->close_when_idle)
        {
          save (self);
          gtk_window_destroy (GTK_WINDOW (self->window));
          return;
        }
      if (self->restart_when_idle)
        {
          self->restart_when_idle = FALSE;
          start (self);
          return;
        }
    }
  show_state (self);
}

static void
on_error (DictateEngine *engine, const char *message, Window *self)
{
  neo_snackbar_show (self->snackbar, message, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
}

/* listening, with what is said going where the cursor is */
static void
start (Window *self)
{
  g_autofree char *reason = NULL;
  GtkTextIter at;

  /* a language installed since the window opened, or one taken away */
  choose_language (self);
  if (!dictate_engine_available (self->engine, &reason))
    {
      neo_snackbar_show (self->snackbar, reason, NULL, NULL, NULL, NEO_SNACKBAR_LONG);
      show_state (self);
      return;
    }
  gtk_text_buffer_get_iter_at_mark (self->buffer, &at, gtk_text_buffer_get_insert (self->buffer));
  gtk_text_buffer_move_mark (self->buffer, self->from, &at);
  gtk_text_buffer_move_mark (self->buffer, self->to, &at);
  g_free (self->speaking);
  self->speaking = g_strdup (dictate_engine_get_language (self->engine));
  dictate_engine_start (self->engine);
}

static void
on_mic (DictateMic *mic, Window *self)
{
  switch (dictate_engine_get_state (self->engine))
    {
    case DICTATE_IDLE:
      start (self);
      break;
    case DICTATE_STARTING:
    case DICTATE_LISTENING:
      dictate_engine_stop (self->engine);
      break;
    case DICTATE_STOPPING:
      break;
    }
}

void
dictate_window_toggle_dictation (GtkWidget *window)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");

  if (self)
    on_mic (DICTATE_MIC (self->mic), self);
}

/* --- the app bar's actions */

static void
on_copy (NeoPressable *button, Window *self)
{
  g_autofree char *text = document_text (self);

  gdk_clipboard_set_text (gtk_widget_get_clipboard (self->window), text);
  neo_snackbar_show (self->snackbar, _("Text copied"), NULL, NULL, NULL, NEO_SNACKBAR_SHORT);
}

static void
on_clear_response (NeoDialog *dialog, int response, Window *self)
{
  if (response != NEO_RESPONSE_OK)
    return;
  gtk_text_buffer_set_text (self->buffer, "", -1);
}

static void
on_clear (NeoPressable *button, Window *self)
{
  NeoDialog *dialog = neo_dialog_new (_("Clear the text?"));

  neo_dialog_set_message (dialog, _("All of the text is deleted."));
  neo_dialog_add_button (dialog, _("Cancel"), NEO_RESPONSE_CANCEL);
  neo_dialog_add_button (dialog, _("Clear"), NEO_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_clear_response), self);
  neo_dialog_present (dialog, self->window);
}

static GtkWidget *
action (const char *icon, const char *name, GCallback callback, Window *self)
{
  GtkWidget *button = neo_icon_button_new (icon);

  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect (button, "clicked", callback, self);
  return button;
}

/* --- the window */

static void
on_lift (GObject *snackbar, GParamSpec *pspec, Window *self)
{
  /* the FAB rises with the snackbar, which never covers it */
  gtk_widget_set_margin_bottom (self->mic, (FAB_ROW - FAB_SIZE) / 2 + (int) neo_snackbar_get_lift (self->snackbar));
}

static gboolean
on_close_request (GtkWindow *window, Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);

  if (state == DICTATE_IDLE)
    {
      save (self);
      return FALSE;
    }
  /* the words already said still go in, then it closes */
  self->close_when_idle = TRUE;
  dictate_engine_stop (self->engine);
  gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
  return TRUE;
}

static void
window_free (gpointer data)
{
  Window *self = data;

  g_clear_handle_id (&self->save_id, g_source_remove);
  g_signal_handlers_disconnect_by_data (self->engine, self);
  dictate_engine_stop (self->engine);
  g_clear_object (&self->engine);
  g_clear_object (&self->settings);
  g_free (self->speaking);
  g_free (self->path);
  g_free (self);
}

GtkWidget *
dictate_window_new (GtkApplication *app)
{
  Window *self = g_new0 (Window, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *title = gtk_label_new (_("Dictate"));
  GtkWidget *page = gtk_overlay_new ();
  GtkTextIter start;

  self->window = neo_window_new (app);
  self->engine = dictate_engine_new ();
  self->settings = g_settings_new ("neo.Dictate");
  self->path = g_build_filename (g_get_user_data_dir (), "neo-dictate", "document.txt", NULL);
  g_object_set_data_full (G_OBJECT (self->window), "dictate-window", self, window_free);
  gtk_window_set_title (GTK_WINDOW (self->window), _("Dictate"));

  /* the app bar */
  gtk_widget_add_css_class (bar, "neo-app-bar");
  gtk_widget_add_css_class (title, "neo-app-bar-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  neo_label_set_font_padding (title, 22);
  gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (bar), title);
  {
    GtkWidget *code = gtk_label_new (NULL);

    /* the outline round the code only, in the middle of the touch target */
    gtk_widget_set_halign (code, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (code, GTK_ALIGN_CENTER);
    self->language = neo_pressable_new (code);
  }
  gtk_widget_add_css_class (self->language, "neo-icon-button");
  gtk_widget_add_css_class (self->language, "dictate-language");
  neo_pressable_set_ripple (NEO_PRESSABLE (self->language), NEO_RIPPLE_CIRCLE, 20);
  g_signal_connect (self->language, "clicked", G_CALLBACK (on_language), self);
  gtk_box_append (GTK_BOX (bar), self->language);
  self->copy = action ("dictate-copy-symbolic", _("Copy"), G_CALLBACK (on_copy), self);
  gtk_box_append (GTK_BOX (bar), self->copy);
  self->clear = action ("dictate-delete-symbolic", _("Clear"), G_CALLBACK (on_clear), self);
  gtk_widget_add_css_class (self->clear, "dictate-bar-end");
  gtk_box_append (GTK_BOX (bar), self->clear);
  gtk_box_append (GTK_BOX (content), bar);

  /* the document */
  self->view = gtk_text_view_new ();
  self->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  gtk_widget_add_css_class (self->view, "dictate-document");
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->view), 24);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->view), 24);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (self->view), 8);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->view), FAB_ROW + 16);
  gtk_text_view_set_pixels_inside_wrap (GTK_TEXT_VIEW (self->view), 4);
  gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (self->view), 4);
  gtk_text_view_set_input_hints (GTK_TEXT_VIEW (self->view),
                                 GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_UPPERCASE_SENTENCES);
  {
    GdkRGBA variant = neo_color (NEO_ON_SURFACE_VARIANT);

    self->partial = gtk_text_buffer_create_tag (self->buffer, "partial", "foreground-rgba", &variant, NULL);
  }
  gtk_text_buffer_get_start_iter (self->buffer, &start);
  self->from = gtk_text_buffer_create_mark (self->buffer, "dictation-from", &start, TRUE);
  self->to = gtk_text_buffer_create_mark (self->buffer, "dictation-to", &start, FALSE);
  load (self);
  g_signal_connect (self->buffer, "changed", G_CALLBACK (on_changed), self);

  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->view);
  gtk_widget_set_vexpand (self->scroller, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (page), self->scroller);

  /* what to do, while there is nothing */
  self->hint = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->hint, "dictate-hint");
  gtk_label_set_wrap (GTK_LABEL (self->hint), TRUE);
  gtk_label_set_justify (GTK_LABEL (self->hint), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (self->hint, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->hint, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target (self->hint, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->hint);

  /* the text fades out under the FAB's row, as the stopwatch's laps */
  {
    GtkWidget *fade = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_widget_add_css_class (fade, "dictate-fade");
    gtk_widget_set_valign (fade, GTK_ALIGN_END);
    gtk_widget_set_can_target (fade, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (page), fade);
  }

  /* the microphone, in the middle of the bottom row */
  self->mic = dictate_mic_new (self->engine);
  gtk_widget_set_halign (self->mic, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->mic, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (self->mic, (FAB_ROW - FAB_SIZE) / 2);
  g_signal_connect (self->mic, "clicked", G_CALLBACK (on_mic), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->mic);

  self->snackbar = NEO_SNACKBAR (neo_snackbar_new ());
  g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_lift), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), GTK_WIDGET (self->snackbar));

  gtk_box_append (GTK_BOX (content), page);
  neo_window_set_content (GTK_WINDOW (self->window), content);

  g_signal_connect (self->engine, "text", G_CALLBACK (on_text), self);
  g_signal_connect (self->engine, "state-changed", G_CALLBACK (on_state_changed), self);
  g_signal_connect (self->engine, "error", G_CALLBACK (on_error), self);
  g_signal_connect (self->window, "close-request", G_CALLBACK (on_close_request), self);
  choose_language (self);
  show_state (self);
  return self->window;
}
