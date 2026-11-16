/* The Dictate window: see dictate-window.h.
 *
 * The document is not typed in: what is said goes at its end, and it can be selected and copied, with no keyboard
 * coming up (an on-screen keyboard opens for an editable text that has the focus). A finger selects as on Android, a
 * long press or a double tap on a word, and selected text gets Android's floating toolbar over it: Copy and Select
 * all.
 *
 * The dictation point is the stretch between two marks: `from` (left gravity) and `to` (right gravity). The sentence
 * being spoken fills it, tagged "partial"; a finished sentence takes its place untagged, and both marks move past
 * it, so the next one follows it.
 *
 * The language listened in is the app bar's button, showing its code: the user's choice (GSettings vibeos.Dictate
 * language) while its model is installed, else the first of the user's languages with one, else English. */
#include "dictate-window.h"
#include "dictate-engine.h"
#include "dictate-mic.h"
#include "dictate-models.h"
#include "dictate-text.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define FAB_ROW         120     /* the FAB's row at the bottom: the document's last line scrolls up out of it */
#define FAB_SIZE        88
#define SAVE_DELAY_MS   1000    /* after the last change, the document is written */

typedef struct {
  GtkWidget *window;
  GtkWidget *page, *scroller, *view, *hint, *mic, *language, *copy, *clear;
  GtkWidget *selection_bar, *select_all;
  guint place_id;               /* the selection bar is placed when idle */
  GtkTextMark *anchor_start, *anchor_end;     /* the word a finger's selection began on */
  gboolean selecting;           /* a finger is selecting: the bar waits until it lifts */
  VibeosSnackbar *snackbar;
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
    gtk_text_buffer_set_text (self->buffer, text, -1);
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

  for (guint i = 0; finished[i]; i++)
    {
      fill (self, finished[i], NULL);
      /* past it: the next goes after it */
      gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
      gtk_text_buffer_move_mark (self->buffer, self->from, &to);
    }
  fill (self, partial, self->partial);
  follow (self);
}

/* --- the language */

static void
show_language (Window *self)
{
  const DictateLanguage *language = dictate_language_lookup (dictate_engine_get_language (self->engine));
  GtkWidget *label = vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->language));
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
on_language_chosen (VibeosDialog *dialog, int response, Window *self)
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
on_language_row (VibeosPressable *row, VibeosDialog *dialog)
{
  /* a row answers with its index, offset past the standard responses */
  vibeos_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "dictate-index")));
}

/* Android's single-choice list: a radio row for each language with a model, the current one checked; a tap on one
 * chooses it */
static void
on_language (VibeosPressable *button, Window *self)
{
  GPtrArray *models = dictate_models_installed ();
  VibeosDialog *dialog = vibeos_dialog_new (_("Language"));
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
      row = vibeos_pressable_new (box);
      gtk_widget_add_css_class (row, "dictate-dialog-item");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "dictate-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_language_row), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "dictate-dialog-list");
  vibeos_dialog_set_content (dialog, list);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  g_object_set_data_full (G_OBJECT (dialog), "dictate-models", models, (GDestroyNotify) g_ptr_array_unref);
  g_signal_connect (dialog, "response", G_CALLBACK (on_language_chosen), self);
  vibeos_dialog_present (dialog, self->window);
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

      /* the partial sentence is gone (the last one finished), the point empty at the end */
      gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->to);
      gtk_text_buffer_move_mark (self->buffer, self->from, &to);
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
  vibeos_snackbar_show (self->snackbar, message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
}

