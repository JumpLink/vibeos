/* vibeos-gallery: the layout widgets, a page at a time, to try them and to measure them.
 *
 *   vibeos-gallery [PAGE] [--fullscreen] [--watch] [--cycle] [--dialog] [--print-text]
 *
 *   settings      (the default) a settings app: the top-level list, and each item's page beside it on a tablet or
 *                 over it on a phone
 *   nav           the bottom navigation bar, with items under their icons and beside them
 *   rail          the navigation rail beside a page
 *   fab           the floating action buttons, in their column or their row
 *   top-bar       the large top bar collapsing over a list, with selection mode on a long press
 *   text          text in sp, which follows Font size, and in dp, which does not, and the weights Bold text adds to
 *   about         the About sheet every app shows, rising over a page once the window shows; a row on the page opens
 *                 it again
 *
 *   --fullscreen  fill the monitor, as the phone's shell shows every window
 *   --watch       print what the window's VibeosLayout says each time it changes, and each property it notifies
 *   --cycle       open and close pages by themselves, every 1.5 s, to time the transitions (with
 *                 G_MESSAGES_DEBUG=vibeos-page-stack): on the settings page an item's page, then a page under it; on
 *                 the about page the sheet's Legal page, back, its Credits page, back, the sheet closed, open again
 *   --dialog      on the settings page, open the dialog "Data Saver" opens, once the window shows (on a tablet it is
 *                 centred on the page beside the list)
 *   --print-text  print every label's font size (px) and weight once the window shows, and again each time Font size
 *                 or Bold text changes
 *
 * The gallery follows Font size and Bold text (org.gnome.desktop.interface text-scaling-factor, vibeos.Interface
 * bold-text, or VIBEOS_FONT_SCALE and VIBEOS_BOLD_TEXT in the environment), live.
 *
 * Run it on a display of its own (tools/vibeos-headless), with VIBEOS_DUMP and VIBEOS_SHOT to see what it lays out. */
#include <vibeos.h>

#include <math.h>
#include <string.h>

static const char *page_name = "settings";
static gboolean fullscreen, watch, cycle, dialog, print_text;

/* ---- the settings app ------------------------------------------------------------------------------------------ */

typedef struct {
  const char *icon, *title, *summary;
  guint32     fill;            /* the icon circle's colour; 0 starts a new group */
} Item;

static const Item top_items[] = {
  { "network-wireless-symbolic", "Network & internet", "Mobile, Wi‑Fi, hotspot", 0x67d4ff },
  { "bluetooth-symbolic", "Connected devices", "Bluetooth, pairing", 0x67d4ff },
  { NULL },
  { "view-app-grid-symbolic", "Apps", "Recent apps, default apps", 0xa1c9ff },
  { "preferences-system-notifications-symbolic", "Notifications", "Notification history, conversations", 0xffaee4 },
  { "audio-volume-high-symbolic", "Sound & vibration", "Volume and vibration", 0xffaee4 },
  { "weather-clear-night-symbolic", "Modes", "Do Not Disturb, Bedtime", 0xffaee4 },
  { "display-brightness-symbolic", "Display", "Dark theme, font size, brightness", 0xffb683 },
  { "applications-graphics-symbolic", "Wallpaper & style", "Home, lock screen", 0xffb683 },
  { NULL },
  { "drive-harddisk-symbolic", "Storage", "34% used - 84.3 GB free", 0xfcbd00 },
  { "battery-good-symbolic", "Battery", "71%", 0x80da88 },
  { "emblem-system-symbolic", "System", "Languages, gestures, time, backup", 0xc7c7c7 },
  { "dialog-information-symbolic", "About device", "Test phone", 0xc7c7c7 },
  { NULL },
  { "security-high-symbolic", "Security & privacy", "App security, device lock, permissions", 0x60d5f3 },
  { "find-location-symbolic", "Location", "On - 3 apps have access to location", 0x60d5f3 },
};

typedef struct {
  GtkWidget        *window;
  VibeosListDetail *list_detail;
  GtkWidget        *rows[G_N_ELEMENTS (top_items)];
  GtkWidget        *selected;
  int               cycle_step;
} Settings;

static GdkRGBA
rgb (guint32 value)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.f, ((value >> 8) & 0xff) / 255.f, (value & 0xff) / 255.f, 1 };
}

static GtkWidget *
scrolled (GtkWidget *child)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), child);
  gtk_widget_set_vexpand (scroll, TRUE);
  return scroll;
}

