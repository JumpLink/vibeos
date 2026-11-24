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
 * language) while its model is installed, else the first of the user's languages with one, else English.
 *
 * Started as Agent (vibeos-dictate --agent, dictate_window_new's `agent`), the page is Talk, a conversation
 * instead of a document: what the user
 * says is a bubble on the right, the model's reply (dictate-talk.h) one on the left, written as it comes and spoken
 * sentence by sentence. The engine listens for one turn (dictate_engine_set_turns: voice activity detection
 * finds where the user has finished) and what they said goes to the model; once the reply has been spoken it
 * listens again. While it listens or a reply is written or spoken, the FAB is a stop square and stops all of it: the
 * reply ends where it is, what was heard and not yet sent is dropped, and nothing listens until the next tap. */
#include "dictate-window.h"
#include "dictate-download-dialog.h"
#include "dictate-engine.h"
#include "dictate-mic.h"
#include "dictate-models.h"
#include "dictate-settings.h"
#include "dictate-talk.h"
#include "dictate-text.h"
#include "dictate-tools.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <vibeos.h>
#include <spongy-gtk.h>

#define FAB_ROW         120     /* the FAB's row at the bottom: the document's last line scrolls up out of it */
#define FAB_ROW_TABLET  168     /* and on a tablet: 16 px under the FAB put it on the screen's bottom edge, where
                                 * the swipe home starts (owner, 11-21); 40 px there */
#define FAB_SIZE        88
#define SAVE_DELAY_MS   1000    /* after the last change, the document is written */
#define TEXT_MARGIN     24      /* the document's margins on a phone */
#define COLUMN_MAX      640     /* on a wider window the text keeps to a column this wide, in the middle */

typedef struct {
  GtkWidget *window;
  GtkWidget *pages, *marker;    /* the conversation and the pages over it (Agent's Settings); Back's marker */
  GtkWidget *page, *scroller, *view, *hint, *mic, *language, *copy, *clear;
  GtkWidget *selection_bar, *select_all;
  int fab_row;                  /* FAB_ROW or FAB_ROW_TABLET, by the device */
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
  gboolean drop_when_idle;      /* Talk: stopped by the user while listening: what was heard is not sent */
  /* Talk */
  GtkWidget *title, *stack, *talk_scroller, *talk_list, *speak;
  DictateTalk *talk;
  gboolean talking;             /* Agent's window: the Talk page */
  GtkWidget *mine, *reply;      /* the bubbles being filled: what the user says, the model's answer */
  GString *heard;               /* the user's finished sentences so far */
  char *said;                   /* and the one being spoken */
  gboolean stopping_reply;      /* the user stopped the reply: it does not listen again on its own */
  DictateTalkState talk_state;  /* the last one seen */
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
  g_autofree char *text = NULL;
  g_autofree char *dir = g_path_get_dirname (self->path);
  g_autoptr (GError) error = NULL;

  g_clear_handle_id (&self->save_id, g_source_remove);
  /* Agent's window has no document: Dictate's is not its to write */
  if (self->talking)
    return;
  text = document_text (self);
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
  gboolean replying = self->talk && dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE;
  gboolean empty = self->talking ? gtk_widget_get_first_child (self->talk_list) == NULL
                                 : gtk_text_buffer_get_char_count (self->buffer) == 0;

  /* in Talk the stop square also stops a reply */
  dictate_mic_set_listening (DICTATE_MIC (self->mic), listening || state == DICTATE_STOPPING ||
                                                      (self->talking && replying));
  gtk_widget_set_sensitive (self->mic, state != DICTATE_STOPPING);
  gtk_label_set_text (GTK_LABEL (self->hint), listening ? _("Listening…")
                                              : self->talking ? _("Tap the microphone and talk")
                                              : _("Tap the microphone and start talking"));
  gtk_widget_set_visible (self->hint, empty);
  gtk_widget_set_visible (self->copy, !self->talking);
  gtk_widget_set_sensitive (self->copy, !empty);
  gtk_widget_set_sensitive (self->clear, !empty);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->clear), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  self->talking ? _("New conversation") : _("Clear"), -1);
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
  double margin = height > 0 ? MIN (0.45, (double) (self->fab_row + 16) / height) : 0;

  gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (self->view), self->to, margin, FALSE, 0, 0);
}

static void talk_heard (Window *self, char **finished, const char *partial);

static void
on_text (DictateEngine *engine, char **finished, const char *partial, Window *self)
{
  GtkTextIter to;

  if (self->talking)
    {
      talk_heard (self, finished, partial);
      return;
    }

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
  /* Agent's settings say it too */
  if (g_object_get_data (G_OBJECT (self->window), "dictate-settings-page"))
    dictate_settings_page_refresh (g_object_get_data (G_OBJECT (self->window), "dictate-settings-page"));
  if (!language)
    return;
  gtk_label_set_text (GTK_LABEL (label), code);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->language), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

