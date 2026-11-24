/* dictate-voicematch-page: see dictate-voicematch-page.h. */
#include "dictate-voicematch-page.h"
#include "dictate-enroll.h"
#include "dictate-owner.h"
#include "dictate-voicematch.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define APP_ID "vibeos.Agent"   /* who asks for the PIN, as the lock screen names it */

typedef struct {
  GtkWidget *window, *page, *body;
  VibeosPageStack *pages;
  GSettings *settings;
  DictateTalk *talk;
  char *language;
  GtkWidget *only, *strict_label, *strict_needed, *strict_note;
  GtkAdjustment *strictness;
  GCancellable *cancellable;
  gboolean syncing;             /* the slider set from the setting, not by the user */
} Page;

static void build (Page *p);

/* the saved sample was made for another model than the one installed now: the check is not made until the owner
 * records it again. dictate_voice_match_stale () is the engine's; until the engine has it, it is a weak reference
 * and no sample is ever stale */
gboolean dictate_voice_match_stale (void) __attribute__ ((weak));

static gboolean
sample_stale (void)
{
  return dictate_voice_match_stale && dictate_voice_match_stale ();
}

/* the strictness's name: 0..33 lenient, ..66 balanced, the rest strict */
static const char *
strict_label (int strictness)
{
  return strictness < 34 ? _("Lenient") : strictness < 67 ? _("Balanced") : _("Strict");
}

char *
dictate_voicematch_summary (GSettings *settings)
{
  int strictness = g_settings_get_int (settings, "voice-match-strictness");

  if (!dictate_voice_match_enrolled ())
    return g_strdup (dictate_voice_match_ready () ? _("Off · no voice sample yet") : _("Not on this phone yet"));
  /* a sample, and nothing to check it with: the model's package removed */
  if (!dictate_voice_match_ready ())
    return g_strdup (_("Off · the voice model is not installed"));
  if (sample_stale ())
    return g_strdup (_("Record your voice again"));
  if (!g_settings_get_boolean (settings, "voice-match"))
    return g_strdup (_("Off · voice sample saved"));
  /* TRANSLATORS: Settings › Voice match's summary; the %s is how close: lenient, balanced or strict */
  return g_strdup_printf (_("On · %s match"),
                          strictness < 34 ? _("lenient") : strictness < 67 ? _("balanced") : _("strict"));
}

static void
page_free (gpointer data)
{
  Page *p = data;

  g_cancellable_cancel (p->cancellable);
  g_clear_object (&p->cancellable);
  g_signal_handlers_disconnect_by_data (p->settings, p);
  g_clear_object (&p->settings);
  g_clear_object (&p->talk);
  g_free (p->language);
  g_free (p);
}

/* --- the recording, and the test, on their own screens over this page */

static void
on_enrolled (gboolean saved, gpointer data)
{
  Page *p = data;

  vibeos_page_stack_pop (p->pages);
  build (p);
}

static void
record (Page *p)
{
  GtkWidget *enroll = dictate_enroll_new (p->settings, p->talk, NULL, p->language, _("Cancel"), on_enrolled, p);

  vibeos_page_stack_push (p->pages, enroll);
  dictate_enroll_start (enroll);
}

static void
on_tested (gboolean saved, gpointer data)
{
  Page *p = data;

  vibeos_page_stack_pop (p->pages);
}

static void
on_test (VibeosPressable *button, Page *p)
{
  vibeos_page_stack_push (p->pages, dictate_enroll_test_new (p->settings, p->language, on_tested, p));
}

/* --- what needs the owner: their PIN first, when the phone has one */

typedef void (*OwnerDone) (Page *p);

typedef struct {
  Page *p;
  OwnerDone then;
} Owner;

static void
on_owner (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree Owner *o = data;
  g_autoptr (GError) error = NULL;

  if (dictate_owner_confirm_finish (result, &error))
    o->then (o->p);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("voice match: the owner was not confirmed: %s", error->message);
}

static void
as_owner (Page *p, OwnerDone then)
{
  Owner *o;

  if (!dictate_owner_has_secret ())
    {
      then (p);
      return;
    }
  o = g_new0 (Owner, 1);
  o->p = p;
  o->then = then;
  dictate_owner_confirm_async (APP_ID, p->cancellable, on_owner, o);
}

static void
on_record (VibeosPressable *button, Page *p)
{
  /* no sample yet: nobody's voice is replaced, so no PIN */
  record (p);
}

static void
on_rerecord (VibeosListRow *row, Page *p)
{
  as_owner (p, record);
}

static void
on_record_again (VibeosPressable *button, Page *p)
{
  as_owner (p, record);
}

