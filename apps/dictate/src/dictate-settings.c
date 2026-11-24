/* dictate-settings: see dictate-settings.h. */
#include "dictate-settings.h"
#include "dictate-access.h"
#include "dictate-download.h"
#include "dictate-download-dialog.h"
#include "dictate-confirm-page.h"
#include "dictate-debuglog-page.h"
#include "dictate-models.h"
#include "dictate-tools.h"
#include "dictate-voice.h"
#include "dictate-voicematch-page.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

typedef struct {
  DictateSettingsHooks hooks;
  GtkWidget *window;
  GtkWidget *page, *language, *rate, *gender, *verbosity, *quiet, *voice, *confirm, *clear, *viewer;
  GPtrArray *languages;         /* their rows, in dictate_languages' order */
} Page;

static void
page_free (gpointer data)
{
  Page *p = data;

  if (g_object_get_data (G_OBJECT (p->window), "dictate-settings-page") == p->page)
    g_object_set_data (G_OBJECT (p->window), "dictate-settings-page", NULL);
  g_signal_handlers_disconnect_by_data (p->hooks.settings, p);
  g_clear_object (&p->hooks.settings);
  g_clear_object (&p->hooks.talk);
  g_ptr_array_unref (p->languages);
  g_free (p);
}

static void
refresh (Page *p)
{
  guint n;
  const DictateLanguage *languages = dictate_languages (&n);
  const char *current = p->hooks.language (p->hooks.data);
  const DictateLanguage *now = dictate_language_lookup (current);

  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->language), now ? now->name : NULL);
  /* a man's or a woman's voice, only where the language's voice has both */
  gtk_widget_set_visible (p->gender, dictate_voice_has_genders (current));
  for (guint i = 0; i < n; i++)
    {
      VibeosListRow *row = p->languages->pdata[i];
      g_autofree char *note = dictate_download_note (languages[i].code, p->hooks.talk);
      guint64 downloaded = dictate_download_downloaded_size (languages[i].code);
      gboolean used = g_strcmp0 (languages[i].code, current) == 0, lacks = note != NULL;
      g_autofree char *summary = NULL;

      if (note)
        summary = g_steal_pointer (&note);
      else if (downloaded)
        {
          g_autofree char *size = g_format_size (downloaded);
          /* TRANSLATORS: a language in Vibe's settings; %s is a size: 90.4 MB */
          summary = g_strdup_printf (_("Downloaded, %s"), size);
        }
      else
        summary = g_strdup (_("On the phone"));
      vibeos_list_row_set_summary (row, summary);
      vibeos_list_row_clear_end (row);
      if (lacks)
        {
          /* what it lacks is downloaded with a tap */
          GtkWidget *get = gtk_label_new (_("Get"));

          gtk_widget_add_css_class (get, "dictate-row-action");
          gtk_widget_set_can_target (get, FALSE);
          vibeos_list_row_set_end_widget (row, get);
        }
      else
        vibeos_list_row_set_value (row, used ? _("In use") : NULL);
      /* a tap downloads what it lacks, or removes what was downloaded (not of the language in use) */
      vibeos_list_row_set_activatable (row, lacks || (downloaded && !used));
    }
  {
    g_autofree char *voice = dictate_voicematch_summary (p->hooks.settings);

    g_autofree char *confirm = dictate_confirm_summary (p->hooks.settings);

    vibeos_list_row_set_summary (VIBEOS_LIST_ROW (p->voice), voice);
    vibeos_list_row_set_summary (VIBEOS_LIST_ROW (p->confirm), confirm);
  }
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (p->clear), p->hooks.has_conversation (p->hooks.data));
}

void
dictate_settings_page_refresh (GtkWidget *page)
{
  Page *p = g_object_get_data (G_OBJECT (page), "dictate-settings");

  if (p)
    refresh (p);
}

/* how fast Vibe speaks (talk-speed): up to three times its voice's pace, for people used to fast speech */
static const struct { double speed; const char *label; } RATES[] = {
  { 0.75, N_("Slower") }, { 1.0, N_("Normal") }, { 1.25, "1.25×" }, { 1.5, "1.5×" }, { 2.0, "2×" }, { 2.5, "2.5×" },
  { 3.0, "3×" },
};