/* The language is now `code`. Every path that changes it comes here, so that all three follow it at once, in the
 * running window: listening (the engine loads that language's model at its next start, Moonshine or Vosk), the
 * reply (Agent's model is told to answer in it with its next question: dictate-talk.c rebuilds its prompt when the
 * language a question comes with changes) and the voice (Agent's for that language, loaded now). */
static void
language_changed (Window *self, const char *code)
{
  DictateState state;

  if (g_strcmp0 (code, dictate_engine_get_language (self->engine)) == 0)
    {
      show_language (self);
      return;
    }
  g_message ("language: now %s", code);
  dictate_engine_set_language (self->engine, code);
  show_language (self);
  /* Agent's voice for it loads now, while nothing is said: its first reply is sooner */
  if (self->talk && dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE)
    dictate_talk_prepare (self->talk, code);
  /* listening: what was said goes in in the old language, then listening goes on in the new one */
  state = dictate_engine_get_state (self->engine);
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
    {
      self->restart_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
}

/* the language to listen in, as the models installed now allow: the chosen one, or the default when its model has
 * gone */
static void
choose_language (Window *self)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "language");
  g_autoptr (DictateModel) model = *chosen ? dictate_model_find (chosen) : NULL;
  g_autofree char *code = model ? g_strdup (chosen) : dictate_language_default ();

  language_changed (self, code);
}

static void start (Window *self);

/* the language chosen, with its model (and Agent's voice) on the phone: already there, or downloaded just now */
static void
on_language_ready (const char *language, gpointer data)
{
  Window *self = data;

  g_settings_set_string (self->settings, "language", language);
  language_changed (self, language);
}

static void
on_language_chosen (VibeosDialog *dialog, int response, Window *self)
{
  guint n;
  const DictateLanguage *languages = dictate_languages (&n);

  if (response < 100 || response - 100 >= (int) n)
    return;
  /* what it lacks is downloaded first, if the user wants it; until it is all in, the language stays as it was */
  dictate_download_ensure (self->window, languages[response - 100].code, self->talk, on_language_ready, self);
}

static void
on_language_row (VibeosPressable *row, VibeosDialog *dialog)
{
  /* a row answers with its index, offset past the standard responses */
  vibeos_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "dictate-index")));
}

/* Android's single-choice list: a radio row for each language, the current one checked, and under one that is not
 * on the phone yet what it lacks (dictate-download-dialog.h); a tap on one chooses it */
static void
on_language (VibeosPressable *button, Window *self)
{
  guint n;
  const DictateLanguage *languages = dictate_languages (&n);
  VibeosDialog *dialog = vibeos_dialog_new (_("Language"));
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  const char *current = dictate_engine_get_language (self->engine);

  for (guint i = 0; i < n; i++)
    {
      gboolean checked = g_strcmp0 (languages[i].code, current) == 0;
      g_autofree char *note = dictate_download_note (languages[i].code, self->talk);
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *mark = gtk_image_new_from_icon_name (checked ? "dictate-radio-on-symbolic"
                                                              : "dictate-radio-off-symbolic");
      GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *text = gtk_label_new (languages[i].name);
      GtkWidget *row;

      gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
      gtk_widget_add_css_class (mark, "dictate-radio");
      if (checked)
        gtk_widget_add_css_class (mark, "dictate-radio-checked");
      gtk_box_append (GTK_BOX (box), mark);
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_box_append (GTK_BOX (texts), text);
      if (note)
        {
          GtkWidget *under = gtk_label_new (note);

          gtk_label_set_xalign (GTK_LABEL (under), 0);
          gtk_label_set_wrap (GTK_LABEL (under), TRUE);
          gtk_widget_add_css_class (under, "dictate-dialog-note");
          gtk_box_append (GTK_BOX (texts), under);
        }
      gtk_widget_set_hexpand (texts, TRUE);
      gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), texts);
      row = vibeos_pressable_new (box);
      gtk_widget_add_css_class (row, "dictate-dialog-item");
      if (note)
        gtk_widget_add_css_class (row, "dictate-dialog-item-two");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "dictate-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_language_row), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "dictate-dialog-list");
  vibeos_dialog_set_content (dialog, list);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
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

/* --- Talk */

static void talk_listen (Window *self);

/* the conversation's end in sight, above the FAB's row, once its new size is laid out */
static gboolean
talk_follow_idle (gpointer data)
{
  Window *self = data;
  GtkAdjustment *adjustment = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->talk_scroller));

  gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment) -
                                        gtk_adjustment_get_page_size (adjustment));
  return G_SOURCE_REMOVE;
}

