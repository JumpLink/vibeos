/* nb-settings: Settings (spec/jelly.md §8; SettingsActivity, activity_settings.xml, xml/settings.xml).
 *
 * still:settings: a CollapsingToolbarLayout 179 dp tall over the list, "Settings" 36 sp 24 dp in with its baseline
 * 31 dp over the bar's foot; scrolled, the bar leaves until its 64 dp toolbar is left, where "Settings" (20 sp, 60 dp
 * in) fades in as the big title fades out (titleCollapseMode fade) and the bar takes surfaceContainer. The list is
 * 8 dp in; a row is 16 dp padded, the title 16 sp over the summary 14 sp (onSurfaceVariant, 20 sp lines), a switch
 * 16 dp after the text. No dividers. Advanced share, Web debugging and LookLock are left out (§14). */
#include "nb.h"
#include "nb-suggest.h"
#include "nb-widgets.h"

#include <math.h>
#include <neo.h>
#include <spongy-gtk.h>

#define BAR_EXPANDED  179
#define BAR_COLLAPSED 64
#define TITLE_START   24
#define TITLE_BASELINE 31

typedef struct {
  NbTab     *t;
  GtkWidget *page, *scroll, *header, *big_title, *bar, *small_title;
  GtkWidget *engine_summary, *provider_summary, *home_summary;
} Settings;

/* ---- rows ---- */

static GtkWidget *
summary_label (const char *text)
{
  GtkWidget *label = nb_label (text, 14, "nb-pref-summary", FALSE);
  PangoAttrList *attrs = pango_attr_list_copy (gtk_label_get_attributes (GTK_LABEL (label)));

  /* textAppearanceBodyMedium's 20 sp lines (the last line keeps the font's own height, as StaticLayout's) */
  pango_attr_list_insert (attrs, pango_attr_line_height_new_absolute (20 * PANGO_SCALE));
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
  gtk_widget_set_margin_top (label, 0);
  gtk_widget_set_margin_bottom (label, 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  return label;
}

/* a Preference: the title over the summary (or alone), `end` (a switch) in the widget frame 16 dp after them */
static GtkWidget *
pref_row (const char *title, const char *summary, GtkWidget *end, GtkWidget **summary_out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_add_css_class (box, "nb-pref-row");
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_valign (text, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (text), nb_label (title, 16, "nb-pref-title", FALSE));
  if (summary || summary_out) {
    GtkWidget *s = summary_label (summary ? summary : "");
    gtk_box_append (GTK_BOX (text), s);
    if (summary_out)
      *summary_out = s;
  }
  gtk_box_append (GTK_BOX (box), text);
  if (end) {
    gtk_widget_set_margin_start (end, 16);
    gtk_widget_set_valign (end, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), end);
  }
  GtkWidget *row = neo_pressable_new (box);
  gtk_widget_add_css_class (row, "nb-pref");
  return row;
}

/* a SwitchPreference for the prefs.json key `key` */

typedef struct {
  const char *key;
  gboolean    fallback;
} SwitchPref;

static void
on_switch_changed (NbSwitch *sw, GParamSpec *pspec, const SwitchPref *p)
{
  nb_store_set_bool (nb->store, p->key, nb_switch_get_active (sw));
  nb_apply_settings ();
}

static void
on_switch_row (GtkWidget *row, NbSwitch *sw)
{
  nb_switch_set_active (sw, !nb_switch_get_active (sw));
}