/* listening, with what is said going at the document's end */
static void
start (Window *self)
{
  g_autofree char *reason = NULL;
  GtkTextIter at;

  /* a language installed since the window opened, or one taken away */
  choose_language (self);
  if (!dictate_engine_available (self->engine, &reason))
    {
      vibeos_snackbar_show (self->snackbar, reason, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
      show_state (self);
      return;
    }
  gtk_text_buffer_get_end_iter (self->buffer, &at);
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
on_copy (VibeosPressable *button, Window *self)
{
  g_autofree char *text = document_text (self);

  gdk_clipboard_set_text (gtk_widget_get_clipboard (self->window), text);
  vibeos_snackbar_show (self->snackbar, _("Text copied"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

static void
on_clear_response (VibeosDialog *dialog, int response, Window *self)
{
  if (response != VIBEOS_RESPONSE_OK)
    return;
  gtk_text_buffer_set_text (self->buffer, "", -1);
}

static void
on_clear (VibeosPressable *button, Window *self)
{
  VibeosDialog *dialog = vibeos_dialog_new (_("Clear the text?"));

  vibeos_dialog_set_message (dialog, _("All of the text is deleted."));
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Clear"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_clear_response), self);
  vibeos_dialog_present (dialog, self->window);
}

static GtkWidget *
action (const char *icon, const char *name, GCallback callback, Window *self)
{
  GtkWidget *button = vibeos_icon_button_new (icon);

  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_signal_connect (button, "clicked", callback, self);
  return button;
}

/* --- the selection's bar: Android's floating text toolbar, over the selected text */

#define BAR_GAP 8       /* between the bar and the selected text, and the page's edges */

static gboolean
place_selection_bar (gpointer data)
{
  Window *self = data;
  GtkTextView *view = GTK_TEXT_VIEW (self->view);
  GtkTextIter start, end;
  GdkRectangle first, last;
  graphene_point_t top, bottom;
  int page_width = gtk_widget_get_width (self->page), page_height = gtk_widget_get_height (self->page);
  int width, height, x, y;
  double centre;

  self->place_id = 0;
  if (!gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end) || self->selecting)
    {
      gtk_widget_set_visible (self->selection_bar, FALSE);
      return G_SOURCE_REMOVE;
    }

  /* the selection's first line's top and last line's bottom, in the page */
  gtk_text_view_get_iter_location (view, &start, &first);
  gtk_text_view_get_iter_location (view, &end, &last);
  gtk_text_view_buffer_to_window_coords (view, GTK_TEXT_WINDOW_WIDGET, first.x, first.y, &first.x, &first.y);
  gtk_text_view_buffer_to_window_coords (view, GTK_TEXT_WINDOW_WIDGET, last.x, last.y + last.height, &last.x,
                                         &last.y);
  if (!gtk_widget_compute_point (self->view, self->page, &GRAPHENE_POINT_INIT (first.x, first.y), &top) ||
      !gtk_widget_compute_point (self->view, self->page, &GRAPHENE_POINT_INIT (last.x, last.y), &bottom))
    return G_SOURCE_REMOVE;
  /* scrolled out of sight: the bar comes back with it */
  if (bottom.y < 0 || top.y > page_height - FAB_ROW)
    {
      gtk_widget_set_visible (self->selection_bar, FALSE);
      return G_SOURCE_REMOVE;
    }

  gtk_widget_set_visible (self->select_all, !gtk_text_iter_is_start (&start) || !gtk_text_iter_is_end (&end));
  gtk_widget_set_visible (self->selection_bar, TRUE);
  /* its size, without the margins that place it (measuring counts them) */
  gtk_widget_measure (self->selection_bar, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
  gtk_widget_measure (self->selection_bar, GTK_ORIENTATION_VERTICAL, width, NULL, &height, NULL, NULL);
  width -= gtk_widget_get_margin_start (self->selection_bar);
  height -= gtk_widget_get_margin_top (self->selection_bar);

  /* above the selection, else below it, else (it fills the page) at the top; never over the FAB's row */
  y = top.y - BAR_GAP - height;
  if (y < BAR_GAP)
    y = bottom.y + BAR_GAP;
  y = CLAMP (y, BAR_GAP, MAX (BAR_GAP, page_height - FAB_ROW - height));
  /* over the middle of a selection on one line, else the page's */
  centre = first.y == last.y - last.height ? (top.x + bottom.x) / 2 : page_width / 2.0;
  x = CLAMP ((int) centre - width / 2, BAR_GAP, MAX (BAR_GAP, page_width - BAR_GAP - width));
  gtk_widget_set_margin_start (self->selection_bar, x);
  gtk_widget_set_margin_top (self->selection_bar, y);
  return G_SOURCE_REMOVE;
}

static void
queue_place (Window *self)
{
  if (!self->place_id)
    self->place_id = g_idle_add (place_selection_bar, self);
}

static void
on_mark_set (GtkTextBuffer *buffer, GtkTextIter *at, GtkTextMark *mark, Window *self)
{
  if (mark == gtk_text_buffer_get_insert (buffer) || mark == gtk_text_buffer_get_selection_bound (buffer))
    queue_place (self);
}

static void
on_copy_selection (VibeosPressable *button, Window *self)
{
  GtkTextIter start, end;
  g_autofree char *text = NULL;

  if (!gtk_text_buffer_get_selection_bounds (self->buffer, &start, &end))
    return;
  text = gtk_text_buffer_get_text (self->buffer, &start, &end, FALSE);
  gdk_clipboard_set_text (gtk_widget_get_clipboard (self->window), text);
  /* as Android: the selection goes once copied */
  gtk_text_buffer_select_range (self->buffer, &end, &end);
  vibeos_snackbar_show (self->snackbar, _("Text copied"), NULL, NULL, NULL, VIBEOS_SNACKBAR_SHORT);
}

static void
on_select_all (VibeosPressable *button, Window *self)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (self->buffer, &start, &end);
  gtk_text_buffer_select_range (self->buffer, &start, &end);
}

static GtkWidget *
selection_bar (Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *copy = vibeos_text_button_new (_("Copy"));

  gtk_widget_add_css_class (bar, "dictate-selection-bar");
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_selection), self);
  gtk_box_append (GTK_BOX (bar), copy);
  self->select_all = vibeos_text_button_new (_("Select all"));
  g_signal_connect (self->select_all, "clicked", G_CALLBACK (on_select_all), self);
  gtk_box_append (GTK_BOX (bar), self->select_all);
  gtk_widget_set_halign (bar, GTK_ALIGN_START);
  gtk_widget_set_valign (bar, GTK_ALIGN_START);
  gtk_widget_set_visible (bar, FALSE);
  return bar;
}

/* --- selecting with a finger, as on Android
 *
 * GTK's text view does not do it in a scrolling document: a double tap's word is unselected again as the finger
 * lifts, a long press does nothing, and the drag that selects words is the scroller's. So for a touch the document
 * has these instead, and its own gestures never see one: a long press selects the word under it, a double tap too,
 * and a drag on from either selects word by word; a tap lets the selection go. The scroller, the view's parent, still
 * sees every touch first, and scrolls. A mouse selects as in any GTK text view. */

/* the text under a point of the view; FALSE off the text */
static gboolean
iter_at (Window *self, double x, double y, GtkTextIter *iter)
{
  int bx, by;

  gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (self->view), GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
  return gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (self->view), iter, bx, by);
}

/* the word at `at`; on a space just after one, that word; elsewhere between words, the character */
static void
word_bounds (const GtkTextIter *at, GtkTextIter *start, GtkTextIter *end)
{
  *start = *end = *at;
  if (gtk_text_iter_inside_word (start))
    {
      if (!gtk_text_iter_starts_word (start))
        gtk_text_iter_backward_word_start (start);
      gtk_text_iter_forward_word_end (end);
    }
  else if (gtk_text_iter_ends_word (start))
    gtk_text_iter_backward_word_start (start);
  else
    gtk_text_iter_forward_char (end);
}

static gboolean
begin_selecting (Window *self, double x, double y)
{
  GtkTextIter at, start, end;

  if (!iter_at (self, x, y, &at))
    return FALSE;
  word_bounds (&at, &start, &end);
  gtk_text_buffer_move_mark (self->buffer, self->anchor_start, &start);
  gtk_text_buffer_move_mark (self->buffer, self->anchor_end, &end);
  self->selecting = TRUE;
  /* focused, the selection has its focused colour; read only, no keyboard comes with it */
  gtk_widget_grab_focus (self->view);
  gtk_text_buffer_select_range (self->buffer, &start, &end);
  return TRUE;
}

static void
end_selecting (Window *self)
{
  if (!self->selecting)
    return;
  self->selecting = FALSE;
  queue_place (self);
}

static void
on_long_press (GtkGestureLongPress *gesture, double x, double y, Window *self)
{
  /* claimed, the touch is no longer the scroller's: moving on selects */
  if (begin_selecting (self, x, y))
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_tap_pressed (GtkGestureClick *gesture, int n_press, double x, double y, Window *self)
{
  if (n_press == 2 && begin_selecting (self, x, y))
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_tap_released (GtkGestureClick *gesture, int n_press, double x, double y, Window *self)
{
  GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  GtkTextIter at;

  /* a tap, not the end of a selecting touch (those are claimed) */
  if (n_press != 1 || gtk_gesture_get_sequence_state (GTK_GESTURE (gesture), sequence) == GTK_EVENT_SEQUENCE_CLAIMED)
    return;
  iter_at (self, x, y, &at);
  gtk_text_buffer_place_cursor (self->buffer, &at);
}

static void
on_select_drag (GtkGestureDrag *gesture, double dx, double dy, Window *self)
{
  GtkTextIter at, start, end, from, to;
  double x, y;

  if (!self->selecting || !gtk_gesture_drag_get_start_point (gesture, &x, &y))
    return;
  /* from the first word to the one under the finger, whichever way it went */
  iter_at (self, x + dx, y + dy, &at);
  word_bounds (&at, &start, &end);
  gtk_text_buffer_get_iter_at_mark (self->buffer, &from, self->anchor_start);
  gtk_text_buffer_get_iter_at_mark (self->buffer, &to, self->anchor_end);
  if (gtk_text_iter_compare (&start, &from) < 0)
    from = start;
  if (gtk_text_iter_compare (&end, &to) > 0)
    to = end;
  gtk_text_buffer_select_range (self->buffer, &from, &to);
}

static void
on_select_drag_end (GtkGestureDrag *gesture, double dx, double dy, Window *self)
{
  end_selecting (self);
}

static void
on_select_cancel (GtkGesture *gesture, GdkEventSequence *sequence, Window *self)
{
  end_selecting (self);
}

/* the view's own gestures never see a touch: the ones above, ahead of this in the capture phase, do */
static gboolean
on_view_event (GtkEventControllerLegacy *controller, GdkEvent *event, Window *self)
{
  switch (gdk_event_get_event_type (event))
    {
    case GDK_TOUCH_BEGIN:
    case GDK_TOUCH_UPDATE:
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
      return GDK_EVENT_STOP;
    default:
      return GDK_EVENT_PROPAGATE;
    }
}

static void
touch_selection (Window *self)
{
  GtkGesture *taps = gtk_gesture_click_new ();
  GtkGesture *long_press = gtk_gesture_long_press_new ();
  GtkGesture *drag = gtk_gesture_drag_new ();
  GtkEventController *stop = gtk_event_controller_legacy_new ();
  GtkGesture *gestures[] = { taps, long_press, drag };

  /* a widget's controllers run newest first, and a controller that is not a gesture stops the run: it goes in first */
  g_signal_connect (stop, "event", G_CALLBACK (on_view_event), self);
  gtk_event_controller_set_propagation_phase (stop, GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (self->view, stop);

  g_signal_connect (taps, "pressed", G_CALLBACK (on_tap_pressed), self);
  g_signal_connect (taps, "released", G_CALLBACK (on_tap_released), self);
  g_signal_connect (long_press, "pressed", G_CALLBACK (on_long_press), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_select_drag), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_select_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_select_cancel), self);
  /* one group: a claim by one (the long press, the double tap) is all three's, so the drag goes on selecting */
  gtk_gesture_group (long_press, taps);
  gtk_gesture_group (drag, taps);
  for (guint i = 0; i < G_N_ELEMENTS (gestures); i++)
    {
      gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (gestures[i]), TRUE);
      gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (gestures[i]), GTK_PHASE_CAPTURE);
      gtk_widget_add_controller (self->view, GTK_EVENT_CONTROLLER (gestures[i]));
    }
}

