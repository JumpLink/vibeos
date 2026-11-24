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
 * reply ends where it is, what was heard and not yet sent is dropped, and nothing listens until the next tap.
 *
 * A call that rings is said ("Call from Anna. Answer or decline?") when the app says so (dictate-main.c,
 * dictate_calls_may_announce ()), and then Agent listens once for what to do, whether or not the window is on the
 * screen; a window made only for that (never shown) closes once the call has stopped ringing and all is quiet. */
#include "dictate-window.h"
#include "dictate-calls.h"
#include "dictate-cues.h"
#include "dictate-download-dialog.h"
#include "dictate-enroll.h"
#include "dictate-engine.h"
#include "dictate-history.h"
#include "dictate-mic.h"
#include "dictate-models.h"
#include "dictate-onboarding.h"
#include "dictate-output.h"
#include "dictate-owner.h"
#include "dictate-settings.h"
#include "dictate-stage.h"
#include "dictate-talk.h"
#include "dictate-text.h"
#include "dictate-offline.h"
#include "dictate-tools.h"
#include "dictate-voicematch.h"

#include <errno.h>
#include <string.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
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
  /* Vibe */
  GtkWidget *stage;
  DictateTalk *talk;
  gboolean talking;             /* Vibe's window: the conversation, not the document */
  GString *heard;               /* the user's finished sentences so far */
  char *said;                   /* and the one being spoken */
  /* the exchange on the screen: what was said and its speech, the reply so far, what became of it */
  char *ex_said;
  GBytes *ex_audio;
  GString *ex_reply;
  char *ex_note;
  gboolean ex_refused;          /* not carried out for the voice */
  guint ex_refused_reason;      /* why: DictateTalk's "refused" reason */
  gboolean ex_record_again;     /* refused as unchecked, and a new sample would let it be checked */
  gboolean ex_model_gone;       /* refused as unchecked, and Voice match's model is not installed */
  gboolean ex_done, ex_undo;    /* something was done in it, and can be undone */
  char *ex_access;              /* it needed what this access-* switch keeps off */
  guint intro_id;               /* the introduction about to be shown */
  guint refusals;               /* commands not carried out for the voice, one after the other */
  gboolean ex_opened;           /* I5: an app was opened in this exchange, and is over Vibe now */
  char **ex_suggestions;        /* K2: replies to tap under the answer */
  char *ex_person;              /* the initials of whom the answer is about, for the orb */
  gboolean ex_choices;          /* F1: rows to pick from are in the middle */
  gboolean help, help_offline;  /* G4: what can be said, on the screen (offline: the fixed commands) */
  guint countdown_s, countdown_id; /* K5: the card goes by itself after so many seconds, and its tick */
  gint64 countdown_end;         /* when, monotonic us (0: not counting yet) */
  guint reply_flags;            /* "reply-info"'s (dictate-pending.h) */
  gboolean answering;           /* its reply is under way */
  gboolean silence;             /* a turn asked for ended with nothing said */
  GtkWidget *card;              /* a proposed action on the screen, until it is settled */
  GPtrArray *history;           /* the earlier exchanges (DictateHistoryEntry), oldest first */
  GCancellable *confirming;     /* the lock screen asked for the PIN */
  gboolean stopping_reply;      /* the user stopped the reply: it does not listen again on its own */
  gboolean reply_failed;        /* the reply going on failed: its end is no "done" */
  DictateTalkState talk_state;  /* the last one seen */
  guint ringing;                /* the call being announced, while it rings */
  gboolean announcing;          /* the announcement is being said: then Agent listens for the answer */
  gboolean answer_listening;    /* and listens for it */
  gboolean held;                /* the power key is held (dictate_window_hold): no pause ends the turn */
  gboolean hold_turn;           /* the last turn was the power key's: no listening again after the reply */
  GCancellable *caller;         /* the caller being looked up */
  /* F3: the call going on (or on hold, or being placed), shown when nothing else is */
  guint call_on;                /* its id; 0 for none */
  gint64 call_connected;        /* when it was answered, Unix ms; 0 not yet */
  DictateCallState call_state;
  char *call_info;              /* get_current_call's result for it */
  GCancellable *call_fetch;     /* that being asked */
  guint call_tick_id;           /* the length shown, by the second */
  GtkWidget *call_banner;       /* the call over an exchange, under the bar (weak) */
  GtkWidget *call_banner_text;  /* its "On a call · 02:41" (weak) */
  char *call_banner_key;        /* what the banner shows (name, mute, hold): built again only when that changes */
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

static void vibe_show (Window *self);

static void
show_state (Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);
  gboolean listening = state == DICTATE_STARTING || state == DICTATE_LISTENING;
  gboolean empty;

  if (self->talking)
    {
      vibe_show (self);
      return;
    }
  empty = gtk_text_buffer_get_char_count (self->buffer) == 0;
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
  GtkWidget *label;
  g_autofree char *code = language ? g_ascii_strup (language->code, -1) : NULL;
  /* TRANSLATORS: the language button's name for screen readers; %s is the language's name for itself */
  g_autofree char *name = language ? g_strdup_printf (_("Language: %s"), language->name) : NULL;

  /* Vibe's settings say it (Vibe has no language button: its settings have the language) */
  if (g_object_get_data (G_OBJECT (self->window), "dictate-settings-page"))
    dictate_settings_page_refresh (g_object_get_data (G_OBJECT (self->window), "dictate-settings-page"));
  if (!self->language)
    return;
  /* without a model there is nothing to choose: the microphone says what to install */
  gtk_widget_set_visible (self->language, language != NULL);
  if (!language)
    return;
  label = vibeos_pressable_get_child (VIBEOS_PRESSABLE (self->language));
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

/* --- Vibe: the conversation on the stage (dictate-stage.h) */

static void talk_listen (Window *self);
static void maybe_close (Window *self);
static void vibe_show (Window *self);
static gboolean asks_for_help (const char *said);
static void vibe_help (Window *self, gboolean offline);

/* a proposed action's card (below): the action, and the line that says what became of it */
typedef struct {
  DictateAction *action;
  GtkWidget *state;             /* what became of it */
  GtkWidget *go;                /* its button, while it is on the stage (weak) */
} Card;

static void countdown_stop (Window *self);
static void countdown_wait (Window *self);
static gboolean card_said (Window *self, const char *said, GBytes *audio);

/* the exchange on the screen goes up among the earlier ones, and the page is new */
static void
vibe_archive (Window *self)
{
  DictateHistoryEntry *entry;

  if (self->card)
    {
      /* a proposal nobody tapped: it is dropped, nothing runs */
      Card *card = g_object_get_data (G_OBJECT (self->card), "dictate-card");

      countdown_stop (self);
      dictate_action_cancel (card->action);
      if (!self->ex_note)
        self->ex_note = g_strdup (_("Not carried out"));
      self->card = NULL;
    }
  if (self->ex_said || self->ex_reply->len)
    {
      entry = g_new0 (DictateHistoryEntry, 1);
      entry->said = g_strdup (self->ex_said);
      entry->reply = g_strdup (self->ex_reply->str);
      entry->note = g_strdup (self->ex_note);
      entry->refused = self->ex_refused;
      g_ptr_array_add (self->history, entry);
      dictate_stage_add_past (DICTATE_STAGE (self->stage), entry->said, entry->reply, entry->note);
      dictate_history_save (self->history);
    }
  g_clear_pointer (&self->ex_said, g_free);
  g_string_truncate (self->ex_reply, 0);
  g_clear_pointer (&self->ex_note, g_free);
  g_clear_pointer (&self->ex_audio, g_bytes_unref);
  g_clear_pointer (&self->ex_access, g_free);
  self->ex_refused = self->ex_done = self->ex_undo = self->silence = self->help = self->ex_opened = FALSE;
  g_clear_pointer (&self->ex_suggestions, g_strfreev);
  g_clear_pointer (&self->ex_person, g_free);
  self->ex_choices = FALSE;
  self->reply_flags = 0;
  dictate_stage_reset (DICTATE_STAGE (self->stage));
}

/* the earlier exchanges, as they were when the app last closed: on the screen, and the model told of them */
static void
vibe_restore (Window *self)
{
  g_autoptr (GArray) exchanges = g_array_new (FALSE, FALSE, sizeof (DictateExchange));

  g_ptr_array_unref (self->history);
  self->history = dictate_history_load ();
  for (guint i = 0; i < self->history->len; i++)
    {
      DictateHistoryEntry *entry = self->history->pdata[i];

      dictate_stage_add_past (DICTATE_STAGE (self->stage), entry->said, entry->reply, entry->note);
      if (!entry->refused && entry->said && entry->reply)
        g_array_append_val (exchanges, ((DictateExchange) { entry->said, entry->reply }));
    }
  if (exchanges->len)
    dictate_talk_restore (self->talk, (DictateExchange *) exchanges->data, exchanges->len);
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
  g_autofree char *tidy_partial = NULL;

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
  if (!self->heard->len && !(partial && *partial))
    return;
  /* words over a card: its countdown stops; they may be "send" or "cancel", so its exchange stays until they end */
  if (self->card)
    countdown_wait (self);
  /* the first words of a new turn: the exchange that was on the screen goes up */
  else if (self->ex_said || self->ex_reply->len)
    vibe_archive (self);
  if (partial && *partial)
    tidy_partial = dictate_text_tidy (partial, dictate_text_ends_sentence (self->heard->str), self->speaking);
  dictate_stage_set_who (DICTATE_STAGE (self->stage), self->held ? _("Listening while you hold") : _("Listening"),
                         FALSE);
  dictate_stage_set_heard (DICTATE_STAGE (self->stage), self->heard->str, tidy_partial);
}

/* the user stopped the listening: what was heard is not sent */
static void
talk_drop (Window *self)
{
  g_string_truncate (self->heard, 0);
  g_clear_pointer (&self->said, g_free);
  if (!self->ex_said && !self->ex_reply->len)
    dictate_stage_reset (DICTATE_STAGE (self->stage));
}

/* `text` to the model, with the turn's speech for the voice match when there is one */
static void
vibe_ask (Window *self, const char *text, GBytes *audio, DictateAskFlags flags)
{
  const char *language = dictate_engine_get_language (self->engine);

  self->answering = TRUE;
  self->reply_failed = FALSE;
  dictate_orb_set_look (dictate_stage_get_orb (DICTATE_STAGE (self->stage)), DICTATE_ORB_WORKING, NULL);
  dictate_cue_play (self->settings, DICTATE_CUE_WORKING);
  dictate_talk_ask_full (self->talk, language, text, audio, flags);
}

/* the exchange on the screen asked again, as it was said: Try again, or the PIN given for it */
static void
vibe_ask_again (Window *self, DictateAskFlags flags)
{
  g_autofree char *said = g_strdup (self->ex_said);
  g_autoptr (GBytes) audio = self->ex_audio ? g_bytes_ref (self->ex_audio) : NULL;

  if (!said)
    return;
  g_string_truncate (self->ex_reply, 0);
  g_clear_pointer (&self->ex_note, g_free);
  g_clear_pointer (&self->ex_access, g_free);
  self->ex_refused = self->ex_done = self->ex_undo = self->help = self->ex_opened = FALSE;
  g_clear_pointer (&self->ex_suggestions, g_strfreev);
  g_clear_pointer (&self->ex_person, g_free);
  self->ex_choices = FALSE;
  self->reply_flags = 0;
  dictate_stage_set_middle (DICTATE_STAGE (self->stage), NULL);
  dictate_stage_set_who (DICTATE_STAGE (self->stage), NULL, FALSE);
  dictate_stage_set_text (DICTATE_STAGE (self->stage), NULL, NULL);
  dictate_stage_set_written (DICTATE_STAGE (self->stage), NULL);
  dictate_stage_clear_buttons (DICTATE_STAGE (self->stage));
  vibe_ask (self, said, audio, flags);
  show_state (self);
}

/* listening has ended: what was said goes to the model; with nothing said after a tap, Vibe says it heard nothing */
/* `said` asked as a new exchange: the one on the screen goes up among the earlier ones */
static void
vibe_send (Window *self, const char *said, GBytes *audio, DictateAskFlags flags)
{
  if (self->ex_said || self->ex_reply->len)
    vibe_archive (self);
  self->ex_said = g_strdup (said);
  self->ex_audio = audio ? g_bytes_ref (audio) : NULL;
  dictate_stage_set_said (DICTATE_STAGE (self->stage), said);
  dictate_stage_set_who (DICTATE_STAGE (self->stage), NULL, FALSE);
  dictate_stage_set_text (DICTATE_STAGE (self->stage), NULL, NULL);
  if (asks_for_help (said))
    {
      vibe_help (self, FALSE);
      return;
    }
  vibe_ask (self, said, audio, flags);
}