/* a page with a top bar over a scrolled list */
static GtkWidget *
page_with_bar (VibeosTopBar **bar_out, GtkWidget *content, const char *title, gboolean back)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_top_bar_new (title);

  vibeos_top_bar_set_back (VIBEOS_TOP_BAR (bar), back);
  vibeos_top_bar_set_compact (VIBEOS_TOP_BAR (bar), TRUE);
  gtk_box_append (GTK_BOX (page), bar);
  gtk_box_append (GTK_BOX (page), scrolled (content));
  if (bar_out)
    *bar_out = VIBEOS_TOP_BAR (bar);
  return page;
}

static void on_sub_row_clicked (VibeosListRow *row, Settings *settings);

/* a dialog over the page it belongs to: beside the list on a tablet */
static void
open_dialog (Settings *settings)
{
  VibeosDialog *d = vibeos_dialog_new ("Data Saver");

  vibeos_dialog_set_message (d, "Data Saver stops some apps from sending or receiving data in the background.");
  vibeos_dialog_add_button (d, "Cancel", VIBEOS_RESPONSE_CANCEL);
  vibeos_dialog_add_button (d, "Turn on", VIBEOS_RESPONSE_OK);
  if (vibeos_list_detail_is_split (settings->list_detail))
    vibeos_dialog_set_anchor (d, vibeos_list_detail_get_detail_pane (settings->list_detail));
  vibeos_dialog_present (d, settings->window);
}

static void
on_dialog_row_clicked (VibeosListRow *row, Settings *settings)
{
  open_dialog (settings);
}

static void
on_back (VibeosTopBar *bar, Settings *settings)
{
  if (!vibeos_list_detail_back (settings->list_detail))
    gtk_window_close (GTK_WINDOW (settings->window));
}

/* an item's page: groups with headers, rows of each kind */
static GtkWidget *
sub_page (Settings *settings, const char *title, gboolean first_level)
{
  GtkWidget *list = vibeos_card_list_new ();
  VibeosCardList *cards = VIBEOS_CARD_LIST (list);
  VibeosTopBar *bar;
  GtkWidget *page, *row;

  vibeos_card_list_set_margins (cards, 0, 16);

  row = vibeos_list_row_new ("Internet", "Connected");
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "network-wireless-symbolic");
  g_signal_connect (row, "clicked", G_CALLBACK (on_sub_row_clicked), settings);
  vibeos_card_list_append (cards, row);
  row = vibeos_list_row_new ("SIMs", NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "network-cellular-symbolic");
  g_signal_connect (row, "clicked", G_CALLBACK (on_sub_row_clicked), settings);
  vibeos_card_list_append (cards, row);
  row = vibeos_list_row_new ("Airplane mode", NULL);
  vibeos_list_row_set_icon (VIBEOS_LIST_ROW (row), "airplane-mode-symbolic");
  vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_card_list_append (cards, row);

  vibeos_card_list_add_group (cards, "General");
  row = vibeos_list_row_new ("Notification organizer", "Sorts notifications into groups as they come in");
  vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), TRUE);
  g_signal_connect (row, "clicked", G_CALLBACK (on_sub_row_clicked), settings);
  vibeos_card_list_append (cards, row);
  row = vibeos_list_row_new ("Data Saver", "Off");
  g_signal_connect (row, "clicked", G_CALLBACK (on_dialog_row_clicked), settings);
  vibeos_card_list_append (cards, row);
  row = vibeos_list_row_new ("Private DNS", "Automatic");
  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_signal_connect (row, "clicked", G_CALLBACK (on_sub_row_clicked), settings);
  vibeos_card_list_append (cards, row);

  vibeos_card_list_add_group (cards, NULL);
  row = vibeos_list_row_new ("Hide silent notifications in status bar",
                             "Notifications that make no sound and no vibration stay out of the status bar, and only "
                             "show in the shade");
  vibeos_list_row_set_switch (VIBEOS_LIST_ROW (row), FALSE);
  vibeos_card_list_append (cards, row);
  row = vibeos_list_row_new ("Version", NULL);
  vibeos_list_row_set_value (VIBEOS_LIST_ROW (row), "0.1");
  vibeos_card_list_append (cards, row);

  page = page_with_bar (&bar, list, title, !first_level || !vibeos_list_detail_is_split (settings->list_detail));
  g_signal_connect (bar, "back", G_CALLBACK (on_back), settings);
  return page;
}