static void
talk_follow (Window *self)
{
  g_idle_add_full (G_PRIORITY_LOW, talk_follow_idle, self, NULL);
}

/* a bubble at the conversation's end: the user's on the right, the model's on the left */
static GtkWidget *
talk_bubble (Window *self, gboolean mine, const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  /* a bubble is as wide as its text, up to about four fifths of a phone's column */
  gtk_label_set_max_width_chars (GTK_LABEL (label), 34);
  gtk_widget_add_css_class (label, "dictate-bubble");
  gtk_widget_add_css_class (label, mine ? "dictate-bubble-mine" : "dictate-bubble-reply");
  gtk_widget_set_halign (label, mine ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (self->talk_list), label);
  talk_follow (self);
  return label;
}

/* the screen reader's name for a bubble, once its text is final */
static void
talk_name (GtkWidget *bubble, gboolean mine)
{
  /* TRANSLATORS: a Talk bubble's name for screen readers; %s is what the user said, or what the model answered */
  g_autofree char *name = g_strdup_printf (mine ? _("You: %s") : _("Vibe: %s"),
                                           gtk_label_get_text (GTK_LABEL (bubble)));
  gtk_accessible_update_property (GTK_ACCESSIBLE (bubble), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

/* what the user has said so far, as it reads */
static char *
talk_said (Window *self)
{
  if (self->said && *self->said)
    {
      g_autofree char *tidy = dictate_text_tidy (self->said, dictate_text_ends_sentence (self->heard->str),
                                                 self->speaking);
      return self->heard->len ? g_strconcat (self->heard->str, " ", tidy, NULL) : g_steal_pointer (&tidy);
    }
  return g_strdup (self->heard->str);
}

static void
talk_heard (Window *self, char **finished, const char *partial)
{
  g_autofree char *said = NULL;

  for (guint i = 0; finished[i]; i++)
    {
      g_autofree char *tidy = dictate_text_tidy (finished[i], dictate_text_ends_sentence (self->heard->str),
                                                 self->speaking);
      if (self->heard->len)
        g_string_append_c (self->heard, ' ');
      g_string_append (self->heard, tidy);
    }
  g_free (self->said);
  self->said = g_strdup (partial);
  said = talk_said (self);
  if (self->mine)
    {
      gtk_label_set_text (GTK_LABEL (self->mine), *said ? said : "…");
      talk_follow (self);
    }
}

/* the user stopped the listening: what was heard is not sent, and its bubble goes */
static void
talk_drop (Window *self)
{
  g_string_truncate (self->heard, 0);
  g_clear_pointer (&self->said, g_free);
  if (self->mine)
    {
      gtk_box_remove (GTK_BOX (self->talk_list), self->mine);
      self->mine = NULL;
    }
}

/* listening has ended: what was said goes to the model, or its empty bubble goes */
static void
talk_send (Window *self)
{
  g_autofree char *said = talk_said (self);

  if (!self->mine)
    return;
  if (!*g_strstrip (said))
    {
      gtk_box_remove (GTK_BOX (self->talk_list), self->mine);
      self->mine = NULL;
      return;
    }
  gtk_label_set_text (GTK_LABEL (self->mine), said);
  talk_name (self->mine, TRUE);
  self->mine = NULL;
  self->reply = talk_bubble (self, FALSE, "…");
  dictate_talk_ask (self->talk, dictate_engine_get_language (self->engine), said);
}

static void
talk_listen (Window *self)
{
  g_autofree char *reason = NULL;

  choose_language (self);
  if (!dictate_engine_available (self->engine, &reason) ||
      !dictate_talk_available (self->talk, dictate_engine_get_language (self->engine), &reason))
    {
      vibeos_snackbar_show (self->snackbar, reason, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
      show_state (self);
      return;
    }
  g_string_truncate (self->heard, 0);
  g_clear_pointer (&self->said, g_free);
  g_free (self->speaking);
  self->speaking = g_strdup (dictate_engine_get_language (self->engine));
  if (!self->mine)
    self->mine = talk_bubble (self, TRUE, "…");
  dictate_engine_set_turns (self->engine, TRUE);
  dictate_engine_start (self->engine);
}

static void
on_talk_reply (DictateTalk *talk, const char *text, Window *self)
{
  if (!self->reply)
    return;
  /* a tool's line stays until the reply has words */
  if (!*text && gtk_widget_has_css_class (self->reply, "dictate-bubble-status"))
    return;
  gtk_widget_remove_css_class (self->reply, "dictate-bubble-status");
  gtk_label_set_text (GTK_LABEL (self->reply), *text ? text : "…");
  talk_follow (self);
}

/* a tool runs: what it does in the reply's bubble ("Checking your calendar…"), until the reply writes */
static void
on_talk_status (DictateTalk *talk, const char *text, Window *self)
{
  if (!self->reply)
    return;
  if (text)
    {
      gtk_label_set_text (GTK_LABEL (self->reply), text);
      gtk_widget_add_css_class (self->reply, "dictate-bubble-status");
    }
  else if (gtk_widget_has_css_class (self->reply, "dictate-bubble-status"))
    {
      gtk_widget_remove_css_class (self->reply, "dictate-bubble-status");
      gtk_label_set_text (GTK_LABEL (self->reply), "…");
    }
}

/* ---- a proposed action: a card with exactly what will happen, and its button; nothing runs without the tap ---- */

typedef struct {
  DictateAction *action;
  GtkWidget *buttons, *state;   /* the card's: its row of buttons, and what became of it */
} Card;

static void
card_free (gpointer data)
{
  Card *card = data;
  dictate_action_unref (card->action);
  g_free (card);
}

static void
card_settle (Card *card, const char *state)
{
  gtk_widget_set_visible (card->buttons, FALSE);
  gtk_label_set_text (GTK_LABEL (card->state), state);
  gtk_widget_set_visible (card->state, TRUE);
}

static void
on_card_done (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *widget = data;
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");
  g_autoptr (GError) error = NULL;
  g_autofree char *done = dictate_action_run_finish (card->action, result, &error);

  card_settle (card, done ? done : error->message);
  g_object_unref (widget);
}

static void
on_card_do (VibeosPressable *button, GtkWidget *widget)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");

  gtk_widget_set_sensitive (card->buttons, FALSE);
  dictate_action_run (card->action, NULL, on_card_done, g_object_ref (widget));
}

static void
on_card_cancel (VibeosPressable *button, GtkWidget *widget)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");

  dictate_action_cancel (card->action);
  card_settle (card, _("Cancelled"));
}

static void
on_talk_propose (DictateTalk *talk, DictateAction *action, Window *self)
{
  GtkWidget *widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *title = gtk_label_new (dictate_action_get_title (action));
  GtkWidget *cancel = vibeos_text_button_new (_("Cancel"));
  GtkWidget *go = vibeos_text_button_new (dictate_action_get_button (action));
  const char *const *lines = dictate_action_get_lines (action);
  Card *card = g_new0 (Card, 1);

  card->action = dictate_action_ref (action);
  gtk_widget_add_css_class (widget, "dictate-card");
  gtk_widget_set_halign (widget, GTK_ALIGN_FILL);
  gtk_widget_add_css_class (title, "dictate-card-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_box_append (GTK_BOX (widget), title);
  for (guint i = 0; lines && lines[i]; i++)
    {
      GtkWidget *line = gtk_label_new (lines[i]);

      gtk_label_set_wrap (GTK_LABEL (line), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (line), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_xalign (GTK_LABEL (line), 0);
      gtk_widget_add_css_class (line, "dictate-card-line");
      gtk_box_append (GTK_BOX (widget), line);
    }
  card->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (card->buttons, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (card->buttons), cancel);
  gtk_box_append (GTK_BOX (card->buttons), go);
  gtk_box_append (GTK_BOX (widget), card->buttons);
  card->state = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (card->state), 1);
  gtk_widget_add_css_class (card->state, "dictate-card-state");
  gtk_widget_set_visible (card->state, FALSE);
  gtk_box_append (GTK_BOX (widget), card->state);
  g_object_set_data_full (G_OBJECT (widget), "dictate-card", card, card_free);
  g_signal_connect (go, "clicked", G_CALLBACK (on_card_do), widget);
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_card_cancel), widget);
  gtk_box_append (GTK_BOX (self->talk_list), widget);
  talk_follow (self);
}

