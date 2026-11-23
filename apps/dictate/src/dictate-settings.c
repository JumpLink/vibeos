/* dictate-settings: see dictate-settings.h. */
#include "dictate-settings.h"
#include "dictate-access.h"
#include "dictate-download.h"
#include "dictate-download-dialog.h"
#include "dictate-models.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>
#include <vibeos.h>

typedef struct {
  DictateSettingsHooks hooks;
  GtkWidget *window;
  GtkWidget *page, *language, *rate, *verbosity;
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
          /* TRANSLATORS: a language in Agent's settings; %s is a size: 90.4 MB */
          summary = g_strdup_printf (_("Downloaded, %s"), size);
        }
      else
        summary = g_strdup (_("On the phone"));
      vibeos_list_row_set_summary (row, summary);
      vibeos_list_row_set_value (row, used ? _("In use") : NULL);
      /* a tap downloads what it lacks, or removes what was downloaded (not of the language in use) */
      vibeos_list_row_set_activatable (row, lacks || (downloaded && !used));
    }
}

void
dictate_settings_page_refresh (GtkWidget *page)
{
  Page *p = g_object_get_data (G_OBJECT (page), "dictate-settings");

  if (p)
    refresh (p);
}

/* how fast Agent speaks (talk-speed): up to three times its voice's pace, for people used to fast speech */
static const struct { double speed; const char *label; } RATES[] = {
  { 0.75, N_("Slower") }, { 1.0, N_("Normal") }, { 1.25, "1.25×" }, { 1.5, "1.5×" }, { 2.0, "2×" }, { 2.5, "2.5×" },
  { 3.0, "3×" },
};

/* how much Agent says (talk-verbosity) */
static const struct { const char *value, *label, *summary; } VERBOSITY[] = {
  { "short", N_("Short"), N_("A sentence") },
  { "normal", N_("Normal"), N_("One to three sentences") },
  { "detailed", N_("Detailed"), N_("More, when the question needs it") },
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

static void
show_choices (Page *p)
{
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->rate), _(RATES[rate_index (p->hooks.settings)].label));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->verbosity),
                             _(VERBOSITY[verbosity_index (p->hooks.settings)].label));
}

static void
on_setting_changed (GSettings *settings, const char *key, Page *p)
{
  /* the language is the window's to say (dictate_settings_page_refresh): its setting changes before the engine's */
  if (g_str_equal (key, "talk-voice"))
    refresh (p);
  else if (g_str_equal (key, "talk-speed") || g_str_equal (key, "talk-verbosity"))
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
  GtkWidget *cards = vibeos_card_list_new (), *scroll = spongy_scroll_view_new ();
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

  /* how Agent talks */
  p->language = vibeos_list_row_new (_("Language"), _("What Agent hears and speaks"));
  g_signal_connect (p->language, "clicked", G_CALLBACK (on_language), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->language);
  switch_row (p, cards, _("Speak the replies"), _("Off, they are only written"), "talk-speak");
  p->rate = vibeos_list_row_new (_("Speech rate"), _("How fast Agent speaks"));
  g_signal_connect (p->rate, "clicked", G_CALLBACK (on_rate), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->rate);
  p->verbosity = vibeos_list_row_new (_("Answers"), _("How much Agent says"));
  g_signal_connect (p->verbosity, "clicked", G_CALLBACK (on_verbosity), p);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->verbosity);
  switch_row (p, cards, _("Listen after a reply"), _("Once a reply has been spoken, Agent listens for your answer"),
              "talk-listen-again");
  switch_row (p, cards, _("Sounds"), _("A short sound when Agent listens, works, is done or fails"), "cue-sounds");
  switch_row (p, cards, _("Vibration"), _("A short vibration at the same moments"), "cue-haptics");

  /* what it can use */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("What Agent can use"));
  {
    /* where what it reads goes, before the switches */
    GtkWidget *note = vibeos_list_row_new (_("Only to VibeOS"),
                                           _("What Agent reads on the phone goes only to VibeOS's own model server, "
                                             "never to anyone else. For the weather, the town's rough position goes "
                                             "to Open-Meteo, as from the Weather app"));
    gtk_widget_set_can_target (note, FALSE);
    vibeos_card_list_append (VIBEOS_CARD_LIST (cards), note);
  }
  for (guint i = 0; i < n; i++)
    switch_row (p, cards, _(access[i].title), NULL, access[i].key);

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

  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 8, 24);
  /* on a tablet the rows are a column in the middle, their switches near their text */
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (cards, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (p->page), scroll);
  g_signal_connect (p->hooks.settings, "changed", G_CALLBACK (on_setting_changed), p);
  g_object_set_data_full (G_OBJECT (p->page), "dictate-settings", p, page_free);
  g_object_set_data (G_OBJECT (window), "dictate-settings-page", p->page);
  refresh (p);
  show_choices (p);
  return p->page;
}