static void
talk_send (Window *self)
{
  g_autofree char *said = talk_said (self);
  g_autoptr (GBytes) audio = dictate_engine_take_turn_audio (self->engine);

  g_string_truncate (self->heard, 0);
  g_clear_pointer (&self->said, g_free);
  /* a card waits: nothing said leaves it as it is; "send" and "cancel" are its */
  if (self->card && (!*g_strstrip (said) || card_said (self, said, audio)))
    {
      vibe_show (self);
      return;
    }
  if (!*g_strstrip (said))
    {
      /* listening on after a reply, the reply stays; a turn of its own asked for, it heard nothing */
      self->silence = !self->ex_said && !self->ex_reply->len && !self->hold_turn;
      if (!self->ex_said && !self->ex_reply->len)
        dictate_stage_reset (DICTATE_STAGE (self->stage));
      return;
    }
  vibe_send (self, said, audio, DICTATE_ASK_NONE);
}

/* listening for a turn, whose end is found in what is said: a tap's, or the power key's, which no pause ends while
 * the key is held (dictate_window_hold) */
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
  self->silence = FALSE;
  /* a turn of its own, from the start: the screen is the listening one at once */
  if (!self->ex_said && !self->ex_reply->len)
    {
      dictate_stage_reset (DICTATE_STAGE (self->stage));
      dictate_stage_set_who (DICTATE_STAGE (self->stage), _("Listening"), FALSE);
    }
  /* the cue plays while the microphone opens: it is over before the first words are heard */
  dictate_cue_play (self->settings, DICTATE_CUE_LISTENING);
  self->hold_turn = FALSE;
  dictate_engine_set_turns (self->engine, TRUE);
  dictate_engine_set_held (self->engine, self->held);
  dictate_engine_start (self->engine);
}

static void
on_talk_reply (DictateTalk *talk, const char *text, Window *self)
{
  if (!self->answering && !self->announcing)
    return;
  g_string_assign (self->ex_reply, text);
  /* a reply in this voice: the earlier refusals are over */
  if (!self->ex_refused)
    self->refusals = 0;
  dictate_stage_set_who (DICTATE_STAGE (self->stage),
                         !dictate_talk_get_speak (self->talk) ? _("Vibe writes")
                         : self->reply_flags & DICTATE_REPLY_PRIVATE_SPEAKER ? _("Vibe says · on the speaker")
                         : _("Vibe says"), FALSE);
  dictate_stage_set_reply (DICTATE_STAGE (self->stage), text);
  if (*text)
    dictate_stage_set_hint (DICTATE_STAGE (self->stage), NULL);
}

/* a tool runs: what it does ("Checking your calendar…") at the bottom, until the reply writes */
static void
on_talk_status (DictateTalk *talk, const char *text, Window *self)
{
  if (self->answering && !self->ex_reply->len)
    dictate_stage_set_hint (DICTATE_STAGE (self->stage), text);
}

/* ---- a proposed action: a card with exactly what will happen, and its button; nothing runs without the tap ---- */

static void
card_free (gpointer data)
{
  Card *card = data;
  g_clear_weak_pointer (&card->go);
  dictate_action_unref (card->action);
  g_free (card);
}

static void
countdown_stop (Window *self)
{
  g_clear_handle_id (&self->countdown_id, g_source_remove);
  self->countdown_s = 0;
  self->countdown_end = 0;
  dictate_stage_set_progress (DICTATE_STAGE (self->stage), -1);
}

/* "three": the seconds left, as the countdown says them */
static char *
countdown_words (guint left)
{
  static const char *const WORDS[] = {
    /* TRANSLATORS: the seconds left of a countdown, in "Sending in %s…" */
    N_("one"), N_("two"), N_("three"), N_("four"), N_("five"),
    N_("six"), N_("seven"), N_("eight"), N_("nine"), N_("ten"),
  };

  return left >= 1 && left <= G_N_ELEMENTS (WORDS) ? g_strdup (_(WORDS[left - 1])) : g_strdup_printf ("%u", left);
}

static void card_go (Window *self);

static gboolean
countdown_tick (gpointer data)
{
  Window *self = data;
  Card *card = self->card ? g_object_get_data (G_OBJECT (self->card), "dictate-card") : NULL;
  gint64 now = g_get_monotonic_time (), total = (gint64) self->countdown_s * G_USEC_PER_SEC;
  gint64 left = self->countdown_end - now;
  g_autofree char *words = NULL, *who = NULL;
  const char *go;

  /* the window gone from the foreground in the countdown: nothing goes by itself unseen; the card waits */
  if (card && (!gtk_widget_get_mapped (self->window) || !gtk_window_is_active (GTK_WINDOW (self->window))))
    {
      self->countdown_id = 0;
      countdown_wait (self);
      return G_SOURCE_REMOVE;
    }
  if (!card || left <= 0)
    {
      self->countdown_id = 0;
      if (card)
        card_go (self);
      return G_SOURCE_REMOVE;
    }
  words = countdown_words ((guint) ((left + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC));
  go = dictate_action_get_button (card->action);
  /* the action's own verb where it has one: "Sending in three…", "Calling in three…" */
  if (!g_strcmp0 (go, _("Send")))
    /* TRANSLATORS: over a message that is sent by itself when the countdown ends; %s is "three" */
    who = g_strdup_printf (_("Sending in %s…"), words);
  else if (!g_strcmp0 (go, _("Call")))
    /* TRANSLATORS: over a call that is placed by itself when the countdown ends; %s is "three" */
    who = g_strdup_printf (_("Calling in %s…"), words);
  else
    /* TRANSLATORS: over an action that goes ahead by itself when the countdown ends; %s is "three" */
    who = g_strdup_printf (_("Going ahead in %s…"), words);
  dictate_stage_set_who (DICTATE_STAGE (self->stage), who, FALSE);
  dictate_stage_set_progress (DICTATE_STAGE (self->stage), 1.0 - (double) left / (double) total);
  return G_SOURCE_CONTINUE;
}

/* the line under a card: what ends the countdown, or what it waits for; while Vibe listens, what may be said */
static void
card_hint (Window *self)
{
  Card *card = self->card ? g_object_get_data (G_OBJECT (self->card), "dictate-card") : NULL;
  DictateState state = dictate_engine_get_state (self->engine);
  gboolean listening = state == DICTATE_STARTING || state == DICTATE_LISTENING;
  g_autofree char *hint = NULL;

  if (!card)
    return;
  if (self->countdown_s)
    dictate_stage_set_hint (DICTATE_STAGE (self->stage), listening ? _("Say “cancel” to stop")
                                                                   : _("Tap Cancel to stop it"));
  else if (listening)
    dictate_stage_set_hint (DICTATE_STAGE (self->stage), _("Say “send” or “cancel”"));
  else
    {
      /* TRANSLATORS: under a proposed action; %s is its button's label ("Send") */
      hint = g_strdup_printf (_("Nothing happens until you tap “%s”"), dictate_action_get_button (card->action));
      dictate_stage_set_hint (DICTATE_STAGE (self->stage), hint);
    }
}

/* the countdown stopped before its end (speech, a touch, the window leaving the foreground): the card waits, its
 * button the action's own again */
static void
countdown_wait (Window *self)
{
  Card *card = self->card ? g_object_get_data (G_OBJECT (self->card), "dictate-card") : NULL;

  if (!card || !self->countdown_s)
    return;
  g_message ("window: the countdown stopped: the card waits");
  countdown_stop (self);
  dictate_stage_set_who (DICTATE_STAGE (self->stage), _("Vibe says"), FALSE);
  if (card->go)
    {
      gtk_label_set_label (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (card->go))),
                           dictate_action_get_button (card->action));
      gtk_accessible_update_property (GTK_ACCESSIBLE (card->go), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      dictate_action_get_button (card->action), -1);
    }
  card_hint (self);
}

/* the read-back spoken (or none to hear): the seconds start, with the window in the foreground */
static void
countdown_start (Window *self)
{
  if (!self->card || !self->countdown_s || self->countdown_end)
    return;
  /* the read-back failed, or the window is not in front: what would go was not heard or seen, so it waits */
  if (self->reply_failed || !gtk_widget_get_mapped (self->window)
      || !gtk_window_is_active (GTK_WINDOW (self->window)))
    {
      countdown_wait (self);
      return;
    }
  self->countdown_end = g_get_monotonic_time () + (gint64) self->countdown_s * G_USEC_PER_SEC;
  countdown_tick (self);
  if (self->countdown_end)
    self->countdown_id = g_timeout_add (100, countdown_tick, self);
}

/* the card's outcome: on it, in the exchange's history, and the orb's ✓ when it was done */
static void
card_settle (Window *self, GtkWidget *widget, const char *state, gboolean done)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");

  gtk_label_set_text (GTK_LABEL (card->state), state);
  gtk_widget_set_visible (card->state, TRUE);
  if (self->card != widget)
    return;
  countdown_stop (self);
  self->card = NULL;
  g_free (self->ex_note);
  self->ex_note = g_strdup (state);
  self->ex_done = done;
  dictate_stage_clear_buttons (DICTATE_STAGE (self->stage));
  vibe_show (self);
}

static void
on_card_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) widget = data;
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");
  GtkRoot *root = gtk_widget_get_root (widget);
  Window *self = root ? g_object_get_data (G_OBJECT (root), "dictate-window") : NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *done = dictate_action_run_finish (card->action, result, &error);

  if (self)
    card_settle (self, widget, done ? done : error->message, done != NULL);
}

/* the card's buttons go insensitive while it runs */
static void
card_running (Window *self, GtkWidget *widget)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");

  if (self && self->card == widget)
    countdown_stop (self);
  if (card->go)
    gtk_widget_set_sensitive (gtk_widget_get_parent (card->go), FALSE);
}

/* its button: Send, or "Send now" in the countdown */
static void
on_card_do (VibeosPressable *button, GtkWidget *widget)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");
  GtkRoot *root = gtk_widget_get_root (widget);

  card_running (root ? g_object_get_data (G_OBJECT (root), "dictate-window") : NULL, widget);
  dictate_action_run (card->action, NULL, on_card_done, g_object_ref (widget));
}

/* the countdown ran out */
static void
card_go (Window *self)
{
  Card *card = g_object_get_data (G_OBJECT (self->card), "dictate-card");
  GtkWidget *widget = self->card;

  g_message ("window: the countdown ran out: the action goes ahead");
  card_running (self, widget);
  dictate_action_run_by (card->action, DICTATE_ACTION_BY_COUNTDOWN, NULL, on_card_done, g_object_ref (widget));
}

/* "send" said to it: run once the voice has passed its check (dictate_talk_confirm_async) */
static void
on_card_confirmed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) widget = data;
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");
  GtkRoot *root = gtk_widget_get_root (widget);
  Window *self = root ? g_object_get_data (G_OBJECT (root), "dictate-window") : NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *done = NULL;

  if (!self)
    return;
  done = dictate_talk_confirm_finish (self->talk, result, &error);
  /* not the owner's voice ("refused" said why): the card is still there, and waits for the tap */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) && self->card == widget)
    {
      if (card->go)
        gtk_widget_set_sensitive (gtk_widget_get_parent (card->go), TRUE);
      card_hint (self);
      return;
    }
  card_settle (self, widget, done ? done : error->message, done != NULL);
}

/* a turn said while a card waits: "send" or "cancel" are its, anything else goes to the model (and the card with
 * the exchange) */
static gboolean
card_said (Window *self, const char *said, GBytes *audio)
{
  Card *card = g_object_get_data (G_OBJECT (self->card), "dictate-card");
  GtkWidget *widget = self->card;

  switch (dictate_offline_confirm_word (dictate_engine_get_language (self->engine), said))
    {
    case DICTATE_CONFIRM_WORD_CONFIRM:
      g_message ("window: \"send\" said to the card");
      card_running (self, widget);
      dictate_talk_confirm_async (self->talk, card->action, audio, DICTATE_ASK_NONE, NULL, on_card_confirmed,
                                  g_object_ref (widget));
      return TRUE;
    case DICTATE_CONFIRM_WORD_CANCEL:
      g_message ("window: \"cancel\" said to the card");
      countdown_stop (self);
      dictate_action_cancel (card->action);
      dictate_stage_set_who (DICTATE_STAGE (self->stage), _("Vibe says"), FALSE);
      card_settle (self, widget, _("Cancelled"), FALSE);
      return TRUE;
    default:
      return FALSE;
    }
}