static void
on_talk_state (DictateTalk *talk, Window *self)
{
  DictateTalkState state = dictate_talk_get_state (talk), was = self->talk_state;

  self->talk_state = state;
  set_inhibit (self, state != DICTATE_TALK_IDLE || dictate_engine_get_state (self->engine) != DICTATE_IDLE);
  if (state == DICTATE_TALK_IDLE && self->reply)
    {
      /* the reply is over: nothing written of it, its bubble goes */
      const char *text = gtk_label_get_text (GTK_LABEL (self->reply));
      if (g_strcmp0 (text, "…") == 0 || gtk_widget_has_css_class (self->reply, "dictate-bubble-status"))
        gtk_box_remove (GTK_BOX (self->talk_list), self->reply);
      else
        talk_name (self->reply, FALSE);
      self->reply = NULL;
      /* spoken to its end: the user's turn */
      if (was == DICTATE_TALK_SPEAKING && !self->stopping_reply && self->talking &&
          g_settings_get_boolean (self->settings, "talk-listen-again") &&
          dictate_engine_get_state (self->engine) == DICTATE_IDLE && gtk_widget_get_mapped (self->window))
        talk_listen (self);
    }
  show_state (self);
}

static void
on_talk_error (DictateTalk *talk, const char *message, Window *self)
{
  vibeos_snackbar_show (self->snackbar, message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
}

/* the FAB in Talk: idle, it listens; otherwise it is the stop square and stops everything: the reply being written
 * or spoken, and the listening, whose words so far are dropped (not sent); then nothing listens until the next tap */
static void
talk_mic (Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);
  gboolean replying = dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE;

  if (!replying && state == DICTATE_IDLE)
    {
      talk_listen (self);
      return;
    }
  if (replying)
    {
      self->stopping_reply = TRUE;
      dictate_talk_stop (self->talk);
      self->stopping_reply = FALSE;
    }
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  show_state (self);
}