static void
delete_sample (Page *p)
{
  g_autoptr (GError) error = NULL;

  if (!dictate_voice_match_delete (&error))
    g_warning ("voice match: the sample was not deleted: %s", error->message);
  else
    g_settings_set_boolean (p->settings, "voice-match", FALSE);
  build (p);
}

/* the dialog's answer, while the page is still there */
static void
on_delete_response (VibeosDialog *dialog, int response, GtkWidget *page)
{
  Page *p = g_object_get_data (G_OBJECT (page), "dictate-voicematch");

  if (p && response == VIBEOS_RESPONSE_OK)
    as_owner (p, delete_sample);
}

static void
on_delete (VibeosListRow *row, Page *p)
{
  VibeosDialog *dialog = vibeos_dialog_new (_("Delete voice sample?"));

  vibeos_dialog_set_message (dialog, _("Voice match turns off and Vibe takes commands from any voice until you record "
                                       "a new sample."));
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (dialog, _("Delete"), VIBEOS_RESPONSE_OK);
  g_signal_connect_object (dialog, "response", G_CALLBACK (on_delete_response), p->page, 0);
  vibeos_dialog_present (dialog, p->window);
}

/* --- how close a match */

static void
show_strictness (Page *p)
{
  int strictness = g_settings_get_int (p->settings, "voice-match-strictness");
  /* TRANSLATORS: under Voice match's slider: 75% match needed */
  g_autofree char *needed = g_strdup_printf (_("%.0f%% match needed"), dictate_voice_match_needed (strictness));

  gtk_label_set_text (GTK_LABEL (p->strict_label), strict_label (strictness));
  gtk_label_set_text (GTK_LABEL (p->strict_needed), needed);
  gtk_label_set_text (GTK_LABEL (p->strict_note),
                      strictness < 34 ? _("Works with a cold or in noise, but a similar voice may get through.")
                      : strictness < 67 ? _("Right for most people and rooms.")
                      : _("Hardest to imitate. You may need to repeat yourself in noise."));
  p->syncing = TRUE;
  gtk_adjustment_set_value (p->strictness, strictness);
  p->syncing = FALSE;
}

static void
on_strictness (GtkAdjustment *adjustment, Page *p)
{
  if (!p->syncing)
    g_settings_set_int (p->settings, "voice-match-strictness", (int) (gtk_adjustment_get_value (adjustment) + 0.5));
}

static void
on_setting (GSettings *settings, const char *key, Page *p)
{
  if (!p->only)
    return;
  if (!g_strcmp0 (key, "voice-match-strictness"))
    show_strictness (p);
  else if (!g_strcmp0 (key, "voice-match"))
    vibeos_list_row_set_summary (VIBEOS_LIST_ROW (p->only),
                                 !g_settings_get_boolean (settings, key) ? _("Off, Vibe answers anyone")
                                 /* on, but nothing to check a voice with */
                                 : !dictate_voice_match_ready () || sample_stale () ? _("Not in effect until voices "
                                                                                        "can be checked again")
                                 : _("Other voices can't give Vibe commands"));
}

static GtkWidget *
label_new (const char *css, const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_add_css_class (label, css);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  return label;
}

static GtkWidget *
pill_new (const char *label, gboolean primary)
{
  GtkWidget *text = gtk_label_new (label);
  GtkWidget *button = vibeos_pressable_new (text);

  gtk_widget_add_css_class (button, "dictate-pill");
  gtk_widget_add_css_class (button, primary ? "dictate-pill-primary" : "dictate-pill-tonal");
  vibeos_pressable_set_fill_role (VIBEOS_PRESSABLE (button),
                                  primary ? VIBEOS_PRIMARY : VIBEOS_SECONDARY_CONTAINER, FALSE);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (button), VIBEOS_RIPPLE_BOUNDED, 24);
  return button;
}

static GtkWidget *
switch_row (Page *p, GtkWidget *cards, const char *title, const char *summary, const char *key)
{
  GtkWidget *row = vibeos_list_row_new (title, summary);
  GtkWidget *toggle = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);

  g_settings_bind (p->settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  return row;
}