static void
on_sub_row_clicked (VibeosListRow *row, Settings *settings)
{
  vibeos_list_detail_push (settings->list_detail, sub_page (settings, "Internet", FALSE));
}

static void
select_row (Settings *settings, GtkWidget *row)
{
  if (settings->selected)
    vibeos_list_row_set_selected (VIBEOS_LIST_ROW (settings->selected), FALSE);
  settings->selected = row;
  /* the selection shows only beside the page it opened */
  if (row && vibeos_list_detail_is_split (settings->list_detail))
    vibeos_list_row_set_selected (VIBEOS_LIST_ROW (row), TRUE);
}

static void
on_top_row_clicked (VibeosListRow *row, Settings *settings)
{
  for (guint i = 0; i < G_N_ELEMENTS (top_items); i++)
    if (settings->rows[i] == GTK_WIDGET (row))
      {
        select_row (settings, GTK_WIDGET (row));
        vibeos_list_detail_show_from (settings->list_detail, sub_page (settings, top_items[i].title, TRUE),
                                      GTK_WIDGET (row));
      }
}

static void
on_split (VibeosListDetail *list_detail, GParamSpec *pspec, Settings *settings)
{
  select_row (settings, vibeos_list_detail_has_detail (list_detail) ? settings->selected : NULL);
}

static void
on_search (VibeosSearchBar *bar, Settings *settings)
{
  GtkWidget *view = vibeos_search_view_new ("Search Settings");

  vibeos_search_view_open (VIBEOS_SEARCH_VIEW (view), settings->window);
}

/* the top level: the search pill pinned over the scrolled groups */
static GtkWidget *
top_page (Settings *settings)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *search = vibeos_search_bar_new ("Search Settings");
  GtkWidget *list = vibeos_card_list_new ();
  GdkRGBA black = { 0, 0, 0, 1 };

  vibeos_search_bar_set_geometry (VIBEOS_SEARCH_BAR (search), 72, 16, 24, 56);
  vibeos_search_bar_set_fill (VIBEOS_SEARCH_BAR (search), &black);
  gtk_widget_set_margin_top (search, 8);
  gtk_widget_set_margin_bottom (search, 8);
  g_signal_connect (search, "activated", G_CALLBACK (on_search), settings);
  gtk_box_append (GTK_BOX (page), search);

  /* the first group a little under the pill */
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 6, 16);
  for (guint i = 0; i < G_N_ELEMENTS (top_items); i++)
    {
      const Item *item = &top_items[i];
      GdkRGBA fill, glyph, black_ = { 0, 0, 0, 1 };

      if (!item->title)
        {
          vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), NULL);
          continue;
        }
      fill = rgb (item->fill);
      glyph = vibeos_rgba_mix (&fill, &black_, 0.7f);
      settings->rows[i] = vibeos_list_row_new (item->title, item->summary);
      vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (settings->rows[i]), item->icon, &fill, &glyph);
      g_signal_connect (settings->rows[i], "clicked", G_CALLBACK (on_top_row_clicked), settings);
      vibeos_card_list_append (VIBEOS_CARD_LIST (list), settings->rows[i]);
    }
  gtk_box_append (GTK_BOX (page), scrolled (list));
  gtk_widget_add_css_class (page, "gallery-settings-top");
  return page;
}

/* --cycle: an item's page, a page under it, back, back */
static gboolean
settings_cycle (gpointer data)
{
  Settings *settings = data;
  VibeosPageStack *detail = vibeos_list_detail_get_detail_stack (settings->list_detail);

  switch (settings->cycle_step++ % 4)
    {
    case 0:
      on_top_row_clicked (VIBEOS_LIST_ROW (settings->rows[0]), settings);
      break;
    case 1:
      vibeos_list_detail_push (settings->list_detail, sub_page (settings, "Internet", FALSE));
      break;
    default:
      if (vibeos_page_stack_get_depth (detail) > 0 || !vibeos_list_detail_is_split (settings->list_detail))
        vibeos_list_detail_back (settings->list_detail);
      break;
    }
  return G_SOURCE_CONTINUE;
}