/* who speaks (talk-voice-gender), where the voice has a man and a woman */
static const char *const GENDERS[] = { "male", "female" };

static const char *
gender_label (guint i)
{
  if (i)
    /* TRANSLATORS: Vibe's voice in Settings: a woman speaks (Russian's voice has a man's and a woman's) */
    return C_("voice gender", "Female");
  /* TRANSLATORS: Vibe's voice in Settings: a man speaks (Russian's voice has a man's and a woman's) */
  return C_("voice gender", "Male");
}

static guint
gender_index (GSettings *settings)
{
  g_autofree char *value = g_settings_get_string (settings, "talk-voice-gender");

  return !g_strcmp0 (value, GENDERS[1]);
}

/* how much Vibe says (talk-verbosity) */
static const struct { const char *value, *label, *summary; } VERBOSITY[] = {
  { "short", N_("Short"), N_("A sentence") },
  { "normal", N_("Normal"), N_("One to three sentences") },
  { "detailed", N_("Detailed"), N_("More, when the question needs it") },
};

/* when Vibe says nothing on its own (quiet-hours) */
static const struct { const char *value, *label; } QUIET[] = {
  { "", N_("Off") }, { "22:00-07:00", "22:00–07:00" }, { "23:00-07:00", "23:00–07:00" },
  { "21:00-06:00", "21:00–06:00" }, { "00:00-08:00", "00:00–08:00" },
};

static guint
rate_index (GSettings *settings)
{
  double speed = g_settings_get_double (settings, "talk-speed");
  guint best = 1;

  for (guint i = 0; i < G_N_ELEMENTS (RATES); i++)
    if (ABS (RATES[i].speed - speed) < ABS (RATES[best].speed - speed))
      best = i;
  return best;
}

static guint
verbosity_index (GSettings *settings)
{
  g_autofree char *value = g_settings_get_string (settings, "talk-verbosity");

  for (guint i = 0; i < G_N_ELEMENTS (VERBOSITY); i++)
    if (!g_strcmp0 (VERBOSITY[i].value, value))
      return i;
  return 1;
}

/* the quiet hours' row, or -1 for a range not among them (set some other way) */
static int
quiet_index (GSettings *settings)
{
  g_autofree char *value = g_settings_get_string (settings, "quiet-hours");

  for (guint i = 0; i < G_N_ELEMENTS (QUIET); i++)
    if (!g_strcmp0 (QUIET[i].value, value))
      return i;
  return -1;
}

static void
show_choices (Page *p)
{
  int quiet = quiet_index (p->hooks.settings);
  g_autofree char *range = quiet < 0 ? g_settings_get_string (p->hooks.settings, "quiet-hours") : NULL;

  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->rate), _(RATES[rate_index (p->hooks.settings)].label));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->gender), gender_label (gender_index (p->hooks.settings)));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->verbosity),
                             _(VERBOSITY[verbosity_index (p->hooks.settings)].label));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->quiet), quiet < 0 ? range : _(QUIET[quiet].label));
}

static void
on_setting_changed (GSettings *settings, const char *key, Page *p)
{
  /* the language is the window's to say (dictate_settings_page_refresh): its setting changes before the engine's */
  if (g_str_equal (key, "talk-voice"))
    refresh (p);
  else if (g_str_equal (key, "talk-speed") || g_str_equal (key, "talk-verbosity") || g_str_equal (key, "quiet-hours")
           || g_str_equal (key, "talk-voice-gender"))
    show_choices (p);
}

static void
on_choice_row (VibeosPressable *row, VibeosDialog *dialog)
{
  /* a row answers with its index, offset past the standard responses */
  vibeos_dialog_respond (dialog, 100 + GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "dictate-index")));
}