/* when the sample was made: today, yesterday, or its date */
static char *
recorded_when (void)
{
  gint64 at = dictate_voice_match_recorded ();
  guint n = dictate_voice_match_phrases ();
  g_autoptr (GDateTime) then = at ? g_date_time_new_from_unix_local (at) : NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int days;

  if (!then)
    /* TRANSLATORS: Voice match's sample: 4 phrases */
    return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u phrase", "%u phrases", n), n);
  days = g_date_time_get_day_of_year (now) - g_date_time_get_day_of_year (then)
         + 366 * (g_date_time_get_year (now) - g_date_time_get_year (then));
  if (days == 0)
    /* TRANSLATORS: Voice match's sample: 4 phrases · recorded today */
    return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u phrase · recorded today", "%u phrases · recorded today",
                                         n), n);
  if (days == 1)
    return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u phrase · recorded yesterday",
                                         "%u phrases · recorded yesterday", n), n);
  {
    /* TRANSLATORS: a date, as in "recorded on 3 March 2026" (strftime) */
    g_autofree char *date = g_date_time_format (then, _("%e %B %Y"));

    g_strstrip (date);
    /* TRANSLATORS: Voice match's sample: 4 phrases · recorded on 3 March 2026 */
    return g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "%u phrase · recorded on %s",
                                         "%u phrases · recorded on %s", n), n, date);
  }
}