static GtkWidget *
settings_page (GtkWidget *window)
{
  Settings *settings = g_new0 (Settings, 1);
  GtkWidget *list_detail = vibeos_list_detail_new ();

  settings->window = window;
  settings->list_detail = VIBEOS_LIST_DETAIL (list_detail);
  g_object_set_data_full (G_OBJECT (list_detail), "gallery-settings", settings, g_free);
  vibeos_list_detail_set_list (settings->list_detail, top_page (settings));
  vibeos_list_detail_set_back_closes (settings->list_detail, TRUE);
  g_signal_connect (list_detail, "notify::split", G_CALLBACK (on_split), settings);
  if (cycle)
    g_timeout_add (1500, settings_cycle, settings);
  return list_detail;
}

/* a settings app opens with its first item's page beside the list on a tablet */
static void
settings_mapped (GtkWidget *list_detail)
{
  Settings *settings = g_object_get_data (G_OBJECT (list_detail), "gallery-settings");

  if (!cycle && vibeos_list_detail_is_split (settings->list_detail)
      && !vibeos_list_detail_has_detail (settings->list_detail))
    {
      select_row (settings, settings->rows[0]);
      vibeos_list_detail_show (settings->list_detail, sub_page (settings, top_items[0].title, TRUE));
    }
  if (dialog)
    open_dialog (settings);
}

/* ---- the navigation bar and rail ------------------------------------------------------------------------------- */

static const char *const nav_icons[] = { "alarm-symbolic", "preferences-system-time-symbolic", "hourglass-symbolic",
                                         "timer-symbolic" };
static const char *const nav_labels[] = { "Alarm", "Clock", "Timer", "Stopwatch" };

static GtkWidget *
filler (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_widget_add_css_class (label, "vibeos-title-large");
  gtk_widget_set_vexpand (label, TRUE);
  gtk_widget_set_hexpand (label, TRUE);
  return label;
}

static GtkWidget *
nav_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = vibeos_nav_bar_new (), *horizontal = vibeos_nav_bar_new ();

  gtk_box_append (GTK_BOX (page), filler ("Navigation bar"));
  for (guint i = 0; i < G_N_ELEMENTS (nav_icons); i++)
    {
      vibeos_nav_bar_append (VIBEOS_NAV_BAR (horizontal), nav_icons[i], nav_labels[i]);
      vibeos_nav_bar_append (VIBEOS_NAV_BAR (bar), nav_icons[i], nav_labels[i]);
    }
  vibeos_nav_bar_set_horizontal_items (VIBEOS_NAV_BAR (horizontal), TRUE);
  gtk_widget_set_name (horizontal, "nav-horizontal");
  gtk_widget_set_name (bar, "nav-bar");
  gtk_box_append (GTK_BOX (page), horizontal);
  gtk_box_append (GTK_BOX (page), bar);
  return page;
}

static GtkWidget *
rail_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *rail = vibeos_nav_rail_new ();
  GtkWidget *fab = vibeos_fab_new ("list-add-symbolic", FALSE);

  for (guint i = 0; i < G_N_ELEMENTS (nav_icons); i++)
    vibeos_nav_rail_append (VIBEOS_NAV_RAIL (rail), nav_icons[i], nav_labels[i]);
  vibeos_nav_rail_set_header (VIBEOS_NAV_RAIL (rail), fab);
  gtk_widget_set_name (rail, "nav-rail");
  gtk_box_append (GTK_BOX (page), rail);
  gtk_box_append (GTK_BOX (page), filler ("Navigation rail"));
  return page;
}

/* ---- the floating action buttons ------------------------------------------------------------------------------- */

static GtkWidget *
fab_page (void)
{
  GtkWidget *column = vibeos_fab_column_new ();
  GtkWidget *fab = vibeos_fab_new ("media-playback-start-symbolic", TRUE);
  GtkWidget *before = vibeos_fab_new ("view-refresh-symbolic", FALSE);
  GtkWidget *after = vibeos_fab_new ("list-add-symbolic", FALSE);

  gtk_widget_set_name (fab, "fab");
  gtk_widget_set_name (before, "fab-before");
  gtk_widget_set_name (after, "fab-after");
  vibeos_fab_column_set_content (VIBEOS_FAB_COLUMN (column), filler ("Timer"));
  vibeos_fab_column_set_fab (VIBEOS_FAB_COLUMN (column), fab);
  vibeos_fab_column_set_buttons (VIBEOS_FAB_COLUMN (column), before, after);
  return column;
}

/* ---- the large top bar ----------------------------------------------------------------------------------------- */

static void
on_selection_closed (VibeosTopBar *bar)
{
  vibeos_top_bar_set_selection (bar, NULL);
}