/* a new conversation: the bubbles go, and the model forgets */
static void
talk_clear (Window *self)
{
  GtkWidget *child;

  self->stopping_reply = TRUE;
  dictate_talk_reset (self->talk);
  self->stopping_reply = FALSE;
  if (dictate_engine_get_state (self->engine) != DICTATE_IDLE)
    dictate_engine_stop (self->engine);
  self->mine = self->reply = NULL;
  while ((child = gtk_widget_get_first_child (self->talk_list)))
    gtk_box_remove (GTK_BOX (self->talk_list), child);
  show_state (self);
}

static void
on_state_changed (DictateEngine *engine, Window *self)
{
  DictateState state = dictate_engine_get_state (engine);

  set_inhibit (self, state != DICTATE_IDLE ||
                     (self->talk && dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE));
  if (state == DICTATE_IDLE && self->talking)
    {
      if (self->close_when_idle)
        {
          gtk_window_destroy (GTK_WINDOW (self->window));
          return;
        }
      if (self->restart_when_idle)
        {
          /* the language changed while listening: what was said is dropped, listening goes on in the new one */
          self->restart_when_idle = FALSE;
          talk_listen (self);
          return;
        }
      if (self->drop_when_idle)
        {
          self->drop_when_idle = FALSE;
          talk_drop (self);
          show_state (self);
          return;
        }
      talk_send (self);
    }
  else if (state == DICTATE_IDLE)
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
  dictate_engine_set_turns (self->engine, FALSE);
  dictate_engine_start (self->engine);
}

static void
on_mic (DictateMic *mic, Window *self)
{
  if (self->talking)
    {
      talk_mic (self);
      return;
    }
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
  if (self->talking)
    talk_clear (self);
  else
    gtk_text_buffer_set_text (self->buffer, "", -1);
}