static void
on_card_cancel (VibeosPressable *button, GtkWidget *widget)
{
  Card *card = g_object_get_data (G_OBJECT (widget), "dictate-card");
  GtkRoot *root = gtk_widget_get_root (widget);
  Window *self = root ? g_object_get_data (G_OBJECT (root), "dictate-window") : NULL;

  dictate_action_cancel (card->action);
  if (self)
    {
      /* the countdown was stopped in time: what was not done is said */
      if (self->card == widget && self->countdown_s)
        dictate_stage_set_who (DICTATE_STAGE (self->stage), _("Vibe says"), FALSE);
      card_settle (self, widget, _("Cancelled"), FALSE);
    }
}

static GtkWidget *
card_line (const char *text, const char *css)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, css);
  return label;
}

static void
on_talk_propose (DictateTalk *talk, DictateAction *action, Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  GtkWidget *widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  const DictateActionField *fields = dictate_action_get_fields (action);
  gboolean destructive = dictate_action_get_destructive (action);
  const char *go = dictate_action_get_button (action);
  GtkWidget *cancel, *button;
  Card *card = g_new0 (Card, 1);

  /* one proposal on the screen at a time: one still there is dropped */
  if (self->card)
    {
      Card *old = g_object_get_data (G_OBJECT (self->card), "dictate-card");

      countdown_stop (self);
      dictate_action_cancel (old->action);
      self->card = NULL;
    }
  card->action = dictate_action_ref (action);
  gtk_widget_add_css_class (widget, "dictate-stage-card");
  if (fields && fields[0].label)
    /* field by field: "Message to" / "Kim Novak", "Text" / "“I'm running late.”" */
    for (guint i = 0; fields[i].label; i++)
      {
        GtkWidget *label = card_line (fields[i].label, "dictate-stage-card-label");

        if (i)
          gtk_widget_add_css_class (label, "dictate-stage-card-next");
        gtk_box_append (GTK_BOX (widget), label);
        gtk_box_append (GTK_BOX (widget), card_line (fields[i].value, "dictate-stage-card-value"));
      }
  else
    {
      /* what the action is, then each thing it will do */
      const char *const *lines = dictate_action_get_lines (action);

      gtk_box_append (GTK_BOX (widget), card_line (dictate_action_get_title (action), "dictate-stage-card-label"));
      for (guint i = 0; lines && lines[i]; i++)
        gtk_box_append (GTK_BOX (widget), card_line (lines[i], "dictate-stage-card-value"));
    }
  card->state = card_line (NULL, "dictate-stage-card-state");
  gtk_widget_set_visible (card->state, FALSE);
  gtk_box_append (GTK_BOX (widget), card->state);
  g_object_set_data_full (G_OBJECT (widget), "dictate-card", card, card_free);

  self->card = widget;
  dictate_stage_set_middle (stage, widget);
  dictate_stage_clear_buttons (stage);
  self->countdown_s = dictate_action_get_countdown (action);
  cancel = dictate_stage_add_button (stage, _("Cancel"), DICTATE_STAGE_BUTTON);
  if (self->countdown_s)
    {
      /* K5: it goes by itself; the button does it now, with the action's own verb where it has one */
      button = dictate_stage_add_button (stage, !g_strcmp0 (go, _("Send")) ? _("Send now")
                                                : !g_strcmp0 (go, _("Call")) ? _("Call now") : _("Do it now"),
                                         DICTATE_STAGE_BUTTON_PRIMARY);
    }
  else
    button = dictate_stage_add_button (stage, go, destructive ? DICTATE_STAGE_BUTTON_DANGER
                                                              : DICTATE_STAGE_BUTTON_PRIMARY);
  g_set_weak_pointer (&card->go, button);
  card_hint (self);
  g_signal_connect_object (button, "clicked", G_CALLBACK (on_card_do), widget, 0);
  g_signal_connect_object (cancel, "clicked", G_CALLBACK (on_card_cancel), widget, 0);
  /* a call announced with the window not on the screen ("decline and text…"): the card needs it */
  if (!gtk_widget_get_visible (self->window))
    gtk_window_present (GTK_WINDOW (self->window));
  /* nothing to hear first (the reply is only written, or already said): the seconds start now */
  if (dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE)
    countdown_start (self);
}

/* ---- what the model side tells besides the reply (dictate-talk.h) ---- */

/* the window left the foreground (another app, the screen off): a countdown stops, and the card waits */
static void
on_window_active (GtkWindow *window, GParamSpec *pspec, Window *self)
{
  if (!gtk_window_is_active (window))
    countdown_wait (self);
}

/* the voice did not match for what was asked: nothing ran, and what Vibe said of it is on the screen */
static void
on_talk_refused (DictateTalk *talk, guint reason, double score, double needed, const char *line, Window *self)
{
  /* "send" said to a card in a voice not the owner's: the card stays, and waits for the tap */
  if (self->card)
    {
      if (reason != DICTATE_REFUSED_UNCHECKED)
        self->refusals++;
      dictate_stage_set_who (DICTATE_STAGE (self->stage), _("Not carried out"), TRUE);
      dictate_stage_set_reply (DICTATE_STAGE (self->stage), line ? line : "");
      card_hint (self);
      return;
    }
  /* nothing more comes for this turn: what it showed so far goes */
  self->ex_refused = TRUE;
  self->ex_refused_reason = reason;
  /* a voice that could not be checked is no sign of another voice: it does not count toward G3's three. Where a model
   * runs and the sample was made for another (an update), a new sample is the way through. stale () needs the model's
   * id, which the turn's check has as a rule already worked out and dictate-voicematch.c keeps */
  self->ex_model_gone = reason == DICTATE_REFUSED_UNCHECKED && !dictate_voice_match_ready ();
  self->ex_record_again = reason == DICTATE_REFUSED_UNCHECKED && !self->ex_model_gone && dictate_voice_match_stale ();
  if (reason != DICTATE_REFUSED_UNCHECKED)
    self->refusals++;
  self->ex_choices = self->ex_opened = FALSE;
  g_clear_pointer (&self->ex_suggestions, g_strfreev);
  g_clear_pointer (&self->ex_person, g_free);
  dictate_stage_set_middle (DICTATE_STAGE (self->stage), NULL);
  dictate_stage_set_written (DICTATE_STAGE (self->stage), NULL);
  g_free (self->ex_note);
  self->ex_note = g_strdup (_("Not carried out"));
  g_string_assign (self->ex_reply, line ? line : "");
  dictate_stage_set_said (DICTATE_STAGE (self->stage), NULL);
  vibe_show (self);
}

/* something was done in this reply: the ✓, and Undo when it can be undone */
static void
on_talk_acted (DictateTalk *talk, gboolean can_undo, Window *self)
{
  self->ex_done = TRUE;
  self->ex_undo = can_undo;
}

static void
on_talk_reply_info (DictateTalk *talk, guint flags, Window *self)
{
  self->reply_flags = flags;
}

/* I5: an app opened for this reply (the tool's result names it): the way back to Vibe is said under the reply */
static void
on_talk_tool (DictateTalk *talk, const char *name, const char *arguments, const char *result, double ms, Window *self)
{
  if (self->answering && !g_strcmp0 (name, "open_app") && result && strstr (result, "\"opened\""))
    self->ex_opened = TRUE;
}

/* a tap on a suggestion or a choice: its label asked, as if said */
static void
on_tapped_reply (VibeosPressable *button, Window *self)
{
  g_autofree char *label = g_strdup (g_object_get_data (G_OBJECT (button), "dictate-label"));

  if (dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE || self->card)
    return;
  vibe_send (self, label, NULL, DICTATE_ASK_TAPPED);
  show_state (self);
}

/* K2: replies the user may tap, under the answer once it is over (vibe_show) */
static void
on_talk_suggestions (DictateTalk *talk, const char *const *labels, Window *self)
{
  if (!self->answering)
    return;
  g_strfreev (self->ex_suggestions);
  self->ex_suggestions = g_strdupv ((char **) labels);
}

/* F1: Vibe asks which one: the rows to pick from, in the middle; a tap asks the row's label */
static void
on_talk_choices (DictateTalk *talk, const char *const *rows, Window *self)
{
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  if (!self->answering || self->card)
    {
      g_object_ref_sink (list);
      g_object_unref (list);
      return;
    }
  gtk_widget_add_css_class (list, "dictate-help");
  gtk_widget_set_valign (list, GTK_ALIGN_CENTER);
  for (guint i = 0; rows && rows[i]; i += 2)
    {
      const char *detail = rows[i + 1];
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *title = gtk_label_new (rows[i]);
      GtkWidget *row = vibeos_pressable_new (box);

      gtk_widget_add_css_class (title, "dictate-help-title");
      gtk_label_set_xalign (GTK_LABEL (title), 0);
      gtk_label_set_wrap (GTK_LABEL (title), TRUE);
      gtk_box_append (GTK_BOX (box), title);
      if (detail && *detail)
        {
          GtkWidget *under = gtk_label_new (detail);

          gtk_widget_add_css_class (under, "dictate-help-examples");
          gtk_label_set_xalign (GTK_LABEL (under), 0);
          gtk_box_append (GTK_BOX (box), under);
        }
      gtk_widget_add_css_class (row, "dictate-help-group");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 20);
      g_object_set_data_full (G_OBJECT (row), "dictate-label", g_strdup (rows[i]), g_free);
      g_signal_connect (row, "clicked", G_CALLBACK (on_tapped_reply), self);
      gtk_box_append (GTK_BOX (list), row);
      if (!detail)
        break;
    }
  dictate_stage_set_middle (DICTATE_STAGE (self->stage), list);
  self->ex_choices = TRUE;
}

/* what a reading tool found, as a card over the answer; each time the whole of it, in place of the last */
static void
on_talk_facts (DictateTalk *talk, const char *const *rows, Window *self)
{
  GtkWidget *facts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  if (!self->answering || self->card)
    {
      g_object_ref_sink (facts);
      g_object_unref (facts);
      return;
    }
  gtk_widget_add_css_class (facts, "dictate-stage-card");
  for (guint i = 0; rows && rows[i] && rows[i + 1]; i += 2)
    {
      GtkWidget *label = card_line (rows[i], "dictate-stage-card-label");

      if (i)
        gtk_widget_add_css_class (label, "dictate-stage-card-next");
      gtk_box_append (GTK_BOX (facts), label);
      gtk_box_append (GTK_BOX (facts), card_line (rows[i + 1], "dictate-stage-card-value"));
    }
  dictate_stage_set_middle (DICTATE_STAGE (self->stage), facts);
}

/* the contact the answer is about: their initials in the orb */
static void
on_talk_person (DictateTalk *talk, const char *name, Window *self)
{
  g_auto (GStrv) words = name ? g_strsplit_set (name, " -", -1) : NULL;
  GString *initials = g_string_new (NULL);

  if (!self->answering)
    {
      g_string_free (initials, TRUE);
      return;
    }
  for (guint w = 0; words && words[w] && g_utf8_strlen (initials->str, -1) < 2; w++)
    if (*words[w] && g_unichar_isalpha (g_utf8_get_char (words[w])))
      g_string_append_unichar (initials, g_unichar_toupper (g_utf8_get_char (words[w])));
  g_free (self->ex_person);
  self->ex_person = g_string_free (initials, initials->len == 0);
  if (self->ex_person && !self->ex_done)
    dictate_orb_set_look (dictate_stage_get_orb (DICTATE_STAGE (self->stage)), DICTATE_ORB_CALLER, self->ex_person);
}

/* holding the power key talks to Vibe (the shell's power-hold-agent), so it is the way back from an app */
static gboolean
hold_comes_back (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "vibeos.Shell", TRUE) : NULL;
  g_autoptr (GSettings) shell = NULL;

  if (!schema || !g_settings_schema_has_key (schema, "power-hold-agent"))
    return FALSE;
  shell = g_settings_new ("vibeos.Shell");
  return g_settings_get_boolean (shell, "power-hold-agent");
}