static void
on_long_row (VibeosListRow *row, VibeosTopBar *bar)
{
  vibeos_top_bar_set_selection (bar, "1 selected");
}

static GtkWidget *
top_bar_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *list = vibeos_card_list_new ();
  GtkWidget *scroll = scrolled (content);
  GtkWidget *bar = vibeos_top_bar_new ("Contacts");

  vibeos_top_bar_set_large (VIBEOS_TOP_BAR (bar), TRUE);
  vibeos_top_bar_add_action (VIBEOS_TOP_BAR (bar), "system-search-symbolic");
  vibeos_top_bar_add_selection_action (VIBEOS_TOP_BAR (bar), "user-trash-symbolic");
  vibeos_top_bar_set_scroll (VIBEOS_TOP_BAR (bar),
                             gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroll)));
  g_signal_connect (bar, "selection-closed", G_CALLBACK (on_selection_closed), NULL);

  /* the large title first in what scrolls, the list under it */
  gtk_box_append (GTK_BOX (content), vibeos_top_bar_get_large_title (VIBEOS_TOP_BAR (bar)));
  gtk_box_append (GTK_BOX (content), list);
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 0, 16);
  for (int i = 0; i < 30; i++)
    {
      g_autofree char *name = g_strdup_printf ("Person %d", i + 1);
      char initials[2] = { 'A' + i % 26, 0 };
      GtkWidget *row = vibeos_list_row_new (name, NULL);

      vibeos_list_row_set_avatar (VIBEOS_LIST_ROW (row), NULL, initials, NULL);
      g_signal_connect (row, "clicked", G_CALLBACK (on_long_row), bar);
      if (i % 5 == 0)
        vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), initials);
      vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
    }
  gtk_box_append (GTK_BOX (page), bar);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}

/* ---- about ----------------------------------------------------------------------------------------------------- */

/* what the gallery adds to the sheet's Credits page, as an app adds its own */
static const VibeosAboutSection about_credits[] = {
  { "Gallery", "Every libvibeos widget, a page at a time." },
  { NULL },
};

static GtkWidget *about_sheet;   /* weak: the sheet while it shows */

static void
about_open (GtkWidget *parent)
{
  const VibeosAboutInfo info = { .name = "Gallery", .icon_name = "vibeos.Settings", .credits = about_credits };

  g_set_weak_pointer (&about_sheet, vibeos_about_present (parent, &info));
}

static void
on_about_row (VibeosListRow *row)
{
  about_open (GTK_WIDGET (row));
}

/* the row on the about page whose title is `title`, in the sheet: what a tap would open */
static GtkWidget *
find_row (GtkWidget *widget, const char *title)
{
  if (VIBEOS_IS_LIST_ROW (widget))
    for (GtkWidget *c = gtk_widget_get_first_child (widget); c; c = gtk_widget_get_next_sibling (c))
      if (GTK_IS_LABEL (c) && g_str_equal (gtk_label_get_text (GTK_LABEL (c)), title))
        return widget;
  for (GtkWidget *c = gtk_widget_get_first_child (widget); c; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *row = find_row (c, title);

      if (row)
        return row;
    }
  return NULL;
}

/* --cycle: Legal, back, Credits, back, closed, open again */
static gboolean
about_cycle (gpointer data)
{
  GtkWidget *page = data;
  static int steps;
  int step = steps++ % 6;

  switch (step)
    {
    case 0:
    case 2:
      if (about_sheet)
        g_signal_emit_by_name (find_row (about_sheet, step == 0 ? "Legal" : "Credits"), "clicked");
      break;
    case 1:
    case 3:
      if (about_sheet)
        vibeos_sheet_back (VIBEOS_SHEET (about_sheet));
      break;
    case 4:
      if (about_sheet)
        vibeos_sheet_close (VIBEOS_SHEET (about_sheet));
      break;
    default:
      about_open (page);
    }
  return G_SOURCE_CONTINUE;
}

static void
about_mapped (GtkWidget *page)
{
  about_open (page);
  if (cycle)
    g_timeout_add_full (G_PRIORITY_DEFAULT, 1500, about_cycle, g_object_ref (page), g_object_unref);
}

