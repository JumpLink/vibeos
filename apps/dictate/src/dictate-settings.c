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
  GtkWidget *page, *language;
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

static void
on_setting_changed (GSettings *settings, const char *key, Page *p)
{
  /* the language is the window's to say (dictate_settings_page_refresh): its setting changes before the engine's */
  if (g_str_equal (key, "talk-voice"))
    refresh (p);
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
  switch_row (p, cards, _("Listen after a reply"), _("Once a reply has been spoken, Agent listens for your answer"),
              "talk-listen-again");

  /* what it can use */
  vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("What Agent can use"));
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
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), cards);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (p->page), scroll);
  g_signal_connect (p->hooks.settings, "changed", G_CALLBACK (on_setting_changed), p);
  g_object_set_data_full (G_OBJECT (p->page), "dictate-settings", p, page_free);
  g_object_set_data (G_OBJECT (window), "dictate-settings-page", p->page);
  refresh (p);
  return p->page;
}