/* Android's single-choice list, as the window's Language dialog: a radio row for each, the current one checked */
static void
choose (Page *p, const char *title, const char *const *labels, const char *const *summaries, guint n, guint current,
        GCallback chosen)
{
  VibeosDialog *dialog = vibeos_dialog_new (title);
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  for (guint i = 0; i < n; i++)
    {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *mark = gtk_image_new_from_icon_name (i == current ? "dictate-radio-on-symbolic"
                                                                   : "dictate-radio-off-symbolic");
      GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *text = gtk_label_new (labels[i]);
      GtkWidget *row;

      gtk_image_set_pixel_size (GTK_IMAGE (mark), 24);
      gtk_widget_add_css_class (mark, "dictate-radio");
      if (i == current)
        gtk_widget_add_css_class (mark, "dictate-radio-checked");
      gtk_box_append (GTK_BOX (box), mark);
      gtk_label_set_xalign (GTK_LABEL (text), 0);
      gtk_box_append (GTK_BOX (texts), text);
      if (summaries)
        {
          GtkWidget *under = gtk_label_new (summaries[i]);

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
      if (summaries)
        gtk_widget_add_css_class (row, "dictate-dialog-item-two");
      vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
      g_object_set_data (G_OBJECT (row), "dictate-index", GINT_TO_POINTER (i));
      g_signal_connect (row, "clicked", G_CALLBACK (on_choice_row), dialog);
      gtk_box_append (GTK_BOX (list), row);
    }
  gtk_widget_add_css_class (list, "dictate-dialog-list");
  vibeos_dialog_set_content (dialog, list);
  vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
  /* the answer goes to the window: the settings page may be gone by then */
  g_signal_connect (dialog, "response", chosen, p->window);
  vibeos_dialog_present (dialog, p->window);
}

static void
on_rate_chosen (VibeosDialog *dialog, int response, GtkWidget *window)
{
  GtkWidget *page = g_object_get_data (G_OBJECT (window), "dictate-settings-page");
  Page *p = page ? g_object_get_data (G_OBJECT (page), "dictate-settings") : NULL;

  if (p && response >= 100 && response - 100 < (int) G_N_ELEMENTS (RATES))
    g_settings_set_double (p->hooks.settings, "talk-speed", RATES[response - 100].speed);
}

static void
on_rate (VibeosListRow *row, Page *p)
{
  const char *labels[G_N_ELEMENTS (RATES)];

  for (guint i = 0; i < G_N_ELEMENTS (RATES); i++)
    labels[i] = _(RATES[i].label);
  choose (p, _("Speech rate"), labels, NULL, G_N_ELEMENTS (RATES), rate_index (p->hooks.settings),
          G_CALLBACK (on_rate_chosen));
}

static void
on_gender_chosen (VibeosDialog *dialog, int response, GtkWidget *window)
{
  GtkWidget *page = g_object_get_data (G_OBJECT (window), "dictate-settings-page");
  Page *p = page ? g_object_get_data (G_OBJECT (page), "dictate-settings") : NULL;

  /* the voice takes it from its next sentence */
  if (p && response >= 100 && response - 100 < (int) G_N_ELEMENTS (GENDERS))
    g_settings_set_string (p->hooks.settings, "talk-voice-gender", GENDERS[response - 100]);
}

static void
on_gender (VibeosListRow *row, Page *p)
{
  const char *labels[G_N_ELEMENTS (GENDERS)];

  for (guint i = 0; i < G_N_ELEMENTS (GENDERS); i++)
    labels[i] = gender_label (i);
  /* TRANSLATORS: the dialog choosing whether a man or a woman speaks for Vibe */
  choose (p, _("Voice"), labels, NULL, G_N_ELEMENTS (GENDERS), gender_index (p->hooks.settings),
          G_CALLBACK (on_gender_chosen));
}

static void
on_verbosity_chosen (VibeosDialog *dialog, int response, GtkWidget *window)
{
  GtkWidget *page = g_object_get_data (G_OBJECT (window), "dictate-settings-page");
  Page *p = page ? g_object_get_data (G_OBJECT (page), "dictate-settings") : NULL;

  if (p && response >= 100 && response - 100 < (int) G_N_ELEMENTS (VERBOSITY))
    g_settings_set_string (p->hooks.settings, "talk-verbosity", VERBOSITY[response - 100].value);
}

static void
on_verbosity (VibeosListRow *row, Page *p)
{
  const char *labels[G_N_ELEMENTS (VERBOSITY)], *summaries[G_N_ELEMENTS (VERBOSITY)];

  for (guint i = 0; i < G_N_ELEMENTS (VERBOSITY); i++)
    labels[i] = _(VERBOSITY[i].label), summaries[i] = _(VERBOSITY[i].summary);
  choose (p, _("Answers"), labels, summaries, G_N_ELEMENTS (VERBOSITY), verbosity_index (p->hooks.settings),
          G_CALLBACK (on_verbosity_chosen));
}

static void
on_quiet_chosen (VibeosDialog *dialog, int response, GtkWidget *window)
{
  GtkWidget *page = g_object_get_data (G_OBJECT (window), "dictate-settings-page");
  Page *p = page ? g_object_get_data (G_OBJECT (page), "dictate-settings") : NULL;

  if (p && response >= 100 && response - 100 < (int) G_N_ELEMENTS (QUIET))
    g_settings_set_string (p->hooks.settings, "quiet-hours", QUIET[response - 100].value);
}

static void
on_quiet (VibeosListRow *row, Page *p)
{
  const char *labels[G_N_ELEMENTS (QUIET)];

  for (guint i = 0; i < G_N_ELEMENTS (QUIET); i++)
    labels[i] = _(QUIET[i].label);
  choose (p, _("Quiet hours"), labels, NULL, G_N_ELEMENTS (QUIET), MAX (quiet_index (p->hooks.settings), 0),
          G_CALLBACK (on_quiet_chosen));
}

static void
on_back (VibeosTopBar *bar, Page *p)
{
  p->hooks.back (p->hooks.data);
}

static void
on_language (VibeosListRow *row, Page *p)
{
  p->hooks.choose_language (p->hooks.data);
}

/* the window's settings page, if it is still there: a dialog's answer comes to the window (the page may have gone) */
static Page *
page_of (GtkWidget *window)
{
  GtkWidget *page = window ? g_object_get_data (G_OBJECT (window), "dictate-settings-page") : NULL;

  return page ? g_object_get_data (G_OBJECT (page), "dictate-settings") : NULL;
}

/* a download from this page has finished */
static void
on_downloaded (const char *language, gpointer data)
{
  Page *p = page_of (data);

  if (p)
    refresh (p);
}

static void
on_remove_response (VibeosDialog *dialog, int response, GtkWidget *window)
{
  const char *language = g_object_get_data (G_OBJECT (dialog), "dictate-language");
  g_autoptr (GError) error = NULL;
  Page *p;

  if (response != VIBEOS_RESPONSE_OK)
    return;
  if (!dictate_download_remove (language, &error))
    g_warning ("settings: removing %s: %s", language, error->message);
  p = page_of (window);
  if (p)
    {
      if (p->hooks.removed)
        p->hooks.removed (language, p->hooks.data);
      refresh (p);
    }
}

static void
on_language_row (VibeosListRow *row, Page *p)
{
  const char *language = g_object_get_data (G_OBJECT (row), "dictate-language");
  const DictateLanguage *known = dictate_language_lookup (language);
  g_autofree char *note = dictate_download_note (language, p->hooks.talk);
  guint64 downloaded = dictate_download_downloaded_size (language);

  if (note || !downloaded)
    {
      /* what it lacks, downloaded if the user wants it; the language listened in stays as it is */
      dictate_download_ensure (p->window, language, p->hooks.talk, on_downloaded, p->window);
      return;
    }
  {
    g_autofree char *size = g_format_size (downloaded);
    /* TRANSLATORS: a dialog's title; %s is the language's name for itself (Deutsch) */
    g_autofree char *title = g_strdup_printf (_("Remove %s?"), known->name);
    /* TRANSLATORS: %s is a size: 90.4 MB */
    g_autofree char *message = g_strdup_printf (_("Its speech recognition and voice are removed from the phone, which "
                                                  "frees %s. You can download them again."), size);
    VibeosDialog *dialog = vibeos_dialog_new (title);

    vibeos_dialog_set_message (dialog, message);
    vibeos_dialog_add_button (dialog, _("Cancel"), VIBEOS_RESPONSE_CANCEL);
    vibeos_dialog_add_button (dialog, _("Remove"), VIBEOS_RESPONSE_OK);
    g_object_set_data_full (G_OBJECT (dialog), "dictate-language", g_strdup (language), g_free);
    g_signal_connect (dialog, "response", G_CALLBACK (on_remove_response), p->window);
    vibeos_dialog_present (dialog, p->window);
  }
}

static void
on_voice (VibeosListRow *row, Page *p)
{
  vibeos_page_stack_push (p->hooks.pages,
                          dictate_voicematch_page_new (p->window, p->hooks.pages, p->hooks.settings, p->hooks.talk,
                                                       p->hooks.language (p->hooks.data)));
}

static void
on_confirm (VibeosListRow *row, Page *p)
{
  vibeos_page_stack_push (p->hooks.pages, dictate_confirm_page_new (p->hooks.pages, p->hooks.settings));
}

static void
on_clear (VibeosListRow *row, Page *p)
{
  p->hooks.clear_conversation (p->hooks.data);
}

static void
viewer_back (gpointer data)
{
  Page *p = data;

  vibeos_page_stack_pop (p->hooks.pages);
}

static void
on_viewer (VibeosListRow *row, Page *p)
{
  vibeos_page_stack_push (p->hooks.pages, dictate_debuglog_page_new (p->window, p->hooks.pages, viewer_back, p));
}

static void
on_about (VibeosListRow *row, Page *p)
{
  p->hooks.about (p->hooks.data);
}

/* back on this page from one over it: Voice match's summary, and whether there is a conversation, may have changed */
static void
on_map (GtkWidget *page, Page *p)
{
  refresh (p);
}

static GtkWidget *
switch_row (Page *p, GtkWidget *cards, const char *title, const char *summary, const char *key)
{
  GtkWidget *row = vibeos_list_row_new (title, summary);
  GtkWidget *toggle = vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);

  g_settings_bind (p->hooks.settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  return row;
}

GtkWidget *
dictate_settings_page_new (GtkWidget *window, const DictateSettingsHooks *hooks)
{
  Page *p = g_new0 (Page, 1);
  GtkWidget *bar = vibeos_top_bar_new (_("Settings"));
  GtkWidget *cards = vibeos_card_list_new (), *more = vibeos_card_list_new (), *scroll = spongy_scroll_view_new ();
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *footer;
  guint n;
  const DictateAccess *access = dictate_access_list (&n);
  const DictateLanguage *languages;

  p->hooks = *hooks;
  g_object_ref (p->hooks.settings);
  if (p->hooks.talk)
    g_object_ref (p->hooks.talk);
  p->window = window;
  p->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  p->languages = g_ptr_array_new ();
  gtk_widget_add_css_class (p->page, "dictate-settings");
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_back), p);
  gtk_box_append (GTK_BOX (p->page), bar);

  /* how Vibe talks */
  p->language = vibeos_list_row_new (_("Language"), _("What Vibe hears and speaks"));
  g_signal_connect (p->language, "clicked", G_CALLBACK (on_language), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->language);
  switch_row (p, cards, _("Speak the replies"), _("Off, they are only written"), "talk-speak");
  p->rate = vibeos_list_row_new (_("Speech rate"), _("How fast Vibe speaks"));
  g_signal_connect (p->rate, "clicked", G_CALLBACK (on_rate), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->rate);
  /* TRANSLATORS: a row in Vibe's settings, under Speech rate; its summary says what it chooses */
  p->gender = vibeos_list_row_new (_("Voice"), _("Whether a man or a woman speaks"));
  g_signal_connect (p->gender, "clicked", G_CALLBACK (on_gender), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->gender);
  p->verbosity = vibeos_list_row_new (_("Answers"), _("How much Vibe says"));
  g_signal_connect (p->verbosity, "clicked", G_CALLBACK (on_verbosity), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->verbosity);
  switch_row (p, cards, _("Listen after a reply"), _("Once a reply has been spoken, Vibe listens for your answer"),
              "talk-listen-again");
  switch_row (p, cards, _("Sounds"), _("A short sound when Vibe listens, works, is done or fails"), "cue-sounds");
  switch_row (p, cards, _("Vibration"), _("A short vibration at the same moments"), "cue-haptics");

  /* Voice match, a page of its own */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Your voice"));
  p->voice = vibeos_list_row_new (_("Voice match"), NULL);
  {
    GdkRGBA fill = vibeos_color (VIBEOS_SECONDARY_CONTAINER), glyph = vibeos_color (VIBEOS_ON_SECONDARY_CONTAINER);

    vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (p->voice), "dictate-mic-symbolic", &fill, &glyph);
  }
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (p->voice));
  g_signal_connect (p->voice, "clicked", G_CALLBACK (on_voice), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->voice);

  /* calls, and when and where it speaks */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Calls and privacy"));
  switch_row (p, cards, _("Announce calls"),
              _("Vibe says who is calling and listens for answer or decline, also when it is closed"),
              "announce-calls");
  switch_row (p, cards, _("Private mode"),
              _("Replies only in headphones; on the speaker short, with no names or messages read out"),
              "talk-private");
  p->quiet = vibeos_list_row_new (_("Quiet hours"), _("Vibe says nothing and plays no sound on its own"));
  g_signal_connect (p->quiet, "clicked", G_CALLBACK (on_quiet), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->quiet);
  switch_row (p, cards, _("Calls in quiet hours"), _("Calls are still announced"), "quiet-calls");

  /* what it can use */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("What Vibe can use"));
  {
    /* where what it reads goes, before the switches */
    GtkWidget *note = vibeos_list_row_new (_("Only to VibeOS"),
                                           _("What Vibe reads on the phone goes only to VibeOS's own model server, "
                                             "never to anyone else. For the weather, the town's rough position goes "
                                             "to Open-Meteo, as from the Weather app"));
    gtk_widget_set_can_target (note, FALSE);
    vibeos_card_list_append (VIBEOS_CARD_LIST (cards), note);
  }
  p->confirm = vibeos_list_row_new (_("Confirmations"), NULL);
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (p->confirm));
  g_signal_connect (p->confirm, "clicked", G_CALLBACK (on_confirm), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->confirm);
  /* a switch only for what Vibe has tools for (the camera has none yet) */
  for (guint i = 0; i < n; i++)
    if (dictate_tools_area_has_tools (access[i].key))
      switch_row (p, cards, _(access[i].title), NULL, access[i].key);

  /* the conversation kept on the screen, which is what Vibe remembers */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Conversation"));
  p->clear = vibeos_list_row_new (_("Clear conversation"), _("Deletes it from the phone, and Vibe forgets it"));
  g_signal_connect (p->clear, "clicked", G_CALLBACK (on_clear), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->clear);

  /* the languages, and what of each is on the phone */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Languages"));
  languages = dictate_languages (&n);
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *row = vibeos_list_row_new (languages[i].name, NULL);

      g_object_set_data (G_OBJECT (row), "dictate-language", (gpointer) languages[i].code);
      g_signal_connect (row, "clicked", G_CALLBACK (on_language_row), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
      g_ptr_array_add (p->languages, row);
    }

  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 8, 0);
  gtk_box_append (GTK_BOX (column), cards);
  footer = gtk_label_new (_("Tap a downloaded language to remove it."));
  gtk_widget_add_css_class (footer, "dictate-settings-footer");
  gtk_label_set_xalign (GTK_LABEL (footer), 0);
  gtk_label_set_wrap (GTK_LABEL (footer), TRUE);
  gtk_box_append (GTK_BOX (column), footer);

  /* finding what went wrong, and About */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (more), _("Help"));
  switch_row (p, more, _("Debug log"), _("Each turn is recorded on the phone, and only there, to find what went wrong"),
              "debug-log");
  p->viewer = vibeos_list_row_new (_("View debug log"), NULL);
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (p->viewer));
  g_signal_connect (p->viewer, "clicked", G_CALLBACK (on_viewer), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (more), p->viewer);
  {
    GtkWidget *about = vibeos_list_row_new (_("About Vibe"), NULL);

    g_signal_connect (about, "clicked", G_CALLBACK (on_about), p);
    vibeos_card_list_append (VIBEOS_CARD_LIST (more), about);
  }
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (more), 0, 24);
  gtk_box_append (GTK_BOX (column), more);
  /* on a tablet the rows are a column in the middle, their switches near their text */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (column, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (p->page), scroll);
  g_signal_connect (p->hooks.settings, "changed", G_CALLBACK (on_setting_changed), p);
  g_object_set_data_full (G_OBJECT (p->page), "dictate-settings", p, page_free);
  g_object_set_data (G_OBJECT (window), "dictate-settings-page", p->page);
  g_signal_connect (p->page, "map", G_CALLBACK (on_map), p);
  refresh (p);
  show_choices (p);
  return p->page;
}