static GtkWidget *
about_page (void)
{
  GtkWidget *list = vibeos_card_list_new ();
  GtkWidget *row = vibeos_list_row_new ("About Gallery", "The sheet every app shows");

  vibeos_list_row_set_chevron (VIBEOS_LIST_ROW (row));
  g_signal_connect (row, "clicked", G_CALLBACK (on_about_row), NULL);
  vibeos_card_list_append (VIBEOS_CARD_LIST (list), row);
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 8, 16);
  return page_with_bar (NULL, list, "About", FALSE);
}

/* ---- text ------------------------------------------------------------------------------------------------------ */

static GtkWidget *
text_line (GtkWidget *box, const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, css_class);
  gtk_box_append (GTK_BOX (box), label);
  return label;
}

/* a label sized in C, as a widget that draws its own text would: from vibeos_sp () and vibeos_font_weight (), again
 * whenever they change */
static void
size_in_code (GtkWidget *label)
{
  PangoAttrList *attributes = pango_attr_list_new ();

  pango_attr_list_insert (attributes, pango_attr_size_new_absolute ((int) round (vibeos_sp (18) * PANGO_SCALE)));
  pango_attr_list_insert (attributes, pango_attr_weight_new (vibeos_font_weight (400)));
  gtk_label_set_attributes (GTK_LABEL (label), attributes);
  pango_attr_list_unref (attributes);
}

static GtkWidget *
text_page (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label;
  static const struct { const char *text, *css_class; } lines[] = {
    { "Follows Font size", "gallery-text-heading" },
    { "8 sp, the smallest", "gallery-sp-8" },
    { "12 sp, a label", "gallery-sp-12" },
    { "14 sp, a summary", "gallery-sp-14" },
    { "16 sp, a title", "gallery-sp-16" },
    { "22 sp, a top bar", "gallery-sp-22" },
    { "30 sp, large", "gallery-sp-30" },
    { "57 sp", "gallery-sp-57" },
    { "100", "gallery-sp-100" },
    { "Never follows it", "gallery-text-heading" },
    { "12 dp, a tab that has to fit", "gallery-dp-12" },
    { "22 dp, a clock's title", "gallery-dp-22" },
    { "Bold text adds to these", "gallery-text-heading" },
    { "Regular, 400", "gallery-weight-regular" },
    { "Medium, 500", "gallery-weight-medium" },
    { "Semibold, 600", "gallery-weight-semibold" },
  };

  gtk_widget_add_css_class (box, "gallery-text-page");
  for (guint i = 0; i < G_N_ELEMENTS (lines); i++)
    text_line (box, lines[i].text, lines[i].css_class);
  label = text_line (box, "18 sp, sized in code", "gallery-text-code");
  size_in_code (label);
  g_signal_connect_object (vibeos_text_get_default (), "changed", G_CALLBACK (size_in_code), label,
                           G_CONNECT_SWAPPED);
  return scrolled (box);
}

/* --print-text: each label shown, with the size and weight it is drawn at */
static void
print_labels (GtkWidget *widget)
{
  if (GTK_IS_LABEL (widget) && gtk_widget_get_mapped (widget))
    {
      const PangoFontDescription *font = pango_context_get_font_description (gtk_widget_get_pango_context (widget));
      double px = pango_font_description_get_size (font) / (double) PANGO_SCALE;
      int weight = pango_font_description_get_weight (font);
      PangoAttrList *attributes = gtk_label_get_attributes (GTK_LABEL (widget));
      g_auto (GStrv) classes = gtk_widget_get_css_classes (widget);
      g_autofree char *joined = g_strjoinv (" ", classes);

      /* sizes and weights set in code */
      if (attributes)
        {
          PangoAttrIterator *it = pango_attr_list_get_iterator (attributes);
          PangoAttribute *size = pango_attr_iterator_get (it, PANGO_ATTR_ABSOLUTE_SIZE);
          PangoAttribute *bold = pango_attr_iterator_get (it, PANGO_ATTR_WEIGHT);

          if (size)
            px = ((PangoAttrInt *) size)->value / (double) PANGO_SCALE;
          if (bold)
            weight = ((PangoAttrInt *) bold)->value;
          pango_attr_iterator_destroy (it);
        }
      g_print ("  %-32s %8.3f px  weight %4d  height %3d  %s\n", gtk_label_get_text (GTK_LABEL (widget)), px, weight,
               gtk_widget_get_height (widget), joined);
    }
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child; child = gtk_widget_get_next_sibling (child))
    print_labels (child);
}