/* names and texts written, not spoken (private mode on the speaker) */
static void
on_talk_written (DictateTalk *talk, const char *const *lines, Window *self)
{
  dictate_stage_set_written (DICTATE_STAGE (self->stage), lines);
}

/* C2: what was asked needs something whose switch is off: it can be turned on from here */
static void
on_access_on (VibeosPressable *button, Window *self)
{
  const char *key = g_object_get_data (G_OBJECT (button), "dictate-access");

  g_settings_set_boolean (self->settings, key, TRUE);
  dictate_stage_clear_buttons (DICTATE_STAGE (self->stage));
  /* and what was asked, asked again: the tap is the user's (the phone is unlocked in their hand) */
  vibe_ask_again (self, DICTATE_ASK_TAPPED);
}

static void
on_access_off (VibeosPressable *button, Window *self)
{
  g_clear_pointer (&self->ex_access, g_free);
  show_state (self);
}

static void
on_talk_no_access (DictateTalk *talk, const char *key, Window *self)
{
  g_free (self->ex_access);
  self->ex_access = g_strdup (key);
}

/* ---- the screen's own buttons ---- */

static void
on_undo (VibeosPressable *button, Window *self)
{
  if (dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE)
    return;
  vibe_archive (self);
  /* TRANSLATORS: what the Undo button asks Vibe, as if said; Vibe's undo tool answers it */
  self->ex_said = g_strdup (C_("vibe asked", "Undo that."));
  dictate_stage_set_said (DICTATE_STAGE (self->stage), self->ex_said);
  vibe_ask (self, self->ex_said, NULL, DICTATE_ASK_TAPPED);
  show_state (self);
}

static void
on_try_again (VibeosPressable *button, Window *self)
{
  if (dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE)
    vibe_ask_again (self, DICTATE_ASK_NONE);
}

static void
on_owner_confirmed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  g_autoptr (GError) error = NULL;
  gboolean confirmed = dictate_owner_confirm_finish (result, &error);

  if (!self)
    return;
  g_clear_object (&self->confirming);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("window: the lock screen could not be asked: %s", error->message);
      return;
    }
  /* the PIN given for this one command: it is asked again, without the voice check */
  if (confirmed && self->ex_refused)
    vibe_ask_again (self, DICTATE_ASK_OWNER_CONFIRMED);
}

static void
on_unlock_pin (VibeosPressable *button, Window *self)
{
  if (self->confirming)
    return;
  self->confirming = g_cancellable_new ();
  dictate_owner_confirm_async ("vibeos.Agent", self->confirming, on_owner_confirmed, g_object_ref (self->window));
}

/* --- G3: the voice not recognised three times running: a new sample offered, behind the owner's PIN */

static void
on_reenrolled (gboolean saved, gpointer data)
{
  Window *self = data;

  vibeos_page_stack_pop (VIBEOS_PAGE_STACK (self->pages));
  if (saved)
    {
      self->refusals = 0;
      vibe_archive (self);
      show_state (self);
    }
}

static void
reenroll (Window *self)
{
  GtkWidget *enroll = dictate_enroll_new (self->settings, self->talk, NULL, dictate_engine_get_language (self->engine),
                                          _("Cancel"), on_reenrolled, self);

  vibeos_page_stack_push (VIBEOS_PAGE_STACK (self->pages), enroll);
  dictate_enroll_start (enroll);
}

static void
on_reenroll_confirmed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  g_autoptr (GError) error = NULL;
  gboolean confirmed = dictate_owner_confirm_finish (result, &error);

  if (!self)
    return;
  g_clear_object (&self->confirming);
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("window: the lock screen could not be asked: %s", error->message);
  if (confirmed)
    reenroll (self);
}

static void
on_reenroll (VibeosPressable *button, Window *self)
{
  if (self->confirming)
    return;
  /* someone else's voice must not replace the owner's: the PIN first, where the phone has one */
  if (!dictate_owner_has_secret ())
    {
      reenroll (self);
      return;
    }
  self->confirming = g_cancellable_new ();
  dictate_owner_confirm_async ("vibeos.Agent", self->confirming, on_reenroll_confirmed, g_object_ref (self->window));
}

static void
on_not_now (VibeosPressable *button, Window *self)
{
  self->refusals = 0;
  vibe_show (self);
}

/* --- G4: what can be said, by kind: only kinds whose tools are there and switched on; offline, the fixed commands
 * (dictate-offline.h) */

static const struct { const char *area, *title, *examples; } HELP[] = {
  { "access-calls", N_("Calls and messages"), N_("“Call Kim” · “Read my messages”") },
  { "access-alarms", N_("Time and alarms"), N_("“Wake me at 6:30” · “Timer, ten minutes”") },
  { "access-settings", N_("This phone"), N_("“Turn on Wi-Fi” · “How's the battery?”") },
  { "access-location", N_("Around me"), N_("“Take me home” · “What's the weather?”") },
};

static const struct { const char *title, *examples; } HELP_OFFLINE[] = {
  { N_("Calls and messages"), N_("“Call Kim” · “Read my messages”") },
  { N_("Time and alarms"), N_("“What time is it?” · “Timer, ten minutes”") },
  { N_("This phone"), N_("“Turn on Wi-Fi” · “How's the battery?”") },
  { N_("What I just did"), N_("“Undo” · “What did you just do?”") },
};

static GtkWidget *
help_group (const char *title, const char *examples)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *t = gtk_label_new (title), *e = gtk_label_new (examples);

  gtk_widget_add_css_class (box, "dictate-help-group");
  gtk_widget_add_css_class (t, "dictate-help-title");
  gtk_widget_add_css_class (e, "dictate-help-examples");
  gtk_label_set_xalign (GTK_LABEL (t), 0);
  gtk_label_set_xalign (GTK_LABEL (e), 0);
  gtk_label_set_wrap (GTK_LABEL (e), TRUE);
  gtk_box_append (GTK_BOX (box), t);
  gtk_box_append (GTK_BOX (box), e);
  return box;
}

static void
vibe_show_help (Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  GtkWidget *groups = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

  gtk_widget_add_css_class (groups, "dictate-help");
  gtk_widget_set_valign (groups, GTK_ALIGN_CENTER);
  if (self->help_offline)
    for (guint i = 0; i < G_N_ELEMENTS (HELP_OFFLINE); i++)
      gtk_box_append (GTK_BOX (groups), help_group (_(HELP_OFFLINE[i].title), _(HELP_OFFLINE[i].examples)));
  else
    for (guint i = 0; i < G_N_ELEMENTS (HELP); i++)
      if (dictate_tools_area_has_tools (HELP[i].area) && g_settings_get_boolean (self->settings, HELP[i].area))
        gtk_box_append (GTK_BOX (groups), help_group (_(HELP[i].title), _(HELP[i].examples)));
  dictate_stage_clear_buttons (stage);
  dictate_stage_set_middle (stage, groups);
  dictate_orb_set_look (dictate_stage_get_orb (stage), DICTATE_ORB_REST, NULL);
  dictate_stage_set_who (stage, _("Vibe says"), FALSE);
  dictate_stage_set_reply (stage, self->ex_reply->str);
  dictate_stage_set_hint (stage, self->help_offline ? _("No connection · basic commands only")
                                                    : _("Tap anywhere and speak, or just try a command"));
}

/* the help asked for: on the screen, not asked of the model; its line spoken */
static void
vibe_help (Window *self, gboolean offline)
{
  self->help = TRUE;
  self->help_offline = offline;
  g_string_assign (self->ex_reply, offline ? _("Without my model, these still work.")
                                           : _("Here are some things you can say."));
  vibe_show_help (self);
  if (dictate_talk_get_speak (self->talk) && dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE)
    {
      self->answering = TRUE;
      dictate_talk_announce (self->talk, dictate_engine_get_language (self->engine), self->ex_reply->str);
    }
}

/* C4's "What can I do?" */
static void
on_what_can_i_do (VibeosPressable *button, Window *self)
{
  vibe_archive (self);
  /* TRANSLATORS: what the button asks Vibe, as if said */
  self->ex_said = g_strdup (C_("vibe asked", "What can I do?"));
  dictate_stage_set_said (DICTATE_STAGE (self->stage), self->ex_said);
  vibe_help (self, TRUE);
}

/* "What can I say?" said, and nothing else: answered on the phone */
static gboolean
asks_for_help (const char *said)
{
  /* TRANSLATORS: what a user says to hear what Vibe can do, alternatives separated by "|", lower case, without
   * punctuation; only the whole sentence counts */
  g_auto (GStrv) each = g_strsplit (_("what can i say|what can you do|what can i ask|help"), "|", -1);
  g_autofree char *down = g_utf8_strdown (said, -1);
  GString *bare = g_string_new (NULL);

  for (const char *c = down; *c; c = g_utf8_next_char (c))
    {
      gunichar u = g_utf8_get_char (c);

      if (g_unichar_isalnum (u) || u == ' ' || u == '\'')
        g_string_append_unichar (bare, u);
    }
  g_strstrip (bare->str);
  for (guint i = 0; each[i]; i++)
    if (!g_strcmp0 (bare->str, each[i]))
      {
        g_string_free (bare, TRUE);
        return TRUE;
      }
  g_string_free (bare, TRUE);
  return FALSE;
}

/* --- F3: in a call, Vibe's stage is the call's: who it is with, for how long, and Mute, Hold and Hang up, which
 * call_control does at once, as when said */

static void
call_info_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  /* the name is written, not said: asked without private mode's withholding */
  DictateToolContext ctx = { GPOINTER_TO_UINT (data), NULL, NULL, NULL, FALSE, FALSE };

  g_task_return_pointer (task, dictate_tools_run ("get_current_call", "{}", &ctx, 5000), g_free);
}

static void
on_call_info (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  GCancellable *cancellable = g_task_get_cancellable (G_TASK (result));
  g_autofree char *found = g_task_propagate_pointer (G_TASK (result), NULL);

  if (!self || g_cancellable_is_cancelled (cancellable) || !self->call_on)
    return;
  if (self->call_fetch == cancellable)
    g_clear_object (&self->call_fetch);
  g_free (self->call_info);
  self->call_info = g_steal_pointer (&found);
  vibe_show (self);
}

static void
call_fetch (Window *self)
{
  g_autoptr (GTask) task = NULL;

  if (self->call_fetch)
    g_cancellable_cancel (self->call_fetch);
  g_clear_object (&self->call_fetch);
  self->call_fetch = g_cancellable_new ();
  task = g_task_new (NULL, self->call_fetch, on_call_info, g_object_ref (self->window));
  g_task_set_task_data (task, GUINT_TO_POINTER (dictate_tools_areas (self->settings)), NULL);
  g_task_run_in_thread (task, call_info_thread);
}

/* what get_current_call says of the call going on: its name (a contact's; else what kind of number), its initials,
 * and how the phone is set for it */
typedef struct {
  char *name, *initials;
  gboolean muted, speaker, can_hold, read;
} CallInfo;

static void
call_info_parse (const char *found, CallInfo *info)
{
  g_autoptr (JsonNode) root = found ? json_from_string (found, NULL) : NULL;
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *calls = o && json_object_has_member (o, "calls") ? json_object_get_array_member (o, "calls") : NULL;
  GString *initials = g_string_new (NULL);

  memset (info, 0, sizeof *info);
  for (guint i = 0; calls && i < json_array_get_length (calls) && !info->name; i++)
    {
      JsonObject *c = json_array_get_object_element (calls, i);
      const char *state = c ? json_object_get_string_member_with_default (c, "state", "") : "";
      const char *with = c ? json_object_get_string_member_with_default (c, "with", NULL) : NULL;
      g_auto (GStrv) words = NULL;

      if (!c || g_str_has_prefix (state, "ringing") || !strcmp (state, "ending"))
        continue;
      if (!json_object_get_boolean_member_with_default (c, "in_contacts", FALSE) || !with)
        {
          info->name = g_strdup (json_object_get_boolean_member_with_default (c, "private_number", FALSE)
                                 ? _("Private number") : _("Unknown number"));
          continue;
        }
      info->name = g_strdup (with);
      words = g_strsplit_set (with, " -", -1);
      for (guint w = 0; words[w] && g_utf8_strlen (initials->str, -1) < 2; w++)
        if (*words[w] && g_unichar_isalpha (g_utf8_get_char (words[w])))
          g_string_append_unichar (initials, g_unichar_toupper (g_utf8_get_char (words[w])));
    }
  info->initials = g_string_free (initials, initials->len == 0);
  info->read = o != NULL;
  if (o)
    {
      info->muted = json_object_get_boolean_member_with_default (o, "microphone_muted", FALSE);
      info->speaker = json_object_get_boolean_member_with_default (o, "speaker_on", FALSE);
      info->can_hold = json_object_get_boolean_member_with_default (o, "can_hold", FALSE);
    }
}