/* the bin: asked first, in Dictate the text goes, in Agent the conversation (and the model forgets it) */
static void
on_clear (VibeosPressable *button, Window *self)
{
  VibeosDialog *dialog;

  dialog = vibeos_dialog_new (self->talking ? _("Clear the conversation?") : _("Clear the text?"));

  vibeos_dialog_set_message (dialog, self->talking ? _("All of the conversation is deleted, and Agent forgets it.")
                                                   : _("All of the text is deleted."));
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

/* Agent's speaker: the replies spoken, or only written; kept in the settings */
static void
show_speak (Window *self)
{
  gboolean speak = dictate_talk_get_speak (self->talk);

  gtk_image_set_from_icon_name (GTK_IMAGE (vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->speak))),
                                speak ? "dictate-speak-symbolic" : "dictate-speak-off-symbolic");
  gtk_accessible_update_state (GTK_ACCESSIBLE (self->speak), GTK_ACCESSIBLE_STATE_PRESSED,
                               speak ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

/* the setting, from the button or from Agent's settings */
static void
on_speak_changed (GSettings *settings, const char *key, Window *self)
{
  gboolean speak = g_settings_get_boolean (settings, "talk-speak");

  if (speak == dictate_talk_get_speak (self->talk))
    return;
  /* stopped by the user: it does not go on to listen on its own */
  self->stopping_reply = TRUE;
  dictate_talk_set_speak (self->talk, speak);
  self->stopping_reply = FALSE;
  show_speak (self);
}

static void
on_speak (VibeosPressable *button, Window *self)
{
  gboolean speak = !dictate_talk_get_speak (self->talk);

  g_settings_set_boolean (self->settings, "talk-speak", speak);
  on_speak_changed (self->settings, "talk-speak", self);
  vibeos_snackbar_show (self->snackbar, speak ? _("Replies are spoken") : _("Replies are only written"), NULL, NULL,
                        NULL, VIBEOS_SNACKBAR_SHORT);
}

/* ⋮: About, VibeOS's About sheet, as every VibeOS app has it */
static const VibeosAboutSection dictate_legal[] = {
  { "Moonshine", "English, German and Spanish are heard by <a href=\"https://github.com/moonshine-ai/moonshine\">"
                 "Moonshine</a> and its tiny streaming models, under the MIT License, on Microsoft's ONNX Runtime "
                 "(MIT)." },
  { "Vosk", "French, Italian and Russian are heard by <a href=\"https://alphacephei.com/vosk/\">Vosk</a> and its "
            "small models, under the Apache License 2.0." },
  { "Downloads", "A language that is not installed can be downloaded in the app: its model comes from Moonshine's "
                 "server (download.moonshine.ai) or Vosk's (alphacephei.com), the same files as the language "
                 "packages', and each file is checked against its SHA-256 before it is used." },
  { NULL }
};

static const VibeosAboutInfo dictate_about = { .name = "Dictate", .icon_name = "vibeos.Dictate",
                                               .legal = dictate_legal };

static const VibeosAboutSection agent_legal[] = {
  { "Hearing", "What is said is heard on the phone: English, German and Spanish by "
               "<a href=\"https://github.com/moonshine-ai/moonshine\">Moonshine</a> and its tiny streaming models (MIT "
               "License), French, Italian and Russian by <a href=\"https://alphacephei.com/vosk/\">Vosk</a> and its "
               "small models (Apache License 2.0), on Microsoft's ONNX Runtime (MIT)." },
  { "Replies", "The replies are written by a language model on VibeOS's server, which the phone reaches with its "
               "own device id, or by a GGUF model on the phone itself with "
               "<a href=\"https://github.com/ggml-org/llama.cpp\">llama.cpp</a> (MIT License)." },
  { "Voice", "The replies are spoken on the phone by a <a href=\"https://github.com/rhasspy/piper\">Piper</a> voice "
             "(MIT License), its sounds from <a href=\"https://github.com/espeak-ng/espeak-ng\">eSpeak NG</a> (GNU "
             "General Public License 3.0 or later), and the end of what is said is found by the "
             "<a href=\"https://github.com/snakers4/silero-vad\">Silero VAD</a> (MIT License). The English voice, "
             "Lessac, is trained on the Lessac recordings of the Blizzard Challenge 2013, which are under the "
             "<a href=\"https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html\">"
             "Blizzard 2013 Lessac licence</a>: research use, no commercial use." },
  { "Downloads", "A language that is not installed can be downloaded in the app, each file checked against its "
                 "SHA-256 before it is used: its model from Moonshine's server (download.moonshine.ai) or Vosk's "
                 "(alphacephei.com), and its voice from <a href=\"https://huggingface.co/rhasspy/piper-voices\">"
                 "rhasspy/piper-voices</a> (MIT License). The voices' recordings: Thorsten-Voice (German), Davefx "
                 "(Spanish), Paola (Italian) and Denis (Russian), CC0; Lessac (English), the Blizzard 2013 Lessac "
                 "licence (research use, no commercial use); the SIWIS "
                 "French speech synthesis database, CC BY 4.0. The French and Italian voices are trained from the "
                 "Hi-Fi TTS voice of Cori Samuel (NVIDIA, CC BY 4.0)." },
  { NULL }
};

static const VibeosAboutInfo agent_about = { .name = "Agent", .icon_name = "vibeos.Agent", .legal = agent_legal };

/* --- Agent's Settings, a page over the conversation (dictate-settings.h) */

static const char *
settings_language (gpointer data)
{
  Window *self = data;

  return dictate_engine_get_language (self->engine);
}

static void
settings_choose_language (gpointer data)
{
  on_language (NULL, data);
}

static void
settings_back (gpointer data)
{
  Window *self = data;

  vibeos_page_stack_pop (VIBEOS_PAGE_STACK (self->pages));
}

static void
open_settings (gpointer data)
{
  Window *self = data;
  DictateSettingsHooks hooks = {
    .settings = self->settings, .talk = self->talk, .language = settings_language,
    .choose_language = settings_choose_language, .back = settings_back, .data = self,
  };

  if (vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (self->pages)) > 1)
    return;
  vibeos_page_stack_push (VIBEOS_PAGE_STACK (self->pages), dictate_settings_page_new (self->window, &hooks));
}

/* the shell's back gesture asks the accessibility tree: a dialog showing means "send Escape", nothing means "leave
 * the app". So the marker shows while a page is over the conversation, and Escape takes that page off. */
static void
on_top_changed (VibeosPageStack *pages, Window *self)
{
  gtk_widget_set_visible (self->marker, vibeos_page_stack_get_depth (pages) > 1);
}

static gboolean
on_back_key (GtkWidget *widget, GVariant *args, gpointer data)
{
  Window *self = data;

  return vibeos_page_stack_pop (VIBEOS_PAGE_STACK (self->pages));
}