/* --- the window */

static void
on_lift (GObject *snackbar, GParamSpec *pspec, Window *self)
{
  /* the FAB rises with the snackbar, which never covers it */
  gtk_widget_set_margin_bottom (self->mic, (FAB_ROW - FAB_SIZE) / 2 + (int) vibeos_snackbar_get_lift (self->snackbar));
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
  g_clear_handle_id (&self->place_id, g_source_remove);
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
  GtkWidget *page = self->page = gtk_overlay_new ();
  GtkTextIter start;

  self->window = vibeos_window_new (app);
  self->engine = dictate_engine_new ();
  self->settings = g_settings_new ("vibeos.Dictate");
  self->path = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "document.txt", NULL);
  g_object_set_data_full (G_OBJECT (self->window), "dictate-window", self, window_free);
  gtk_window_set_title (GTK_WINDOW (self->window), _("Dictate"));

  /* the app bar */
  gtk_widget_add_css_class (bar, "vibeos-app-bar");
  gtk_widget_add_css_class (title, "vibeos-app-bar-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  vibeos_label_set_font_padding (title, 22);
  gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (bar), title);
  {
    GtkWidget *code = gtk_label_new (NULL);

    /* the outline round the code only, in the middle of the touch target */
    gtk_widget_set_halign (code, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (code, GTK_ALIGN_CENTER);
    self->language = vibeos_pressable_new (code);
  }
  gtk_widget_add_css_class (self->language, "vibeos-icon-button");
  gtk_widget_add_css_class (self->language, "dictate-language");
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (self->language), VIBEOS_RIPPLE_CIRCLE, 20);
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
  /* read only: selectable, but nothing to type, so no keyboard */
  gtk_text_view_set_editable (GTK_TEXT_VIEW (self->view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (self->view), FALSE);
  gtk_text_buffer_set_enable_undo (self->buffer, FALSE);
  {
    GdkRGBA variant = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);

    self->partial = gtk_text_buffer_create_tag (self->buffer, "partial", "foreground-rgba", &variant, NULL);
  }
  gtk_text_buffer_get_start_iter (self->buffer, &start);
  self->from = gtk_text_buffer_create_mark (self->buffer, "dictation-from", &start, TRUE);
  self->to = gtk_text_buffer_create_mark (self->buffer, "dictation-to", &start, FALSE);
  self->anchor_start = gtk_text_buffer_create_mark (self->buffer, NULL, &start, TRUE);
  self->anchor_end = gtk_text_buffer_create_mark (self->buffer, NULL, &start, FALSE);
  touch_selection (self);
  load (self);
  g_signal_connect (self->buffer, "changed", G_CALLBACK (on_changed), self);
  g_signal_connect (self->buffer, "mark-set", G_CALLBACK (on_mark_set), self);

  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->view);
  gtk_widget_set_vexpand (self->scroller, TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (page), self->scroller);
  g_signal_connect_swapped (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller)), "value-changed",
                            G_CALLBACK (queue_place), self);

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

  self->selection_bar = selection_bar (self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->selection_bar);

  /* the microphone, in the middle of the bottom row */
  self->mic = dictate_mic_new (self->engine);
  gtk_widget_set_halign (self->mic, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->mic, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (self->mic, (FAB_ROW - FAB_SIZE) / 2);
  g_signal_connect (self->mic, "clicked", G_CALLBACK (on_mic), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->mic);

  self->snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());
  g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_lift), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), GTK_WIDGET (self->snackbar));

  gtk_box_append (GTK_BOX (content), page);
  vibeos_window_set_content (GTK_WINDOW (self->window), content);

  g_signal_connect (self->engine, "text", G_CALLBACK (on_text), self);
  g_signal_connect (self->engine, "state-changed", G_CALLBACK (on_state_changed), self);
  g_signal_connect (self->engine, "error", G_CALLBACK (on_error), self);
  g_signal_connect (self->window, "close-request", G_CALLBACK (on_close_request), self);
  choose_language (self);
  show_state (self);
  return self->window;
}