/* the call's stage is the one on the screen: nothing else is going on */
static gboolean
call_showing (Window *self)
{
  return self->call_on && !self->ringing && !self->ex_said && !self->ex_reply->len && !self->card && !self->silence
         && !self->help && dictate_engine_get_state (self->engine) == DICTATE_IDLE
         && dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE;
}

/* "On a call · 02:41", "On hold", "Calling…" */
static char *
call_status (Window *self)
{
  if (self->call_state == DICTATE_CALL_DIALING || self->call_state == DICTATE_CALL_CONNECTING)
    return g_strdup (_("Calling…"));
  if (self->call_state == DICTATE_CALL_ONHOLD)
    return g_strdup (_("On hold"));
  if (self->call_connected > 0)
    {
      gint64 s = MAX (0, (g_get_real_time () / 1000 - self->call_connected) / 1000);
      g_autofree char *length = s >= 3600 ? g_strdup_printf ("%d:%02d:%02d", (int) (s / 3600), (int) (s / 60 % 60),
                                                             (int) (s % 60))
                                          : g_strdup_printf ("%02d:%02d", (int) (s / 60), (int) (s % 60));

      /* TRANSLATORS: over the name of whom the call is with; %s is how long it has gone on: 02:41 or 1:02:41 */
      return g_strdup_printf (_("On a call · %s"), length);
    }
  return g_strdup (_("On a call"));
}

/* the call's line, by the second: over the call's stage, or in the banner over an exchange */
static gboolean
call_tick (gpointer data)
{
  Window *self = data;
  g_autofree char *status = NULL;

  if (!self->call_on)
    {
      self->call_tick_id = 0;
      return G_SOURCE_REMOVE;
    }
  status = call_status (self);
  if (call_showing (self))
    dictate_stage_set_who (DICTATE_STAGE (self->stage), status, FALSE);
  else if (self->call_banner_text)
    gtk_label_set_text (GTK_LABEL (self->call_banner_text), status);
  return G_SOURCE_CONTINUE;
}

static void
on_call_control_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  g_autofree char *outcome = g_task_propagate_pointer (G_TASK (result), NULL);

  /* how the phone is set for the call now (muted, on hold); the banner is built again either way, its pressed button
   * pressable again */
  if (self && self->call_on)
    {
      g_clear_pointer (&self->call_banner_key, g_free);
      call_fetch (self);
    }
}

typedef struct {
  guint32 areas;
  char *arguments;
} CallControl;

static void
call_control_free (gpointer data)
{
  CallControl *c = data;

  g_free (c->arguments);
  g_free (c);
}

static void
call_control_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  CallControl *c = data;
  DictateToolContext ctx = { c->areas, NULL, NULL, NULL, FALSE, FALSE };

  g_task_return_pointer (task, dictate_tools_run ("call_control", c->arguments, &ctx, 5000), g_free);
}

static void
on_call_control (VibeosPressable *button, Window *self)
{
  CallControl *c = g_new0 (CallControl, 1);
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_call_control_done, g_object_ref (self->window));

  c->areas = dictate_tools_areas (self->settings);
  c->arguments = g_strdup_printf ("{\"action\":\"%s\"}", (const char *) g_object_get_data (G_OBJECT (button),
                                                                                         "dictate-call-action"));
  g_task_set_task_data (task, c, call_control_free);
  g_task_run_in_thread (task, call_control_thread);
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
}

static void
call_control_button (Window *self, const char *label, DictateStageButtonKind kind, const char *action)
{
  GtkWidget *button = dictate_stage_add_button (DICTATE_STAGE (self->stage), label, kind);

  g_object_set_data (G_OBJECT (button), "dictate-call-action", (gpointer) action);
  g_signal_connect (button, "clicked", G_CALLBACK (on_call_control), self);
}

static void
vibe_show_call (Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  CallInfo info;

  call_info_parse (self->call_info, &info);
  dictate_orb_set_look (dictate_stage_get_orb (stage), DICTATE_ORB_CALLER, info.initials);
  dictate_stage_set_text (stage, info.name ? info.name : "",
                          !info.read ? NULL : info.speaker ? _("Speaker is on.") : NULL);
  dictate_stage_clear_buttons (stage);
  if (info.read)
    {
      call_control_button (self, info.muted ? _("Unmute") : _("Mute"), DICTATE_STAGE_BUTTON,
                           info.muted ? "unmute" : "mute");
      if (info.can_hold && self->call_state != DICTATE_CALL_DIALING && self->call_state != DICTATE_CALL_CONNECTING)
        call_control_button (self, self->call_state == DICTATE_CALL_ONHOLD ? _("Resume") : _("Hold"),
                             DICTATE_STAGE_BUTTON, self->call_state == DICTATE_CALL_ONHOLD ? "resume" : "hold");
    }
  call_control_button (self, _("Hang up"), DICTATE_STAGE_BUTTON_DANGER, "hang_up");
  dictate_stage_set_hint (stage, info.can_hold ? _("Tap anywhere and say “mute”, “hold”, “speaker” or “hang up”")
                                               : _("Tap anywhere and say “mute”, “speaker” or “hang up”"));
  call_tick (self);
  g_free (info.name);
  g_free (info.initials);
}

/* a small button of the banner, doing what call_control does */
static GtkWidget *
call_banner_button (Window *self, const char *label, VibeosColor fill, const char *css, const char *action)
{
  GtkWidget *text = gtk_label_new (label), *button = vibeos_pressable_new (text);

  gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (button, "dictate-call-banner-button");
  if (css)
    gtk_widget_add_css_class (button, css);
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (button), fill, FALSE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 20);
  gtk_widget_set_hexpand (button, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_object_set_data (G_OBJECT (button), "dictate-call-action", (gpointer) action);
  g_signal_connect (button, "clicked", G_CALLBACK (on_call_control), self);
  return button;
}

/* F3 over an exchange: while something is asked or answered in a call, the call stays on top, under the bar: who it
 * is with, how long, and Mute, Hold and Hang up; the exchange is under it. Over the call's own stage, none */
static void
call_banner_update (Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  GtkWidget *banner, *row, *name;
  g_autofree char *status = NULL, *key = NULL;
  CallInfo info;

  if (!self->call_on || self->ringing || call_showing (self))
    {
      dictate_stage_set_banner (stage, NULL);
      g_clear_pointer (&self->call_banner_key, g_free);
      return;
    }
  call_info_parse (self->call_info, &info);
  key = g_strdup_printf ("%s|%d|%d|%d|%d|%d", info.name ? info.name : "", info.read, info.muted, info.can_hold,
                         self->call_state, self->call_banner != NULL);
  if (self->call_banner && !g_strcmp0 (key, self->call_banner_key))
    {
      g_free (info.name);
      g_free (info.initials);
      return;
    }
  g_free (self->call_banner_key);
  self->call_banner_key = g_steal_pointer (&key);
  banner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (banner, "dictate-call-banner");
  name = gtk_label_new (info.name ? info.name : _("Call"));
  gtk_widget_add_css_class (name, "dictate-call-banner-name");
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  status = call_status (self);
  row = gtk_label_new (status);
  gtk_widget_add_css_class (row, "dictate-call-banner-status");
  gtk_label_set_xalign (GTK_LABEL (row), 0);
  {
    GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

    gtk_widget_set_hexpand (name, TRUE);
    gtk_box_append (GTK_BOX (top), name);
    gtk_box_append (GTK_BOX (top), row);
    gtk_box_append (GTK_BOX (banner), top);
  }
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous (GTK_BOX (row), TRUE);
  if (info.read)
    {
      gtk_box_append (GTK_BOX (row), call_banner_button (self, info.muted ? _("Unmute") : _("Mute"),
                                                         VIBEOS_SURFACE_CONTAINER_HIGHEST, NULL,
                                                         info.muted ? "unmute" : "mute"));
      if (info.can_hold && self->call_state != DICTATE_CALL_DIALING && self->call_state != DICTATE_CALL_CONNECTING)
        gtk_box_append (GTK_BOX (row),
                        call_banner_button (self, self->call_state == DICTATE_CALL_ONHOLD ? _("Resume") : _("Hold"),
                                            VIBEOS_SURFACE_CONTAINER_HIGHEST, NULL,
                                            self->call_state == DICTATE_CALL_ONHOLD ? "resume" : "hold"));
    }
  gtk_box_append (GTK_BOX (row), call_banner_button (self, _("Hang up"), VIBEOS_ERROR_CONTAINER,
                                                     "dictate-call-banner-danger", "hang_up"));
  gtk_box_append (GTK_BOX (banner), row);
  dictate_stage_set_banner (stage, banner);
  g_set_weak_pointer (&self->call_banner, banner);
  g_set_weak_pointer (&self->call_banner_text, gtk_widget_get_last_child (gtk_widget_get_first_child (banner)));
  g_free (info.name);
  g_free (info.initials);
}

/* B1: the day's greeting and date, as the clock has them */
static void
vibe_show_idle (Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int hour = g_date_time_get_hour (now);
  /* TRANSLATORS: the date under Vibe's greeting, a g_date_time_format () string: "Tuesday 24 November" */
  g_autofree char *date = g_date_time_format (now, _("%A %e %B"));
  g_autofree char *quiet = g_settings_get_string (self->settings, "quiet-hours");

  if (call_showing (self))
    {
      vibe_show_call (self);
      return;
    }
  dictate_orb_set_look (dictate_stage_get_orb (stage), DICTATE_ORB_REST, NULL);
  dictate_stage_clear_buttons (stage);
  dictate_stage_set_hint (stage, _("Tap anywhere and speak"));
  if (dictate_calls_quiet_now (self->settings))
    {
      /* C6: quiet hours, until their end */
      const char *dash = strchr (quiet, '-');
      /* TRANSLATORS: over "I'm keeping quiet."; %s is when the quiet hours end: 07:00 */
      g_autofree char *who = g_strdup_printf (_("Quiet hours · until %s"), dash ? dash + 1 : "");
      gboolean calls = g_settings_get_boolean (self->settings, "announce-calls")
                       && g_settings_get_boolean (self->settings, "quiet-calls");

      dictate_orb_set_look (dictate_stage_get_orb (stage), DICTATE_ORB_DIM, NULL);
      dictate_stage_set_who (stage, who, FALSE);
      dictate_stage_set_text (stage, _("I'm keeping quiet."),
                              calls ? _("I won't speak or play sounds on my own. Calls are still announced. Ask me "
                                        "anything and I'll answer.")
                                    : _("I won't speak or play sounds on my own. Ask me anything and I'll answer."));
      return;
    }
  dictate_stage_set_who (stage, NULL, FALSE);
  dictate_stage_set_text (stage, hour < 5 || hour >= 18 ? _("Good evening.")
                                 : hour < 12 ? _("Good morning.") : _("Good afternoon."),
                          g_strstrip (date));
}