static void
on_more (VibeosPressable *button, Window *self)
{
  VibeosMenu *menu = vibeos_menu_new ();

  if (self->talking)
    vibeos_menu_append (menu, _("Settings"), open_settings, self);
  vibeos_menu_append_about (menu, self->talking ? &agent_about : &dictate_about);
  vibeos_menu_present (menu, GTK_WIDGET (button));
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
  if (bottom.y < 0 || top.y > page_height - self->fab_row)
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
  y = CLAMP (y, BAR_GAP, MAX (BAR_GAP, page_height - self->fab_row - height));
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
  gtk_widget_set_margin_bottom (self->mic, (self->fab_row - FAB_SIZE) / 2 + (int) vibeos_snackbar_get_lift (self->snackbar));
}

static gboolean
on_close_request (GtkWindow *window, Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);

  if (self->talk)
    {
      self->stopping_reply = TRUE;
      dictate_talk_stop (self->talk);
      self->stopping_reply = FALSE;
    }
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

/* the document's margins: the phone's, or wider so that the text keeps to a readable column in the middle; and the
 * FAB's row, higher on a tablet */
static void
update_margins (Window *self)
{
  VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (self->window));
  int margin = MAX (TEXT_MARGIN, (vibeos_layout_get_width (layout) - COLUMN_MAX) / 2);

  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->view), margin);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->view), margin);
  gtk_widget_set_margin_start (self->talk_list, margin - 8);
  gtk_widget_set_margin_end (self->talk_list, margin - 8);

  self->fab_row = vibeos_layout_is_tablet (layout) ? FAB_ROW_TABLET : FAB_ROW;
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->view), self->fab_row + 16);
  gtk_widget_set_margin_bottom (self->talk_list, self->fab_row + 16);
  gtk_widget_set_margin_bottom (self->mic, (self->fab_row - FAB_SIZE) / 2 + (int) vibeos_snackbar_get_lift (self->snackbar));
}

/* the theme changed: the words still being recognised in its colour */
static void
update_partial (Window *self)
{
  GdkRGBA variant = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);

  g_object_set (self->partial, "foreground-rgba", &variant, NULL);
}

static void
window_free (gpointer data)
{
  Window *self = data;

  /* the window's layout goes with the window; the theme stays */
  g_signal_handlers_disconnect_by_data (vibeos_theme_get_default (), self);
  g_clear_handle_id (&self->save_id, g_source_remove);
  g_clear_handle_id (&self->place_id, g_source_remove);
  g_signal_handlers_disconnect_by_data (self->engine, self);
  dictate_engine_stop (self->engine);
  g_clear_object (&self->engine);
  if (self->talk)
    g_signal_handlers_disconnect_by_data (self->talk, self);
  g_clear_object (&self->talk);
  g_string_free (self->heard, TRUE);
  g_free (self->said);
  g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  g_free (self->speaking);
  g_free (self->path);
  g_free (self);
}