/* a few frames on, when a change of the settings has been styled and laid out */
static gboolean
print_text_now (GtkWidget *window, GdkFrameClock *clock, gpointer data)
{
  int *frames = data;

  if (++*frames < 3)
    return G_SOURCE_CONTINUE;
  g_print ("font size %g, bold text %s:\n", vibeos_font_scale (), vibeos_bold_text () ? "on" : "off");
  print_labels (window);
  return G_SOURCE_REMOVE;
}

static void
print_text_soon (GtkWidget *window)
{
  gtk_widget_add_tick_callback (window, print_text_now, g_new0 (int, 1), g_free);
}

/* ---- the window ------------------------------------------------------------------------------------------------ */

static void
on_layout_changed (VibeosLayout *layout)
{
  g_autoptr(GEnumClass) devices = g_type_class_ref (VIBEOS_TYPE_DEVICE_CLASS);
  g_autoptr(GEnumClass) widths = g_type_class_ref (VIBEOS_TYPE_WIDTH_CLASS);

  g_print ("layout changed: %s, %s, %s, %d×%d, smallest width %d\n",
           g_enum_get_value (devices, vibeos_layout_get_device_class (layout))->value_nick,
           g_enum_get_value (widths, vibeos_layout_get_width_class (layout))->value_nick,
           vibeos_layout_is_landscape (layout) ? "landscape" : "portrait", vibeos_layout_get_width (layout),
           vibeos_layout_get_height (layout), vibeos_layout_get_smallest_width (layout));
}

static void
on_layout_notify (VibeosLayout *layout, GParamSpec *pspec)
{
  g_print ("layout notify: %s\n", pspec->name);
}

static void
on_activate (GtkApplication *app)
{
  GtkWidget *window = vibeos_window_new (app);
  VibeosLayout *layout = vibeos_layout_get_for_window (GTK_WINDOW (window));
  GtkWidget *content;

  if (watch)
    {
      g_signal_connect (layout, "notify", G_CALLBACK (on_layout_notify), NULL);
      g_signal_connect (layout, "changed", G_CALLBACK (on_layout_changed), NULL);
      on_layout_changed (layout);
    }
  if (g_str_equal (page_name, "nav"))
    content = nav_page ();
  else if (g_str_equal (page_name, "rail"))
    content = rail_page ();
  else if (g_str_equal (page_name, "fab"))
    content = fab_page ();
  else if (g_str_equal (page_name, "top-bar"))
    content = top_bar_page ();
  else if (g_str_equal (page_name, "text"))
    content = text_page ();
  else if (g_str_equal (page_name, "about"))
    {
      content = about_page ();
      g_signal_connect (content, "map", G_CALLBACK (about_mapped), NULL);
    }
  else
    {
      content = settings_page (window);
      g_signal_connect (content, "map", G_CALLBACK (settings_mapped), NULL);
    }
  vibeos_window_set_content (GTK_WINDOW (window), content);
  if (fullscreen)
    gtk_window_fullscreen (GTK_WINDOW (window));
  if (print_text)
    {
      g_signal_connect (window, "map", G_CALLBACK (print_text_soon), NULL);
      g_signal_connect_object (vibeos_text_get_default (), "changed", G_CALLBACK (print_text_soon), window,
                               G_CONNECT_SWAPPED);
    }
  gtk_window_present (GTK_WINDOW (window));
}

/* libvibeos with the gallery's text following Font size and Bold text, and the gallery's own stylesheet */
static void
on_startup (GtkApplication *app)
{
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_resource ("/vibeos/Gallery/gallery.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;

  for (int i = 1; i < argc; i++)
    {
      if (g_str_equal (argv[i], "--fullscreen"))
        fullscreen = TRUE;
      else if (g_str_equal (argv[i], "--watch"))
        watch = TRUE;
      else if (g_str_equal (argv[i], "--cycle"))
        cycle = TRUE;
      else if (g_str_equal (argv[i], "--dialog"))
        dialog = TRUE;
      else if (g_str_equal (argv[i], "--print-text"))
        print_text = TRUE;
      else if (argv[i][0] != '-')
        page_name = argv[i];
      else
        {
          g_printerr ("usage: vibeos-gallery [settings|nav|rail|fab|top-bar|text|about] [--fullscreen] [--watch] [--cycle] "
                      "[--dialog] [--print-text]\n");
          return 2;
        }
    }
  app = gtk_application_new ("org.vibeos.Gallery", G_APPLICATION_NON_UNIQUE);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  return g_application_run (G_APPLICATION (app), 1, argv);
}