/* the page's body as things are: with no sample, the card that offers one; with it, everything else */
static void
build (Page *p)
{
  GtkWidget *child, *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gboolean coming;

  while ((child = gtk_widget_get_first_child (p->body)))
    gtk_box_remove (GTK_BOX (p->body), child);
  p->only = p->strict_label = p->strict_needed = p->strict_note = NULL;
  p->strictness = NULL;

  /* the voice check does not run on this phone yet: nothing to record a sample for */
  coming = !dictate_voice_match_enrolled () && !dictate_voice_match_ready ();
  gtk_box_append (GTK_BOX (box),
                  label_new ("dictate-vm-intro",
                             coming ? _("Vibe can't tell voices apart on this phone yet, so it answers anyone who "
                                        "speaks to it.")
                                    : _("Vibe learns your voice and won't carry out commands from anyone who doesn't "
                                        "sound like you. The sample never leaves this phone.")));
  if (coming)
    {
      GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

      gtk_widget_add_css_class (card, "dictate-vm-card");
      gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-title", _("Voice match is coming")));
      gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-sub",
                                                 _("Then Vibe learns your voice and won't carry out commands from "
                                                   "anyone who doesn't sound like you. Recording your voice will be "
                                                   "offered here.")));
      gtk_box_append (GTK_BOX (box), card);
    }
  else if (!dictate_voice_match_enrolled ())
    {
      GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *button;
      guint n = DICTATE_VOICE_MATCH_PHRASES;
      /* TRANSLATORS: Voice match with no sample yet */
      g_autofree char *how = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                                           "Read %u short phrase aloud. About a minute, somewhere "
                                                           "quiet.",
                                                           "Read %u short phrases aloud. About a minute, somewhere "
                                                           "quiet.", n), n);

      gtk_widget_add_css_class (card, "dictate-vm-card");
      gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-title", _("No voice sample yet")));
      gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-sub", how));
      button = pill_new (_("Record my voice"), TRUE);
      g_signal_connect (button, "clicked", G_CALLBACK (on_record), p);
      gtk_box_append (GTK_BOX (card), button);
      gtk_box_append (GTK_BOX (box), card);
    }
  else
    {
      GtkWidget *cards = vibeos_card_list_new (), *row, *again, *strict, *top, *scale, *ends, *test;
      g_autofree char *when = recorded_when ();

      /* a sample the check cannot use: the model it was made for has gone, or been replaced by an update */
      if (!dictate_voice_match_ready ())
        {
          GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

          gtk_widget_add_css_class (card, "dictate-vm-card");
          gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-title", _("The voice model is not installed")));
          gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-sub",
                                                     _("Your voice sample is kept, but Vibe can't check voices "
                                                       "until the model is installed again.")));
          gtk_box_append (GTK_BOX (box), card);
        }
      else if (sample_stale ())
        {
          GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *button;

          gtk_widget_add_css_class (card, "dictate-vm-card");
          gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-title", _("Record your voice again")));
          gtk_box_append (GTK_BOX (card), label_new ("dictate-vm-card-sub",
                                                     _("Voice match was updated, and your sample was made for the "
                                                       "earlier version. Vibe can't check voices with it.")));
          button = pill_new (_("Record again"), TRUE);
          /* the old sample is still the owner's: replacing it asks for the PIN, as Re-record does */
          g_signal_connect (button, "clicked", G_CALLBACK (on_record_again), p);
          gtk_box_append (GTK_BOX (card), button);
          gtk_box_append (GTK_BOX (box), card);
        }

      p->only = switch_row (p, cards, _("Only answer my voice"), NULL, "voice-match");
      on_setting (p->settings, "voice-match", p);

      row = vibeos_list_row_new (_("Voice sample"), when);
      /* recorded again only where a recording can be made into a sample; a stale one has its card above for it */
      if (dictate_voice_match_ready () && !sample_stale ())
        {
          again = gtk_label_new (_("Re-record"));
          gtk_widget_add_css_class (again, "dictate-row-action");
          gtk_widget_set_can_target (again, FALSE);
          vibeos_list_row_set_end_widget (VIBEOS_LIST_ROW (row), again);
          g_signal_connect (row, "clicked", G_CALLBACK (on_rerecord), p);
        }
      else
        vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), FALSE);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);

      row = vibeos_list_row_new (_("Delete voice sample"), _("Vibe will answer anyone again"));
      gtk_widget_add_css_class (row, "dictate-row-danger");
      g_signal_connect (row, "clicked", G_CALLBACK (on_delete), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);

      /* how close a match: the slider on a card of its own */
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("How close a match"));
      strict = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (strict, "dictate-vm-strict");
      top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      p->strict_label = label_new ("dictate-vm-strict-label", NULL);
      gtk_widget_set_hexpand (p->strict_label, TRUE);
      p->strict_needed = label_new ("dictate-vm-strict-needed", NULL);
      gtk_box_append (GTK_BOX (top), p->strict_label);
      gtk_box_append (GTK_BOX (top), p->strict_needed);
      gtk_box_append (GTK_BOX (strict), top);
      p->strictness = gtk_adjustment_new (50, 0, 100, 1, 10, 0);
      scale = gtk_scale_new (GTK_ORIENTATION_HORIZONTAL, p->strictness);
      gtk_scale_set_draw_value (GTK_SCALE (scale), FALSE);
      gtk_widget_add_css_class (scale, "dictate-vm-scale");
      gtk_accessible_update_property (GTK_ACCESSIBLE (scale), GTK_ACCESSIBLE_PROPERTY_LABEL, _("How close a match"),
                                      -1);
      g_signal_connect (p->strictness, "value-changed", G_CALLBACK (on_strictness), p);
      gtk_box_append (GTK_BOX (strict), scale);
      ends = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (ends, "dictate-vm-ends");
      child = label_new ("dictate-vm-end", _("Lenient"));
      gtk_widget_set_hexpand (child, TRUE);
      gtk_box_append (GTK_BOX (ends), child);
      gtk_box_append (GTK_BOX (ends), label_new ("dictate-vm-end", _("Strict")));
      gtk_box_append (GTK_BOX (strict), ends);
      p->strict_note = label_new ("dictate-vm-strict-note", NULL);
      gtk_box_append (GTK_BOX (strict), p->strict_note);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), strict);
      show_strictness (p);

      /* what the owner's voice is asked for (dictate_tools_risk ()) */
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Ask for my voice on"));
      switch_row (p, cards, _("Everyday questions"), _("Time, weather, alarms, timers"), "voice-match-everyday");
      switch_row (p, cards, _("Personal information"), _("Reading messages, calendar, contacts"),
                  "voice-match-personal");
      switch_row (p, cards, _("Actions"), _("Calling, sending, deleting, changing settings"), "voice-match-actions");
      vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 0, 0);
      gtk_box_append (GTK_BOX (box), cards);

      /* a test only where the check can be made */
      if (dictate_voice_match_ready () && !sample_stale ())
        {
          test = pill_new (_("Test voice match"), FALSE);
          gtk_widget_add_css_class (test, "dictate-vm-test");
          g_signal_connect (test, "clicked", G_CALLBACK (on_test), p);
          gtk_box_append (GTK_BOX (box), test);
        }
    }
  gtk_box_append (GTK_BOX (p->body), vibeos_clamp_new (box, VIBEOS_CLAMP_SETTINGS));
}

static void
on_back (VibeosTopBar *bar, Page *p)
{
  vibeos_page_stack_pop (p->pages);
}

GtkWidget *
dictate_voicematch_page_new (GtkWidget *window, VibeosPageStack *pages, GSettings *settings, DictateTalk *talk,
                             const char *language)
{
  Page *p = g_new0 (Page, 1);
  GtkWidget *bar = vibeos_top_bar_new (_("Voice match")), *scroll = spongy_scroll_view_new ();

  p->window = window;
  p->pages = pages;
  p->settings = g_object_ref (settings);
  p->talk = talk ? g_object_ref (talk) : NULL;
  p->language = g_strdup (language);
  p->cancellable = g_cancellable_new ();
  p->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->page, "dictate-settings");
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_back), p);
  gtk_box_append (GTK_BOX (p->page), bar);
  p->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->body, "dictate-vm-body");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), p->body);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (p->page), scroll);
  g_signal_connect (settings, "changed", G_CALLBACK (on_setting), p);
  g_object_set_data_full (G_OBJECT (p->page), "dictate-voicematch", p, page_free);
  build (p);
  return p->page;
}