GtkWidget *
dictate_window_new (GtkApplication *app, gboolean agent)
{
  Window *self = g_new0 (Window, 1);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *title;
  GtkWidget *page = self->page = gtk_overlay_new ();
  GtkTextIter start;

  self->window = vibeos_window_new (app);
  self->engine = dictate_engine_new ();
  self->talking = agent;
  self->talk = agent ? dictate_talk_new () : NULL;
  self->heard = g_string_new (NULL);
  self->settings = g_settings_new ("vibeos.Dictate");
  self->path = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "document.txt", NULL);
  g_object_set_data_full (G_OBJECT (self->window), "dictate-window", self, window_free);
  gtk_window_set_title (GTK_WINDOW (self->window), agent ? _("Agent") : _("Dictate"));

  /* the app bar */
  title = self->title = gtk_label_new (agent ? _("Agent") : _("Dictate"));
  gtk_widget_add_css_class (bar, "vibeos-app-bar");
  gtk_widget_add_css_class (title, "vibeos-app-bar-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  vibeos_label_set_font_padding_sp (title, 22);
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
  if (agent)
    {
      self->speak = action ("dictate-speak-symbolic", _("Speak the replies"), G_CALLBACK (on_speak), self);
      gtk_box_append (GTK_BOX (bar), self->speak);
    }
  self->copy = action ("dictate-copy-symbolic", _("Copy"), G_CALLBACK (on_copy), self);
  gtk_box_append (GTK_BOX (bar), self->copy);
  self->clear = action ("dictate-delete-symbolic", _("Clear"), G_CALLBACK (on_clear), self);
  gtk_box_append (GTK_BOX (bar), self->clear);
  {
    /* Material's ⋮, as the other actions' glyphs (Adwaita's view-more is a size larger beside them) */
    GtkWidget *more = action ("dictate-more-symbolic", _("More options"), G_CALLBACK (on_more), self);

    gtk_widget_add_css_class (more, "dictate-bar-end");
    gtk_box_append (GTK_BOX (bar), more);
  }
  gtk_box_append (GTK_BOX (content), bar);

  /* the document */
  self->view = gtk_text_view_new ();
  self->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->view));
  gtk_widget_add_css_class (self->view, "dictate-document");
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->view), TEXT_MARGIN);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->view), TEXT_MARGIN);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (self->view), 8);
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
  if (!agent)
    load (self);
  g_signal_connect (self->buffer, "changed", G_CALLBACK (on_changed), self);
  g_signal_connect (self->buffer, "mark-set", G_CALLBACK (on_mark_set), self);

  self->scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->view);
  gtk_widget_set_vexpand (self->scroller, TRUE);

  /* Talk's conversation */
  self->talk_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (self->talk_list, "dictate-talk");
  gtk_widget_set_valign (self->talk_list, GTK_ALIGN_END);
  gtk_widget_set_margin_top (self->talk_list, 8);
  self->talk_scroller = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->talk_scroller), self->talk_list);
  gtk_widget_set_vexpand (self->talk_scroller, TRUE);

  self->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (self->stack), self->scroller, "write");
  gtk_stack_add_named (GTK_STACK (self->stack), self->talk_scroller, "talk");
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), agent ? "talk" : "write");
  gtk_overlay_set_child (GTK_OVERLAY (page), self->stack);
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
  g_signal_connect (self->mic, "clicked", G_CALLBACK (on_mic), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->mic);

  self->snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());
  g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_lift), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), GTK_WIDGET (self->snackbar));

  gtk_box_append (GTK_BOX (content), page);
  {
    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkEventController *shortcuts = gtk_shortcut_controller_new ();
    GtkShortcutAction *back = gtk_callback_action_new (on_back_key, self, NULL);

    self->pages = vibeos_page_stack_new ();
    gtk_widget_set_vexpand (self->pages, TRUE);
    vibeos_page_stack_set_root (VIBEOS_PAGE_STACK (self->pages), content);
    g_signal_connect (self->pages, "top-changed", G_CALLBACK (on_top_changed), self);
    gtk_box_append (GTK_BOX (outer), self->pages);
    self->marker = g_object_new (GTK_TYPE_BOX, "accessible-role", GTK_ACCESSIBLE_ROLE_DIALOG, NULL);
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->marker), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    agent ? _("Agent") : _("Dictate"), -1);
    gtk_widget_set_can_target (self->marker, FALSE);
    gtk_widget_set_visible (self->marker, FALSE);
    gtk_box_append (GTK_BOX (outer), self->marker);
    vibeos_window_set_content (GTK_WINDOW (self->window), outer);
    /* Back, Escape, Alt+Left: the page over the conversation goes */
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                                            g_object_ref (back)));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Left, GDK_ALT_MASK),
                                                            g_object_ref (back)));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Back, 0), back));
    gtk_widget_add_controller (self->window, shortcuts);
  }

  g_signal_connect (self->engine, "text", G_CALLBACK (on_text), self);
  g_signal_connect (self->engine, "state-changed", G_CALLBACK (on_state_changed), self);
  g_signal_connect (self->engine, "error", G_CALLBACK (on_error), self);
  if (self->talk)
    {
      g_signal_connect (self->talk, "reply", G_CALLBACK (on_talk_reply), self);
      g_signal_connect (self->talk, "state-changed", G_CALLBACK (on_talk_state), self);
      g_signal_connect (self->talk, "error", G_CALLBACK (on_talk_error), self);
      g_signal_connect (self->talk, "status", G_CALLBACK (on_talk_status), self);
      g_signal_connect (self->talk, "propose", G_CALLBACK (on_talk_propose), self);
      dictate_talk_set_speed (self->talk, g_settings_get_double (self->settings, "talk-speed"));
      dictate_talk_set_speak (self->talk, g_settings_get_boolean (self->settings, "talk-speak"));
      show_speak (self);
      g_signal_connect (self->settings, "changed::talk-speak", G_CALLBACK (on_speak_changed), self);
    }
  g_signal_connect (self->window, "close-request", G_CALLBACK (on_close_request), self);
  g_signal_connect_swapped (vibeos_layout_get_for_window (GTK_WINDOW (self->window)), "changed",
                            G_CALLBACK (update_margins), self);
  g_signal_connect_swapped (vibeos_theme_get_default (), "changed", G_CALLBACK (update_partial), self);
  update_margins (self);
  /* and Agent's model and voice load while the window comes up (language_changed): the first answer is sooner */
  choose_language (self);
  show_state (self);
  return self->window;
}