/* the stage as things are now: the orb, the buttons, and the line at the bottom */
static void
vibe_show (Window *self)
{
  DictateStage *stage = DICTATE_STAGE (self->stage);
  DictateOrb *orb = dictate_stage_get_orb (stage);
  DictateState state = dictate_engine_get_state (self->engine);
  DictateTalkState talk = dictate_talk_get_state (self->talk);
  gboolean shown = self->ex_said || self->ex_reply->len;

  call_banner_update (self);
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING || state == DICTATE_STOPPING)
    {
      dictate_orb_set_look (orb, DICTATE_ORB_LISTENING, NULL);
      if (self->ringing && self->answer_listening)
        dictate_stage_set_hint (stage, _("Say “answer” or “decline”"));
      else if (self->card)
        card_hint (self);
      else if (self->held)
        dictate_stage_set_hint (stage, _("Release the side button when you have finished"));
      else if (shown)
        dictate_stage_set_hint (stage, _("Listening for your answer…"));
      else
        dictate_stage_set_hint (stage, _("Tap to stop"));
      return;
    }
  if (self->ringing && self->announcing)
    return;
  if (talk == DICTATE_TALK_THINKING)
    {
      dictate_orb_set_look (orb, DICTATE_ORB_WORKING, NULL);
      return;
    }
  if (talk == DICTATE_TALK_SPEAKING)
    {
      dictate_orb_set_look (orb, self->ex_done ? DICTATE_ORB_DONE : self->ex_person ? DICTATE_ORB_CALLER
                                                                                    : DICTATE_ORB_REST,
                            self->ex_person);
      dictate_stage_set_hint (stage, _("Tap to stop"));
      return;
    }
  if (self->silence)
    {
      /* G1: a turn asked for, and nothing said in it */
      dictate_orb_set_look (orb, DICTATE_ORB_DIM, NULL);
      dictate_stage_set_who (stage, _("Vibe says"), FALSE);
      dictate_stage_set_text (stage, _("I didn't hear anything."), _("Hold the phone a little closer, or speak up."));
      dictate_stage_set_hint (stage, _("Tap anywhere to try again"));
      return;
    }
  if (!shown)
    {
      vibe_show_idle (self);
      return;
    }
  if (self->card)
    {
      card_hint (self);
      return;
    }
  if (self->help)
    {
      vibe_show_help (self);
      return;
    }
  dictate_stage_clear_buttons (stage);
  if (self->ex_refused && self->ex_refused_reason == DICTATE_REFUSED_UNCHECKED)
    {
      /* reason 2: a sample, and the voice could not be checked against it. Saying it again is refused the same way;
       * the PIN lets this one command through, and a new sample, where one can be made, the ones after it */
      g_autofree char *headline = NULL, *rest = NULL;

      dictate_stage_split_reply (self->ex_reply->str, &headline, &rest);
      dictate_orb_set_look (orb, DICTATE_ORB_REFUSED, NULL);
      dictate_stage_set_who (stage, _("Not carried out"), TRUE);
      dictate_stage_set_text (stage, *headline ? headline : _("I can't check your voice right now."),
                              self->ex_record_again
                              ? _("Voice match was updated since your voice was recorded. Record it again and I "
                                  "can check it.")
                              : self->ex_model_gone
                              ? _("The voice model is not installed any more, so I can't tell your voice from "
                                  "anyone else's.")
                              /* the model is there and the sample made for it, and the check failed to run */
                              : _("Voice match couldn't run just now, so I can't tell your voice from anyone "
                                  "else's."));
      /* beside "Record again", half the width: the shorter label (the longer one is cut there) */
      if (dictate_owner_has_secret ())
        g_signal_connect (dictate_stage_add_button (stage, self->ex_record_again ? _("Use PIN instead")
                                                                                 : _("Unlock with PIN instead"),
                                                    DICTATE_STAGE_BUTTON),
                          "clicked", G_CALLBACK (on_unlock_pin), self);
      if (self->ex_record_again)
        g_signal_connect (dictate_stage_add_button (stage, _("Record again"), DICTATE_STAGE_BUTTON_PRIMARY),
                          "clicked", G_CALLBACK (on_reenroll), self);
      dictate_stage_set_hint (stage, dictate_owner_has_secret ()
                              ? _("Until voices can be checked, commands like this one need your PIN")
                              : _("Until voices can be checked, commands like this one are not carried out"));
      return;
    }
  if (self->ex_refused && self->refusals >= 3 && dictate_voice_match_ready ())
    {
      /* G3: three in a row; the voice may have changed (a cold, another microphone) */
      dictate_orb_set_look (orb, DICTATE_ORB_REFUSED, NULL);
      dictate_stage_set_who (stage, _("Third try not recognised"), TRUE);
      dictate_stage_set_text (stage, _("Has your voice changed?"),
                              dictate_owner_has_secret ()
                              ? _("A cold or a new microphone can do that. Enter your PIN and I'll learn it again.")
                              : _("A cold or a new microphone can do that. Record your voice again and I'll learn "
                                  "it."));
      g_signal_connect (dictate_stage_add_button (stage, _("Not now"), DICTATE_STAGE_BUTTON), "clicked",
                        G_CALLBACK (on_not_now), self);
      g_signal_connect (dictate_stage_add_button (stage, dictate_owner_has_secret () ? _("Enter PIN")
                                                                                   : _("Record again"),
                                                  DICTATE_STAGE_BUTTON_PRIMARY), "clicked",
                        G_CALLBACK (on_reenroll), self);
      dictate_stage_set_hint (stage, _("Until then, commands in this voice are not carried out"));
      return;
    }
  if (self->ex_refused)
    {
      /* C1: not carried out for the voice */
      g_autofree char *headline = NULL, *rest = NULL;

      dictate_stage_split_reply (self->ex_reply->str, &headline, &rest);
      dictate_orb_set_look (orb, DICTATE_ORB_REFUSED, NULL);
      dictate_stage_set_who (stage, _("Not carried out"), TRUE);
      dictate_stage_set_text (stage, *headline ? headline : _("That didn't sound like my owner."),
                              *rest ? rest : _("I only take commands from the voice I was taught. Try again, "
                                               "closer to the phone."));
      /* the lock screen's own PIN, never the voice, lets it through: only where the phone has one */
      if (dictate_owner_has_secret ())
        g_signal_connect (dictate_stage_add_button (stage, _("Unlock with PIN instead"), DICTATE_STAGE_BUTTON),
                          "clicked", G_CALLBACK (on_unlock_pin), self);
      dictate_stage_set_hint (stage, _("Tap anywhere to try again"));
      return;
    }
  if (self->ex_access)
    {
      GtkWidget *on;

      dictate_orb_set_look (orb, DICTATE_ORB_DIM, NULL);
      g_signal_connect (dictate_stage_add_button (stage, _("Leave it off"), DICTATE_STAGE_BUTTON), "clicked",
                        G_CALLBACK (on_access_off), self);
      on = dictate_stage_add_button (stage, _("Turn on"), DICTATE_STAGE_BUTTON_TONAL);
      g_object_set_data_full (G_OBJECT (on), "dictate-access", g_strdup (self->ex_access), g_free);
      g_signal_connect (on, "clicked", G_CALLBACK (on_access_on), self);
      dictate_stage_set_hint (stage, NULL);
      return;
    }
  if (self->reply_flags & DICTATE_REPLY_OFFLINE)
    {
      /* C4: the model could not be reached; the fixed commands answered */
      dictate_orb_set_look (orb, DICTATE_ORB_DIM, NULL);
      g_signal_connect (dictate_stage_add_button (stage, _("Try again"), DICTATE_STAGE_BUTTON), "clicked",
                        G_CALLBACK (on_try_again), self);
      g_signal_connect (dictate_stage_add_button (stage, _("What can I do?"), DICTATE_STAGE_BUTTON_TONAL), "clicked",
                        G_CALLBACK (on_what_can_i_do), self);
      dictate_stage_set_hint (stage, _("No connection · basic commands only"));
      return;
    }
  dictate_orb_set_look (orb, self->ex_done ? DICTATE_ORB_DONE : self->ex_person ? DICTATE_ORB_CALLER
                                                                                : DICTATE_ORB_REST,
                        self->ex_person);
  /* K2: replies to tap, three at most, the last the likeliest ("Text her", "Call back") */
  for (guint i = 0; self->ex_suggestions && self->ex_suggestions[i] && i < 3; i++)
    {
      gboolean last = !self->ex_suggestions[i + 1] || i == 2;
      GtkWidget *button = dictate_stage_add_button (stage, self->ex_suggestions[i],
                                                    last ? DICTATE_STAGE_BUTTON_PRIMARY : DICTATE_STAGE_BUTTON);

      g_object_set_data_full (G_OBJECT (button), "dictate-label", g_strdup (self->ex_suggestions[i]), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_tapped_reply), self);
    }
  if (self->ex_undo)
    {
      g_signal_connect (dictate_stage_add_button (stage, _("Undo"), DICTATE_STAGE_BUTTON), "clicked",
                        G_CALLBACK (on_undo), self);
      dictate_stage_set_hint (stage, _("Say “undo” to take it back"));
      return;
    }
  if (self->ex_choices)
    dictate_stage_set_hint (stage, _("Say which one, or tap it"));
  else if (self->ex_opened && hold_comes_back ())
    dictate_stage_set_hint (stage, _("Hold the side button to come back"));
  else if (self->reply_flags & DICTATE_REPLY_PRIVATE_SPEAKER)
    dictate_stage_set_hint (stage, _("Private mode is on"));
  else if (!dictate_talk_get_speak (self->talk))
    /* J5: Speak the replies is off */
    dictate_stage_set_hint (stage, _("Speech is off · replies are only written"));
  else
    dictate_stage_set_hint (stage, _("Tap anywhere and speak"));
}

static void
on_talk_state (DictateTalk *talk, Window *self)
{
  DictateTalkState state = dictate_talk_get_state (talk), was = self->talk_state;

  self->talk_state = state;
  set_inhibit (self, state != DICTATE_TALK_IDLE || dictate_engine_get_state (self->engine) != DICTATE_IDLE);
  /* K5: the card read back; its countdown starts once Vibe has finished saying it */
  if (state == DICTATE_TALK_IDLE)
    countdown_start (self);
  if (state == DICTATE_TALK_IDLE && (self->answering || self->announcing))
    {
      gboolean announced = self->announcing;

      self->answering = FALSE;
      self->announcing = FALSE;
      /* a call announced, still ringing: Vibe listens for what to do with it, on the screen or not */
      if (announced && was == DICTATE_TALK_SPEAKING && !self->stopping_reply && self->ringing
          && dictate_engine_get_state (self->engine) == DICTATE_IDLE)
        {
          talk_listen (self);
          self->answer_listening = TRUE;
        }
      /* spoken to its end: the user's turn (whose cue says it is over) */
      else if (was == DICTATE_TALK_SPEAKING && !self->stopping_reply && self->talking && !self->hold_turn &&
               !self->ex_refused &&
               g_settings_get_boolean (self->settings, "talk-listen-again") &&
               dictate_engine_get_state (self->engine) == DICTATE_IDLE && gtk_widget_get_mapped (self->window))
        talk_listen (self);
      else if (was != DICTATE_TALK_IDLE && !self->stopping_reply && !self->reply_failed && self->talking)
        dictate_cue_play (self->settings, self->ex_refused ? DICTATE_CUE_FAILED : DICTATE_CUE_DONE);
    }
  show_state (self);
  maybe_close (self);
}

static void
on_talk_error (DictateTalk *talk, const char *message, Window *self)
{
  /* told before the reply ends (dictate-talk.c): its end then plays no "done" */
  self->reply_failed = TRUE;
  dictate_cue_play (self->settings, DICTATE_CUE_FAILED);
  vibeos_snackbar_show (self->snackbar, message, NULL, NULL, NULL, VIBEOS_SNACKBAR_LONG);
}

/* the server had too many questions: its reply says so, and the cue is the failed one */
static void
on_talk_busy (DictateTalk *talk, Window *self)
{
  self->reply_failed = TRUE;
  dictate_cue_play (self->settings, DICTATE_CUE_FAILED);
}

/* A tap anywhere on the stage, as the FAB was: idle, it listens; listening, it stops, and the words so far are
 * dropped (not sent); while Vibe speaks it stops speaking at once (the reply goes on being written); while the model
 * is still at it, the reply stops where it is. */
static void
talk_mic (Window *self)
{
  DictateState state = dictate_engine_get_state (self->engine);
  DictateTalkState talk = dictate_talk_get_state (self->talk);

  if (talk == DICTATE_TALK_IDLE && state == DICTATE_IDLE)
    {
      talk_listen (self);
      show_state (self);
      return;
    }
  self->stopping_reply = TRUE;
  if (talk == DICTATE_TALK_SPEAKING)
    dictate_talk_hush (self->talk);
  else if (talk == DICTATE_TALK_THINKING)
    dictate_talk_stop (self->talk);
  self->stopping_reply = FALSE;
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  show_state (self);
}