static GtkWidget *
switch_row (const char *title, const char *summary, const SwitchPref *p)
{
  GtkWidget *sw = nb_switch_new ();

  nb_switch_set_active (NB_SWITCH (sw), nb_store_get_bool (nb->store, p->key, p->fallback));
  g_signal_connect (sw, "notify::active", G_CALLBACK (on_switch_changed), (gpointer) p);
  gtk_accessible_update_property (GTK_ACCESSIBLE (sw), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  GtkWidget *row = pref_row (title, summary, sw, NULL);
  g_signal_connect (row, "clicked", G_CALLBACK (on_switch_row), sw);
  return row;
}

static const SwitchPref reach_mode = { "reachMode", FALSE };
static const SwitchPref fullscreen_shortcuts = { "fullscreenShortcuts", FALSE };
static const SwitchPref dynamic_popups = { "dynamicPopups", FALSE };
static const SwitchPref javascript = { "javascript", TRUE };
static const SwitchPref location = { "location", TRUE };
static const SwitchPref do_not_track = { "doNotTrack", FALSE };
static const SwitchPref cookies = { "cookies", TRUE };

/* ---- the list preferences ---- */

static int
engine_index (void)
{
  const char *url = nb_store_get_string (nb->store, "searchEngine", NB_DEFAULT_ENGINE);
  for (int i = 0; nb_engine_urls[i]; i++)
    if (g_str_equal (nb_engine_urls[i], url))
      return i;
  return -1;
}

static int
provider_index (void)
{
  const char *id = nb_store_get_string (nb->store, "suggestionProvider", "GOOGLE");
  for (int i = 0; nb_suggest_ids[i]; i++)
    if (g_str_equal (nb_suggest_ids[i], id))
      return i;
  return -1;
}

static void
on_engine_picked (int index, gpointer data)
{
  Settings *s = data;
  nb_store_set_string (nb->store, "searchEngine", nb_engine_urls[index]);
  gtk_label_set_text (GTK_LABEL (s->engine_summary), nb_engine_names[index]);
}

static void
on_engine (GtkWidget *row, Settings *s)
{
  nb_list_dialog_present (s->t->win, "Search engine", nb_engine_names, engine_index (), on_engine_picked, s);
}

static void
on_provider_picked (int index, gpointer data)
{
  Settings *s = data;
  nb_store_set_string (nb->store, "suggestionProvider", nb_suggest_ids[index]);
  gtk_label_set_text (GTK_LABEL (s->provider_summary), nb_suggest_names[index]);
}

static void
on_provider (GtkWidget *row, Settings *s)
{
  nb_list_dialog_present (s->t->win, "Suggestions provider", nb_suggest_names, provider_index (), on_provider_picked,
                          s);
}

/* ---- the home page (editHomePage) ---- */

static void
sync_home_summary (Settings *s)
{
  gtk_label_set_text (GTK_LABEL (s->home_summary), nb_home_autoload () ? nb_home_page () : "Disabled");
}

typedef struct {
  Settings  *s;
  GtkWidget *mark, *field;
} HomeEdit;

static void
on_home_check (GtkWidget *row, HomeEdit *h)
{
  gboolean on = !nb_mark_get_active (NB_MARK (h->mark));
  nb_mark_set_active (NB_MARK (h->mark), on);
  /* homepageUrlEditText.isEnabled = checked */
  gtk_widget_set_sensitive (h->field, on);
}

static void
on_home_response (NbDialog *d, int response, HomeEdit *h)
{
  if (response == NB_RESPONSE_OK) {
    const char *url = gtk_editable_get_text (GTK_EDITABLE (nb_field_get_entry (h->field)));
    nb_store_set_string (nb->store, "homePage", url && *url ? url : NB_DEFAULT_HOME);
    nb_store_set_bool (nb->store, "homePageAutoload", nb_mark_get_active (NB_MARK (h->mark)));
  } else if (response == NB_RESPONSE_NEUTRAL) {
    nb_store_set_string (nb->store, "homePage", NB_DEFAULT_HOME);
    nb_store_set_bool (nb->store, "homePageAutoload", TRUE);
  }
  sync_home_summary (h->s);
}

static void
on_home (GtkWidget *row, Settings *s)
{
  HomeEdit *h = g_new0 (HomeEdit, 1);
  NbDialog *d = nb_dialog_new ("Select home page");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  h->s = s;
  nb_dialog_set_message (d, "Write the home page URL");
  /* dialog_homepage_edit.xml: ?dialogPreferredPadding either side; the MaterialCheckBox "Enabled", then the
   * outlined URL field (hint "Home page"), enabled only while the box is checked */
  gtk_widget_add_css_class (box, "nb-home-edit");
  GtkWidget *check = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (check, "nb-check-row");
  h->mark = nb_mark_new (NB_MARK_CHECK);
  nb_mark_set_active (NB_MARK (h->mark), nb_home_autoload ());
  gtk_box_append (GTK_BOX (check), h->mark);
  GtkWidget *label = nb_label ("Enabled", 14, "nb-check-text", FALSE);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (check), label);
  GtkWidget *check_row = neo_pressable_new (check);
  gtk_accessible_update_property (GTK_ACCESSIBLE (check_row), GTK_ACCESSIBLE_PROPERTY_LABEL, "Enabled", -1);
  g_signal_connect (check_row, "clicked", G_CALLBACK (on_home_check), h);
  gtk_box_append (GTK_BOX (box), check_row);
  h->field = nb_field_new ("Home page", FALSE);
  GtkEntry *entry = nb_field_get_entry (h->field);
  gtk_editable_set_text (GTK_EDITABLE (entry), nb_home_page ());
  gtk_entry_set_input_purpose (entry, GTK_INPUT_PURPOSE_URL);
  gtk_widget_set_sensitive (h->field, nb_home_autoload ());
  gtk_box_append (GTK_BOX (box), h->field);
  nb_dialog_set_content (d, box);
  nb_dialog_add_button (d, "Reset", NB_RESPONSE_NEUTRAL);
  nb_dialog_add_button (d, "Cancel", NB_RESPONSE_CANCEL);
  nb_dialog_add_button (d, "OK", NB_RESPONSE_OK);
  g_object_set_data_full (G_OBJECT (d), "nb-data", h, (GDestroyNotify) g_free);
g_signal_connect (d, "response", G_CALLBACK (on_home_response), h);
  nb_dialog_present (d, s->t->win);
}

