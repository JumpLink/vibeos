/* dictate-confirm-page: see dictate-confirm-page.h. */
#include "dictate-confirm-page.h"

#include <glib/gi18n.h>
#include <spongy-gtk.h>

#define KEY "outward-confirm"

typedef struct {
  VibeosPageStack *pages;
  GSettings *settings;
  GtkWidget *outward, *wait, *countdown;
} Page;

static gboolean
has_choice (GSettings *settings)
{
  g_autoptr (GSettingsSchema) schema = NULL;

  g_object_get (settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key (schema, KEY);
}

static gboolean
counts_down (GSettings *settings)
{
  g_autofree char *how = has_choice (settings) ? g_settings_get_string (settings, KEY) : NULL;

  return !g_strcmp0 (how, "countdown");
}

char *
dictate_confirm_summary (GSettings *settings)
{
  return g_strdup (counts_down (settings) ? _("Texts and calls: a countdown")
                                          : _("Texts and calls: wait for “send”"));
}

static void
page_free (gpointer data)
{
  Page *p = data;

  g_signal_handlers_disconnect_by_data (p->settings, p);
  g_object_unref (p->settings);
  g_free (p);
}

static void
refresh (Page *p)
{
  gboolean down = counts_down (p->settings);

  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->outward), down ? _("Countdown") : _("Wait"));
  if (!p->wait)
    return;
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->wait), down ? NULL : C_("how Vibe asks before it acts", "In use"));
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (p->countdown), down ? C_("how Vibe asks before it acts", "In use") : NULL);
  gtk_accessible_update_state (GTK_ACCESSIBLE (p->wait), GTK_ACCESSIBLE_STATE_CHECKED,
                               down ? GTK_ACCESSIBLE_TRISTATE_FALSE : GTK_ACCESSIBLE_TRISTATE_TRUE, -1);
  gtk_accessible_update_state (GTK_ACCESSIBLE (p->countdown), GTK_ACCESSIBLE_STATE_CHECKED,
                               down ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

static void
on_setting (GSettings *settings, const char *key, Page *p)
{
  if (!g_strcmp0 (key, KEY))
    refresh (p);
}

static void
on_wait (VibeosListRow *row, Page *p)
{
  g_settings_set_string (p->settings, KEY, "wait");
}

static void
on_countdown (VibeosListRow *row, Page *p)
{
  g_settings_set_string (p->settings, KEY, "countdown");
}

static GtkWidget *
tier (GtkWidget *cards, const char *title, const char *examples, const char *how)
{
  GtkWidget *row = vibeos_list_row_new (title, examples);

  vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), how);
  vibeos_list_row_set_activatable (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_card_list_append (VIBEOS_CARD_LIST (cards), row);
  return row;
}

static void
on_back (VibeosTopBar *bar, Page *p)
{
  vibeos_page_stack_pop (p->pages);
}

GtkWidget *
dictate_confirm_page_new (VibeosPageStack *pages, GSettings *settings)
{
  Page *p = g_new0 (Page, 1);
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), *bar = vibeos_top_bar_new (_("Confirmations"));
  GtkWidget *scroll = spongy_scroll_view_new (), *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *intro = gtk_label_new (_("How sure Vibe must be before it acts."));
  GtkWidget *cards = vibeos_card_list_new ();

  p->pages = pages;
  p->settings = g_object_ref (settings);
  gtk_widget_add_css_class (page, "dictate-settings");
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), TRUE);
  g_signal_connect (bar, "back", G_CALLBACK (on_back), p);
  gtk_box_append (GTK_BOX (page), bar);

  gtk_widget_add_css_class (box, "dictate-vm-body");
  gtk_widget_add_css_class (intro, "dictate-vm-intro");
  gtk_label_set_xalign (GTK_LABEL (intro), 0);
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_box_append (GTK_BOX (box), intro);

  /* the kinds the tools are sorted in (dictate-tools.h's tiers), each with what is done before it */
  tier (cards, _("Read"), _("Messages, battery, next appointment"), _("At once"));
  tier (cards, _("Reversible"), _("Alarms, timers, Wi-Fi, volume"), _("Do, offer undo"));
  p->outward = tier (cards, _("Outward"), _("Texts and calls"), NULL);
  if (has_choice (settings))
    {
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (cards), _("Outward actions"));
      p->wait = vibeos_list_row_new (_("Wait for “send”"), _("Nothing goes until you say or tap it"));
      g_signal_connect (p->wait, "clicked", G_CALLBACK (on_wait), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->wait);
      p->countdown = vibeos_list_row_new (_("Count down three seconds"),
                                          _("Sends unless you say or tap “cancel”"));
      g_signal_connect (p->countdown, "clicked", G_CALLBACK (on_countdown), p);
      vibeos_card_list_append (VIBEOS_CARD_LIST (cards), p->countdown);
      g_signal_connect (settings, "changed", G_CALLBACK (on_setting), p);
    }
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (cards), 0, 0);
  gtk_box_append (GTK_BOX (box), cards);
  refresh (p);

  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), vibeos_clamp_new (box, VIBEOS_CLAMP_SETTINGS));
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  g_object_set_data_full (G_OBJECT (page), "dictate-confirm", p, page_free);
  return page;
}