/* a new conversation: all of it goes from the screen and the phone, and the model forgets it */
static void
talk_clear (Window *self)
{
  self->stopping_reply = TRUE;
  dictate_talk_reset (self->talk);
  self->stopping_reply = FALSE;
  if (dictate_engine_get_state (self->engine) != DICTATE_IDLE)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  if (self->card)
    {
      Card *card = g_object_get_data (G_OBJECT (self->card), "dictate-card");

      countdown_stop (self);
      dictate_action_cancel (card->action);
      self->card = NULL;
    }
  g_clear_pointer (&self->ex_said, g_free);
  g_string_truncate (self->ex_reply, 0);
  g_clear_pointer (&self->ex_note, g_free);
  g_clear_pointer (&self->ex_audio, g_bytes_unref);
  g_clear_pointer (&self->ex_access, g_free);
  self->ex_refused = self->ex_done = self->ex_undo = self->silence = self->answering = self->help = self->ex_opened = FALSE;
  g_clear_pointer (&self->ex_suggestions, g_strfreev);
  g_clear_pointer (&self->ex_person, g_free);
  self->ex_choices = FALSE;
  g_ptr_array_set_size (self->history, 0);
  dictate_history_clear ();
  dictate_stage_clear_past (DICTATE_STAGE (self->stage));
  dictate_stage_reset (DICTATE_STAGE (self->stage));
  show_state (self);
}

static void
on_clear_conversation (VibeosDialog *dialog, int response, Window *self)
{
  if (response == VIBEOS_RESPONSE_OK)
    talk_clear (self);
}

/* asked first: from Settings, or a long press on the conversation */
static void
vibe_ask_clear (gpointer data)
{
  Window *self = data;
  VibeosDialog *dialog = vibeos_dialog_new (_("Clear the conversation?"));

  vibeos_dialog_set_message (dialog, _("All of the conversation is deleted from the phone, and Vibe forgets it."));
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Clear"), VIBEOS_RESPONSE_OK);
  g_signal_connect (dialog, "response", G_CALLBACK (on_clear_conversation), self);
  vibeos_dialog_present (dialog, self->window);
}

static gboolean
vibe_has_conversation (gpointer data)
{
  Window *self = data;

  return self->history->len || self->ex_said || self->ex_reply->len;
}

static void
on_stage_tapped (DictateStage *stage, Window *self)
{
  /* a touch in a countdown stops it: the card waits */
  if (self->countdown_s)
    {
      countdown_wait (self);
      return;
    }
  talk_mic (self);
}

static void
on_stage_long_pressed (DictateStage *stage, Window *self)
{
  if (vibe_has_conversation (self))
    vibe_ask_clear (self);
}

static void
on_state_changed (DictateEngine *engine, Window *self)
{
  DictateState state = dictate_engine_get_state (engine);

  set_inhibit (self, state != DICTATE_IDLE ||
                     (self->talk && dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE));
  if (state == DICTATE_IDLE && self->talking)
    {
      self->answer_listening = FALSE;
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
      maybe_close (self);
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

  if (!self)
    return;
  /* push-to-talk while Agent answers: it stops at once and listens, the user wants to speak */
  if (self->talking && self->talk && dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE
      && dictate_engine_get_state (self->engine) == DICTATE_IDLE)
    {
      self->stopping_reply = TRUE;
      dictate_talk_stop (self->talk);
      self->stopping_reply = FALSE;
      talk_listen (self);
      show_state (self);
      return;
    }
  if (self->talking)
    talk_mic (self);
  else
    on_mic (DICTATE_MIC (self->mic), self);
}

/* --- the power key held (dictate-main.c "hold") */

void
dictate_window_hold (GtkWidget *window, gboolean held, gboolean locked)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  DictateState state;

  if (!self || !self->talk)
    return;
  state = dictate_engine_get_state (self->engine);
  if (held)
    {
      g_message ("window: the power key is held: listening%s", locked ? ", the phone locked" : "");
      dictate_talk_set_locked (self->talk, locked);
      /* Agent stops speaking at once: the user wants to speak */
      if (dictate_talk_get_state (self->talk) != DICTATE_TALK_IDLE)
        {
          self->stopping_reply = TRUE;
          dictate_talk_stop (self->talk);
          self->stopping_reply = FALSE;
        }
      self->held = TRUE;
      self->drop_when_idle = FALSE;
      /* listening at once, as after a tap on the microphone (a turn already under way is held on) */
      dictate_engine_set_held (self->engine, TRUE);
      if (state == DICTATE_IDLE)
        talk_listen (self);
      self->hold_turn = TRUE;
    }
  else if (self->held)
    {
      /* Let go: Agent listens on until the speaker pauses (at once if they have), then what was said goes to the
       * model, as Android's assistant listens on after its long press. The key is mostly let go before the first
       * word: stopping here ended those turns with nothing heard, the microphone off. */
      self->held = FALSE;
      dictate_engine_set_held (self->engine, FALSE);
    }
  show_state (self);
  maybe_close (self);
}

void
dictate_window_hold_cancel (GtkWidget *window)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  DictateState state;

  if (!self || !self->talk || !self->held)
    return;
  g_message ("window: the power key's hold is given up: nothing is sent");
  self->held = FALSE;
  dictate_engine_set_held (self->engine, FALSE);
  state = dictate_engine_get_state (self->engine);
  /* what was heard goes, as when the microphone is stopped while listening */
  if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  show_state (self);
  maybe_close (self);
}

void
dictate_window_unlocked (GtkWidget *window)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");

  if (self && self->talk)
    dictate_talk_set_locked (self->talk, FALSE);
}

/* --- calls (dictate-calls.h): the app tells the window of them */

/* a window made only to announce a call, or for the power key held on a locked phone, never shown: it goes once the
 * call has stopped ringing (the key is let go) and all is quiet, and with it the models it loaded */
static gboolean
close_idle (gpointer data)
{
  GtkWidget *window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");

  if (self && !gtk_widget_get_visible (window) && !self->ringing && !self->announcing && !self->held
      && dictate_engine_get_state (self->engine) == DICTATE_IDLE
      && dictate_talk_get_state (self->talk) == DICTATE_TALK_IDLE)
    gtk_window_destroy (GTK_WINDOW (window));
  return G_SOURCE_REMOVE;
}

static void
maybe_close (Window *self)
{
  if (self->talk && !gtk_widget_get_visible (self->window) && !self->ringing)
    g_idle_add_full (G_PRIORITY_LOW, close_idle, g_object_ref (self->window), g_object_unref);
}

/* the call that rings, as get_current_call says it (only its name, or what kind of number; in private mode on the
 * speaker, not even the name). `data`: the areas, the private mode's bit above them */
#define CALLER_PRIVATE (1ull << 32)

static void
caller_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  guint64 bits = *(guint64 *) data;
  DictateToolContext ctx = { (guint32) bits, NULL, NULL, NULL, FALSE, FALSE };

  ctx.private_speaker = (bits & CALLER_PRIVATE) && !dictate_output_headphones (NULL);

  g_task_return_pointer (task, dictate_tools_run ("get_current_call", "{}", &ctx, 5000), g_free);
}

/* the caller's initials for the orb, from get_current_call: only a contact's name, never a number's digits */
static char *
caller_initials (const char *found)
{
  g_autoptr (JsonNode) root = found ? json_from_string (found, NULL) : NULL;
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *calls = o && json_object_has_member (o, "calls") ? json_object_get_array_member (o, "calls") : NULL;
  GString *initials = g_string_new (NULL);

  for (guint i = 0; calls && i < json_array_get_length (calls) && !initials->len; i++)
    {
      JsonObject *c = json_array_get_object_element (calls, i);
      const char *with = c ? json_object_get_string_member_with_default (c, "with", NULL) : NULL;
      g_auto (GStrv) words = NULL;

      if (!c || !json_object_get_boolean_member_with_default (c, "in_contacts", FALSE) || !with
          || !g_str_has_prefix (json_object_get_string_member_with_default (c, "state", ""), "ringing"))
        continue;
      words = g_strsplit_set (with, " -", -1);
      for (guint w = 0; words[w] && g_utf8_strlen (initials->str, -1) < 2; w++)
        if (*words[w] && g_unichar_isalpha (g_utf8_get_char (words[w])))
          g_string_append_unichar (initials, g_unichar_toupper (g_utf8_get_char (words[w])));
    }
  return g_string_free (initials, initials->len == 0);
}

/* Answer and Decline on the call's screen: the Phone's call service told at once, as the spoken ones are */
typedef struct {
  const char *tool;
  guint32 areas;
} CallTool;

static void
call_tool_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  CallTool *call = data;
  DictateToolContext ctx = { call->areas, NULL, NULL, NULL, FALSE, FALSE };

  g_task_return_pointer (task, dictate_tools_run (call->tool, "{}", &ctx, 5000), g_free);
}

static void
on_call_tool (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree char *outcome = g_task_propagate_pointer (G_TASK (result), NULL);

  g_message ("window: %s from the call's screen: %s", (const char *) data, outcome ? "done" : "failed");
}

static void
on_call_button (VibeosPressable *button, Window *self)
{
  CallTool *call = g_new (CallTool, 1);
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_call_tool, (gpointer) g_object_get_data (G_OBJECT (button),
                                                                                             "dictate-tool"));

  call->tool = g_object_get_data (G_OBJECT (button), "dictate-tool");
  call->areas = dictate_tools_areas (self->settings);
  g_task_set_task_data (task, call, g_free);
  g_task_run_in_thread (task, call_tool_thread);
  /* the announcement and the listening for an answer stop: the tap was the answer */
  self->stopping_reply = TRUE;
  dictate_talk_stop (self->talk);
  self->stopping_reply = FALSE;
  if (dictate_engine_get_state (self->engine) != DICTATE_IDLE)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  dictate_stage_clear_buttons (DICTATE_STAGE (self->stage));
}

static GtkWidget *
call_button (Window *self, const char *label, DictateStageButtonKind kind, const char *tool)
{
  GtkWidget *button = dictate_stage_add_button (DICTATE_STAGE (self->stage), label, kind);

  g_object_set_data (G_OBJECT (button), "dictate-tool", (gpointer) tool);
  g_signal_connect (button, "clicked", G_CALLBACK (on_call_button), self);
  return button;
}

static void
on_caller (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GtkWidget) window = data;
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  g_autofree char *found = g_task_propagate_pointer (G_TASK (result), NULL);
  g_autofree char *text = NULL, *initials = NULL;
  DictateStage *stage;
  const char *language;

  if (!self || !found || !self->ringing)
    return;
  choose_language (self);
  language = dictate_engine_get_language (self->engine);
  if (!(text = dictate_calls_announce_text (language, found)))
    return;
  g_message ("window: a call rings: it is announced");
  /* whatever went on stops: listening, the words so far dropped, and a reply */
  if (dictate_engine_get_state (self->engine) != DICTATE_IDLE)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  self->stopping_reply = TRUE;
  dictate_talk_stop (self->talk);
  self->stopping_reply = FALSE;
  /* B7: the caller in the orb, what Vibe says of the call, and Decline and Answer */
  vibe_archive (self);
  stage = DICTATE_STAGE (self->stage);
  initials = caller_initials (found);
  dictate_stage_set_call_label (stage, _("Incoming call"));
  dictate_orb_set_look (dictate_stage_get_orb (stage), DICTATE_ORB_CALLER, initials);
  dictate_stage_set_who (stage, _("Vibe says"), FALSE);
  dictate_stage_set_reply (stage, text);
  dictate_stage_set_large_buttons (stage, TRUE);
  call_button (self, _("Decline"), DICTATE_STAGE_BUTTON_DANGER, "decline_call");
  call_button (self, _("Answer"), DICTATE_STAGE_BUTTON_PRIMARY, "answer_call");
  dictate_stage_set_hint (stage, NULL);
  self->reply_failed = FALSE;
  self->announcing = TRUE;
  dictate_talk_announce (self->talk, language, text);
}

void
dictate_window_announce_call (GtkWidget *window, guint id)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");
  g_autoptr (GTask) task = NULL;

  if (!self || !self->talk)
    return;
  self->ringing = id;
  if (self->caller)
    g_cancellable_cancel (self->caller);
  g_clear_object (&self->caller);
  self->caller = g_cancellable_new ();
  task = g_task_new (NULL, self->caller, on_caller, g_object_ref (window));
  {
    guint64 *bits = g_new (guint64, 1);

    *bits = dictate_tools_areas (self->settings)
            | (g_settings_get_boolean (self->settings, "talk-private") ? CALLER_PRIVATE : 0);
    g_task_set_task_data (task, bits, g_free);
  }
  g_task_run_in_thread (task, caller_thread);
}