/* ---- clear cookies ---- */

static void
on_clear_cookies (GtkWidget *row, Settings *s)
{
  /* CookieManager.removeAllCookies, then a LENGTH_LONG toast */
  nb_clear_cookies ();
  nb_toast_show (s->t->win, "Cleared all the cookies");
}

/* ---- the collapsing bar ---- */

static void
on_up (Settings *s)
{
  nb_tab_pop_page (s->t);
}

static void
on_scrolled (GtkAdjustment *adj, Settings *s)
{
  double range = BAR_EXPANDED - BAR_COLLAPSED;
  double f = CLAMP (gtk_adjustment_get_value (adj) / range, 0, 1);

  /* CollapsingTextHelper in fade mode: the expanded title is gone by half way, the collapsed one comes in over
   * the second half */
  gtk_widget_set_opacity (s->big_title, CLAMP (1 - f * 2, 0, 1));
  gtk_widget_set_opacity (s->small_title, CLAMP (f * 2 - 1, 0, 1));
  /* the bar's contentScrim / liftOnScroll: surfaceContainer once collapsed */
  if (f >= 1)
    gtk_widget_add_css_class (s->bar, "collapsed");
  else
    gtk_widget_remove_css_class (s->bar, "collapsed");
}

static GtkWidget *
category (const char *title)
{
  GtkWidget *label = nb_label (title, 14, "nb-pref-category", TRUE);
  gtk_widget_add_css_class (label, "nb-pref-category-row");
  return label;
}

GtkWidget *
nb_settings_page_new (NbTab *t)
{
  Settings *s = g_new0 (Settings, 1);
  GtkWidget *row;

  s->t = t;
  s->page = gtk_overlay_new ();
  gtk_widget_add_css_class (s->page, "nb-page");
  gtk_widget_add_css_class (s->page, "nb-settings");
  g_object_set_data_full (G_OBJECT (s->page), "nb-settings", s, g_free);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (content, GTK_ALIGN_START);

  /* the expanded bar: scrolls away with the list down to its collapsed 64 dp */
  s->header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (s->header, "nb-settings-header");
  gtk_widget_set_size_request (s->header, -1, BAR_EXPANDED);
  s->big_title = nb_label ("Settings", 36, "nb-settings-big-title", TRUE);
  gtk_widget_set_vexpand (s->big_title, TRUE);
  gtk_widget_set_valign (s->big_title, GTK_ALIGN_END);
  gtk_widget_set_margin_start (s->big_title, TITLE_START);
  gtk_widget_set_margin_end (s->big_title, TITLE_START);
  /* the baseline 31 dp over the foot: the line's descent (Roboto 500 / 2048 em) below it */
  gtk_widget_set_margin_top (s->big_title, 0);
  gtk_widget_set_margin_bottom (s->big_title, (int) round (TITLE_BASELINE - 36 * 500 / 2048.0));
  gtk_box_append (GTK_BOX (s->header), s->big_title);
  gtk_box_append (GTK_BOX (content), s->header);

  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (list, "nb-pref-list");
  row = pref_row ("Search engine", NULL, NULL, &s->engine_summary);
  int e = engine_index ();
  gtk_label_set_text (GTK_LABEL (s->engine_summary), e >= 0 ? nb_engine_names[e] : "");
  g_signal_connect (row, "clicked", G_CALLBACK (on_engine), s);
  gtk_box_append (GTK_BOX (list), row);
  row = pref_row ("Suggestions provider", NULL, NULL, &s->provider_summary);
  int p = provider_index ();
  gtk_label_set_text (GTK_LABEL (s->provider_summary), p >= 0 ? nb_suggest_names[p] : "");
  g_signal_connect (row, "clicked", G_CALLBACK (on_provider), s);
  gtk_box_append (GTK_BOX (list), row);
  row = pref_row ("Home page", NULL, NULL, &s->home_summary);
  sync_home_summary (s);
  g_signal_connect (row, "clicked", G_CALLBACK (on_home), s);
  gtk_box_append (GTK_BOX (list), row);
  gtk_box_append (GTK_BOX (list), switch_row ("Reach mode", "Relocate interface elements for easier one-hand usage",
                                              &reach_mode));
  gtk_box_append (GTK_BOX (list), switch_row ("Fullscreen shortcuts", "Force shortcuts to open in fullscreen mode",
                                              &fullscreen_shortcuts));
  gtk_box_append (GTK_BOX (list), category ("Privacy & security"));
  gtk_box_append (GTK_BOX (list), switch_row ("Dynamic popups",
                                              "Allow JavaScript to open popups without user interaction",
                                              &dynamic_popups));
  gtk_box_append (GTK_BOX (list), switch_row ("JavaScript", "Enable JavaScript", &javascript));
  gtk_box_append (GTK_BOX (list), switch_row ("Location", "Allow websites to request access to your location",
                                              &location));
  gtk_box_append (GTK_BOX (list), switch_row ("Do Not Track", "Send a Do Not Track request to websites you visit",
                                              &do_not_track));
  gtk_box_append (GTK_BOX (list), switch_row ("Cookies", "Allow websites to save and load cookies", &cookies));
  row = pref_row ("Clear cookies", NULL, NULL, NULL);
  g_signal_connect (row, "clicked", G_CALLBACK (on_clear_cookies), s);
  gtk_box_append (GTK_BOX (list), row);
  gtk_box_append (GTK_BOX (content), list);

  s->scroll = spongy_scroll_view_new ();
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (s->scroll), content);
  gtk_overlay_set_child (GTK_OVERLAY (s->page), s->scroll);
  GtkAdjustment *adj = spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (s->scroll));
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_scrolled), s);

  /* the pinned toolbar: the up arrow, and the collapsed title */
  s->bar = nb_toolbar_new ("Settings", 20, G_CALLBACK (on_up), s, NULL);
  gtk_widget_add_css_class (s->bar, "nb-settings-bar");
  gtk_widget_set_valign (s->bar, GTK_ALIGN_START);
  s->small_title = nb_toolbar_get_title (s->bar);
  gtk_widget_set_opacity (s->small_title, 0);
  gtk_overlay_add_overlay (GTK_OVERLAY (s->page), s->bar);
  return s->page;
}