void
dictate_window_call_stopped (GtkWidget *window, guint id)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");

  if (!self || !self->talk || self->ringing != id)
    return;
  self->ringing = 0;
  /* answered or declined on the screen, or missed: no more announcing, and no listening for it */
  if (self->announcing)
    {
      self->announcing = FALSE;
      self->stopping_reply = TRUE;
      dictate_talk_stop (self->talk);
      self->stopping_reply = FALSE;
    }
  if (self->answer_listening && dictate_engine_get_state (self->engine) != DICTATE_IDLE)
    {
      self->drop_when_idle = TRUE;
      dictate_engine_stop (self->engine);
    }
  /* the call's screen goes; what Vibe said of it stays, as any reply */
  dictate_stage_set_call_label (DICTATE_STAGE (self->stage), NULL);
  dictate_stage_clear_buttons (DICTATE_STAGE (self->stage));
  dictate_stage_set_large_buttons (DICTATE_STAGE (self->stage), FALSE);
  show_state (self);
  maybe_close (self);
}

void
dictate_window_calls_changed (GtkWidget *window, GPtrArray *calls)
{
  Window *self = g_object_get_data (G_OBJECT (window), "dictate-window");

  DictateCall *on = NULL;

  if (!self || !self->talk)
    return;
  /* for the model: what "answer", "hang up" or "mute" would be about */
  dictate_talk_set_context (self->talk, dictate_calls_context (calls));
  /* F3: the call to show: one going on, else one on hold, else one being placed */
  for (guint i = 0; calls && i < calls->len; i++)
    {
      DictateCall *c = calls->pdata[i];

      if (c->state == DICTATE_CALL_ACTIVE)
        {
          on = c;
          break;
        }
      if (c->state == DICTATE_CALL_ONHOLD || c->state == DICTATE_CALL_DIALING || c->state == DICTATE_CALL_CONNECTING)
        on = on ? on : c;
    }
  if (!on)
    {
      if (!self->call_on)
        return;
      self->call_on = 0;
      g_clear_pointer (&self->call_info, g_free);
      g_clear_handle_id (&self->call_tick_id, g_source_remove);
      dictate_stage_set_title (DICTATE_STAGE (self->stage), _("Vibe"));
      dictate_stage_set_banner (DICTATE_STAGE (self->stage), NULL);
      vibe_show (self);
      return;
    }
  if (on->id != self->call_on)
    g_clear_pointer (&self->call_info, g_free);
  self->call_on = on->id;
  /* in a call, the bar says so, over the call's stage and over what is asked in it */
  dictate_stage_set_title (DICTATE_STAGE (self->stage), _("Call"));
  self->call_connected = on->connected;
  self->call_state = on->state;
  if (!self->call_tick_id)
    self->call_tick_id = g_timeout_add_seconds (1, call_tick, self);
  call_fetch (self);
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
  if (response == VIBEOS_RESPONSE_OK)
    gtk_text_buffer_set_text (self->buffer, "", -1);
}

/* the bin: asked first, then the text goes */
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

/* Agent's speaker: the replies spoken, or only written; kept in the settings */
/* Speak the replies, in Vibe's settings */
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
}

/* Speech rate in Vibe's settings: the next sentence is at the new pace */
static void
on_speed_changed (GSettings *settings, const char *key, Window *self)
{
  dictate_talk_set_speed (self->talk, g_settings_get_double (settings, "talk-speed"));
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

static const VibeosAboutInfo agent_about = { .name = "Vibe", .icon_name = "vibeos.Agent", .legal = agent_legal };

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
settings_about (gpointer data)
{
  Window *self = data;

  vibeos_about_present (self->window, &agent_about);
}

static void
open_settings (gpointer data)
{
  Window *self = data;
  DictateSettingsHooks hooks = {
    .settings = self->settings, .talk = self->talk, .language = settings_language,
    .choose_language = settings_choose_language, .back = settings_back,
    .pages = VIBEOS_PAGE_STACK (self->pages), .has_conversation = vibe_has_conversation,
    .clear_conversation = vibe_ask_clear, .about = settings_about, .data = self,
  };

  if (vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (self->pages)) > 1)
    return;
  /* what is heard while Settings is open does not go to the model: the listening stops, and what it heard is dropped */
  if (self->talking)
    {
      DictateState state = dictate_engine_get_state (self->engine);

      if (state == DICTATE_STARTING || state == DICTATE_LISTENING)
        {
          self->drop_when_idle = TRUE;
          dictate_engine_stop (self->engine);
        }
    }
  vibeos_page_stack_push (VIBEOS_PAGE_STACK (self->pages), dictate_settings_page_new (self->window, &hooks));
}

/* --- Vibe's introduction, once (dictate-onboarding.h) */

static void
intro_set_language (const char *language, gpointer data)
{
  on_language_ready (language, data);
}

static void
intro_done (const char *said, GBytes *audio, gpointer data)
{
  Window *self = data;

  vibeos_page_stack_pop (VIBEOS_PAGE_STACK (self->pages));
  if (!said)
    return;
  if (!*said)
    {
      talk_listen (self);
      show_state (self);
      return;
    }
  /* a thing to try, tapped: the user's own question, not their voice */
  vibe_send (self, said, audio, audio ? DICTATE_ASK_NONE : DICTATE_ASK_TAPPED);
}

/* on the window's first showing, unless a call rings (the window shown for it): the introduction, if it is due */
static gboolean
intro_maybe (gpointer data)
{
  Window *self = data;
  DictateOnboardingHooks hooks = {
    .language = settings_language, .set_language = intro_set_language, .done = intro_done, .data = self,
  };
  GtkWidget *intro;

  self->intro_id = 0;
  if (!g_settings_get_boolean (self->settings, "first-run") || self->ringing
      || vibeos_page_stack_get_depth (VIBEOS_PAGE_STACK (self->pages)) > 1)
    return G_SOURCE_REMOVE;
  intro = dictate_onboarding_new (self->window, self->settings, self->talk, &hooks);
  vibeos_page_stack_push (VIBEOS_PAGE_STACK (self->pages), intro);
  dictate_onboarding_start (intro);
  return G_SOURCE_REMOVE;
}

static void
on_window_map (GtkWidget *window, Window *self)
{
  if (!self->intro_id)
    self->intro_id = g_idle_add (intro_maybe, self);
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

  vibeos_menu_append_about (menu, &dictate_about);
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
  if (!self->mic)
    return;
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

  if (self->talking)
    return;
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (self->view), margin);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (self->view), margin);

  self->fab_row = vibeos_layout_is_tablet (layout) ? FAB_ROW_TABLET : FAB_ROW;
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (self->view), self->fab_row + 16);
  gtk_widget_set_margin_bottom (self->mic, (self->fab_row - FAB_SIZE) / 2 + (int) vibeos_snackbar_get_lift (self->snackbar));
}

/* the theme changed: the words still being recognised in its colour */
static void
update_partial (Window *self)
{
  GdkRGBA variant = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);

  if (self->partial)
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
  g_clear_handle_id (&self->intro_id, g_source_remove);
  g_clear_handle_id (&self->countdown_id, g_source_remove);
  g_clear_handle_id (&self->call_tick_id, g_source_remove);
  if (self->call_fetch)
    g_cancellable_cancel (self->call_fetch);
  g_clear_object (&self->call_fetch);
  g_clear_pointer (&self->call_info, g_free);
  g_clear_weak_pointer (&self->call_banner);
  g_clear_weak_pointer (&self->call_banner_text);
  g_clear_pointer (&self->call_banner_key, g_free);
  if (self->caller)
    g_cancellable_cancel (self->caller);
  g_clear_object (&self->caller);
  g_signal_handlers_disconnect_by_data (self->engine, self);
  dictate_engine_stop (self->engine);
  g_clear_object (&self->engine);
  if (self->talk)
    g_signal_handlers_disconnect_by_data (self->talk, self);
  g_clear_object (&self->talk);
  if (self->confirming)
    g_cancellable_cancel (self->confirming);
  g_clear_object (&self->confirming);
  g_string_free (self->heard, TRUE);
  g_free (self->said);
  g_free (self->ex_said);
  g_clear_pointer (&self->ex_audio, g_bytes_unref);
  g_string_free (self->ex_reply, TRUE);
  g_free (self->ex_note);
  g_free (self->ex_access);
  g_clear_pointer (&self->history, g_ptr_array_unref);
  g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  g_free (self->speaking);
  g_free (self->path);
  g_free (self);
}

/* Dictate's page: the app bar, the document, the microphone */
static GtkWidget *
dictate_content (Window *self)
{
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *title;
  GtkWidget *page = self->page = gtk_overlay_new ();
  GtkTextIter start;

  /* the app bar */
  title = gtk_label_new (_("Dictate"));
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
  g_signal_connect (self->mic, "clicked", G_CALLBACK (on_mic), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), self->mic);

  self->snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());
  g_signal_connect (self->snackbar, "notify::lift", G_CALLBACK (on_lift), self);
  gtk_overlay_add_overlay (GTK_OVERLAY (page), GTK_WIDGET (self->snackbar));

  gtk_box_append (GTK_BOX (content), page);
  return content;
}

/* Vibe's: the conversation, its column kept to a phone's width in the middle of a wider window */
static GtkWidget *
vibe_content (Window *self)
{
  GtkWidget *overlay = gtk_overlay_new ();

  self->stage = dictate_stage_new (self->engine);
  g_signal_connect (self->stage, "tapped", G_CALLBACK (on_stage_tapped), self);
  g_signal_connect (self->stage, "long-pressed", G_CALLBACK (on_stage_long_pressed), self);
  g_signal_connect_swapped (self->stage, "settings", G_CALLBACK (open_settings), self);
  gtk_widget_add_css_class (overlay, "dictate-stage");
  gtk_overlay_set_child (GTK_OVERLAY (overlay), vibeos_clamp_new (self->stage, COLUMN_MAX));
  self->snackbar = VIBEOS_SNACKBAR (vibeos_snackbar_new ());
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (self->snackbar));
  return overlay;
}

GtkWidget *
dictate_window_new (GtkApplication *app, gboolean agent)
{
  Window *self = g_new0 (Window, 1);
  GtkWidget *content;

  self->window = vibeos_window_new (app);
  self->engine = dictate_engine_new ();
  self->talking = agent;
  self->talk = agent ? dictate_talk_new () : NULL;
  self->heard = g_string_new (NULL);
  self->ex_reply = g_string_new (NULL);
  self->history = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_history_entry_free);
  self->settings = g_settings_new ("vibeos.Dictate");
  self->path = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "document.txt", NULL);
  g_object_set_data_full (G_OBJECT (self->window), "dictate-window", self, window_free);
  gtk_window_set_title (GTK_WINDOW (self->window), agent ? _("Vibe") : _("Dictate"));

  content = agent ? vibe_content (self) : dictate_content (self);
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
                                    agent ? _("Vibe") : _("Dictate"), -1);
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
      g_signal_connect (self->talk, "busy", G_CALLBACK (on_talk_busy), self);
      g_signal_connect (self->talk, "refused", G_CALLBACK (on_talk_refused), self);
      g_signal_connect (self->talk, "acted", G_CALLBACK (on_talk_acted), self);
      g_signal_connect (self->talk, "reply-info", G_CALLBACK (on_talk_reply_info), self);
      g_signal_connect (self->talk, "written", G_CALLBACK (on_talk_written), self);
      g_signal_connect (self->talk, "no-access", G_CALLBACK (on_talk_no_access), self);
      g_signal_connect (self->talk, "tool", G_CALLBACK (on_talk_tool), self);
      g_signal_connect (self->talk, "suggestions", G_CALLBACK (on_talk_suggestions), self);
      g_signal_connect (self->talk, "choices", G_CALLBACK (on_talk_choices), self);
      g_signal_connect (self->talk, "facts", G_CALLBACK (on_talk_facts), self);
      g_signal_connect (self->talk, "person", G_CALLBACK (on_talk_person), self);
      dictate_talk_set_speed (self->talk, g_settings_get_double (self->settings, "talk-speed"));
      dictate_talk_set_speak (self->talk, g_settings_get_boolean (self->settings, "talk-speak"));
      g_signal_connect (self->settings, "changed::talk-speak", G_CALLBACK (on_speak_changed), self);
      g_signal_connect (self->settings, "changed::talk-speed", G_CALLBACK (on_speed_changed), self);
      dictate_owner_probe ();
      vibe_restore (self);
      g_signal_connect (self->window, "map", G_CALLBACK (on_window_map), self);
      g_signal_connect (self->window, "notify::is-active", G_CALLBACK (on_window_active), self);
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
