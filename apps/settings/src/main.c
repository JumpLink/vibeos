/* VibeOS Settings: the phone's and the tablet's settings app, in C, GTK 4 and libvibeos. The main menu groups
 * everything the way a phone does; each entry is one of our own pages (ns-pages.h), or an app (Software). On a
 * tablet the menu and the chosen entry's page are side by side. The services the pages follow (NetworkManager,
 * BlueZ, the sound server) are connected at start, in the background. */
#include "ns-bt.h"
#include "ns-caps.h"
#include "ns-net.h"
#include "ns-pages.h"
#include "ns-sound.h"
#include "ns-ui.h"

#include <string.h>

/* the circle colours of the entries' families */
typedef enum {
  CAT_NETWORK, CAT_APPS, CAT_NOTIFICATIONS, CAT_DISPLAY, CAT_BATTERY, CAT_SYSTEM, CAT_SECURITY, CAT_ACCESSIBILITY,
} Category;

static const VibeosColor category_fill[] = {
  [CAT_NETWORK] = VIBEOS_CATEGORY_NETWORK, [CAT_APPS] = VIBEOS_CATEGORY_APPS,
  [CAT_NOTIFICATIONS] = VIBEOS_CATEGORY_NOTIFICATIONS, [CAT_DISPLAY] = VIBEOS_CATEGORY_DISPLAY,
  [CAT_BATTERY] = VIBEOS_CATEGORY_BATTERY, [CAT_SYSTEM] = VIBEOS_CATEGORY_SYSTEM,
  [CAT_SECURITY] = VIBEOS_CATEGORY_SECURITY, [CAT_ACCESSIBILITY] = VIBEOS_CATEGORY_ACCESSIBILITY,
};
static const VibeosColor category_glyph[] = {
  [CAT_NETWORK] = VIBEOS_ON_CATEGORY_NETWORK, [CAT_APPS] = VIBEOS_ON_CATEGORY_APPS,
  [CAT_NOTIFICATIONS] = VIBEOS_ON_CATEGORY_NOTIFICATIONS, [CAT_DISPLAY] = VIBEOS_ON_CATEGORY_DISPLAY,
  [CAT_BATTERY] = VIBEOS_ON_CATEGORY_BATTERY, [CAT_SYSTEM] = VIBEOS_ON_CATEGORY_SYSTEM,
  [CAT_SECURITY] = VIBEOS_ON_CATEGORY_SECURITY, [CAT_ACCESSIBILITY] = VIBEOS_ON_CATEGORY_ACCESSIBILITY,
};

typedef struct {
  const char *icon;
  Category    category;
  const char *title, *subtitle, *keywords;
  NsActivate  page;            /* our own page */
  const char *needs_schema;    /* shown only if this settings schema is installed */
  NsCap       needs;           /* shown only while the device has this (a battery) */
} Entry;

/* The main menu, in Android's order and with its names. NULL title: the next card. Network & internet's summary
 * and About's title follow the device (entry_texts). */
static const Entry entries[] = {
  { "network-wireless-symbolic", CAT_NETWORK, "Network & internet", "Wi-Fi, hotspot",
    "wlan wifi internet sim mobile data apn vpn hotspot tethering flight airplane proxy wired", ns_page_connections },
  { "bluetooth-symbolic", CAT_NETWORK, "Connected devices", "Bluetooth, pairing",
    "pair headphones speaker watch sharing visible device name printers printing", ns_page_connected_devices },
  { NULL },
  { "view-app-grid-symbolic", CAT_APPS, "Apps", "Default apps, app settings",
    "permissions storage default browser email", ns_page_apps },
  { "preferences-system-notifications-symbolic", CAT_NOTIFICATIONS, "Notifications",
    "App notifications, do not disturb", "dnd banner pop-up lock screen emergency alerts", ns_page_notifications },
  { "audio-volume-high-symbolic", CAT_NOTIFICATIONS, "Sound & vibration", "Volume, vibration, ringtone",
    "ringtone alarm sound speaker microphone vibrate mute silent haptic dial pad tones", ns_page_sound },
  { "display-brightness-symbolic", CAT_DISPLAY, "Display", "Dark theme, font size, brightness",
    "screen timeout adaptive brightness night light intensity warm amber rotate scale always on aod display size "
    "text bold zoom lock screen", ns_page_display },
  { "preferences-desktop-wallpaper-symbolic", CAT_DISPLAY, "Wallpaper & style", "Wallpapers, colours, home screen",
    "background accent dark theme colour color parallax launcher grid columns rows layout icon size pack shape "
    "app names labels drawer", ns_page_wallpaper },
  { NULL },
  { "battery-full-symbolic", CAT_BATTERY, "Battery", "Charge, power and screen blank",
    "power dim health temperature voltage capacity", ns_page_battery, NULL, NS_CAP_BATTERY },
  { "preferences-system-symbolic", CAT_SYSTEM, "System", "Languages, keyboard, time",
    "region locale language keyboard clock date time users mouse touchpad updates software upgrade",
    ns_page_general },
  { "help-about-symbolic", CAT_SYSTEM, "About phone", "Device name, versions",
    "phone tablet model kernel version memory storage name about", ns_page_about },
  { NULL },
  { "security-high-symbolic", CAT_SECURITY, "Security & privacy", "Screen lock, permissions",
    "password pin pattern swipe unlock lock screen camera microphone history trash", ns_page_privacy },
  { "find-location-symbolic", CAT_SECURITY, "Location", "Location services, app access",
    "gps geoclue", ns_page_location, "org.gnome.system.location" },
  { "avatar-default-symbolic", CAT_SECURITY, "Accounts", "Online accounts",
    "google nextcloud email", ns_page_accounts },
  { NULL },
  { "preferences-desktop-accessibility-symbolic", CAT_ACCESSIBILITY, "Accessibility", "Display, interaction",
    "zoom contrast screen reader animations font size bold text display size", ns_page_accessibility },
};

static GtkWidget *window, *search_bar;
static VibeosListDetail *list_detail;
static GPtrArray *menu_rows;     /* the menu's rows, each with its entry ("ns-entry") */
static GtkWidget *selected;      /* the entry whose page shows beside the menu */

static gboolean
installed (const Entry *e)
{
  return !e->needs_schema || ns_settings_exists (e->needs_schema);
}

static void
set_circle (GtkWidget *row, const Entry *e)
{
  GdkRGBA fill = vibeos_color (category_fill[e->category]), glyph = vibeos_color (category_glyph[e->category]);
  vibeos_list_row_set_icon_circle (VIBEOS_LIST_ROW (row), e->icon, &fill, &glyph);
}

/* ---- the selection: the entry whose page shows beside the menu, a pill in the page's colour ---- */

static void
select_row (GtkWidget *row)
{
  if (selected)
    vibeos_list_row_set_selected (VIBEOS_LIST_ROW (selected), FALSE);
  g_set_weak_pointer (&selected, row);
  if (row && vibeos_list_detail_is_split (list_detail))
    vibeos_list_row_set_selected (VIBEOS_LIST_ROW (row), TRUE);
}

static GtkWidget *
row_for_page (NsActivate page)
{
  for (guint i = 0; menu_rows && i < menu_rows->len; i++) {
    const Entry *e = g_object_get_data (menu_rows->pdata[i], "ns-entry");
    if (e->page == page)
      return menu_rows->pdata[i];
  }
  return NULL;
}

static void
open_entry (const Entry *e, GtkWidget *row)
{
  if (ns_busy ())
    return;
  select_row (row);
  ns_open_entry (e->page, row, NULL);
}

static void
menu_row_clicked (GtkWidget *row, gpointer data)
{
  open_entry (data, row);
}

/* a tablet shows the first entry's page beside the menu, rather than an empty pane */
static gboolean
show_first (gpointer data)
{
  if (vibeos_list_detail_is_split (list_detail) && !vibeos_list_detail_has_detail (list_detail) && menu_rows->len) {
    const Entry *e = g_object_get_data (menu_rows->pdata[0], "ns-entry");
    select_row (menu_rows->pdata[0]);
    ns_open_entry (e->page, NULL, NULL);
  }
  return G_SOURCE_REMOVE;
}

static void
split_changed (GObject *o, GParamSpec *ps, gpointer data)
{
  gboolean split = vibeos_list_detail_is_split (list_detail);
  if (split)
    gtk_widget_add_css_class (window, "ns-split");
  else
    gtk_widget_remove_css_class (window, "ns-split");
  select_row (vibeos_list_detail_has_detail (list_detail) ? selected : NULL);
  if (split)
    g_idle_add (show_first, NULL);
}

static void
detail_closed (VibeosListDetail *l, gpointer data)
{
  select_row (NULL);
}

/* ---- search: every word somewhere in an entry's title, subtitle or keywords ---- */

static gboolean
matches (const Entry *e, char **words)
{
  g_autofree char *joined = g_strjoin (" ", e->title, e->subtitle, e->keywords, NULL);
  g_autofree char *text = g_utf8_strdown (joined, -1);
  for (guint w = 0; words && words[w]; w++)
    if (!strstr (text, words[w]))
      return FALSE;
  return TRUE;
}

static void
result_clicked (GtkWidget *row, gpointer data)
{
  GtkWidget *view = gtk_widget_get_ancestor (row, VIBEOS_TYPE_SEARCH_VIEW);
  const Entry *e = data;
  if (view)
    vibeos_search_view_close (VIBEOS_SEARCH_VIEW (view));
  open_entry (e, row_for_page (e->page));
}

static void
search_changed (VibeosSearchView *view, const char *text, GtkWidget *results)
{
  g_autofree char *q = g_utf8_strdown (text ? text : "", -1);
  g_auto (GStrv) words = g_str_tokenize_and_fold (q, NULL, NULL);
  vibeos_card_list_remove_all (VIBEOS_CARD_LIST (results));
  if (!words || !words[0])
    return;
  for (guint i = 0; i < menu_rows->len; i++) {
    const Entry *e = g_object_get_data (menu_rows->pdata[i], "ns-entry");
    /* an entry the device has no use for (no battery) is not found either */
    if (!gtk_widget_get_visible (menu_rows->pdata[i]) || !matches (e, words))
      continue;
    GtkWidget *r = ns_row (results, NULL, e->page == ns_page_about ? ns_about_title () : e->title, e->subtitle);
    set_circle (r, e);
    ns_row_on_activate (r, result_clicked, (gpointer) e);
  }
}

static void
search_open (VibeosSearchBar *bar, gpointer data)
{
  GtkWidget *view = vibeos_search_view_new ("Search Settings");
  GtkWidget *results = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (results), 8, 16);
  gtk_widget_add_css_class (results, "ns-search-results");
  GtkWidget *sc = ns_scroller_new (results);
  gtk_widget_set_vexpand (sc, TRUE);
  vibeos_search_view_set_content (VIBEOS_SEARCH_VIEW (view), sc);
  g_signal_connect (view, "search-changed", G_CALLBACK (search_changed), results);
  vibeos_search_view_open (VIBEOS_SEARCH_VIEW (view), window);
}

/* ---- the main menu: the search pill pinned over the scrolled cards ---- */

/* the texts that follow the device: a modem adds Mobile to Network & internet and the ringtone to Sound &
 * vibration, a tablet is About tablet */
static void
entry_texts (void)
{
  for (guint i = 0; menu_rows && i < menu_rows->len; i++) {
    VibeosListRow *r = menu_rows->pdata[i];
    const Entry *e = g_object_get_data (G_OBJECT (r), "ns-entry");
    if (e->page == ns_page_connections)
      vibeos_list_row_set_summary (r, ns_caps_has (NS_CAP_MODEM) ? "Mobile, Wi-Fi, hotspot" : "Wi-Fi, VPN");
    else if (e->page == ns_page_sound)
      vibeos_list_row_set_summary (r, ns_caps_has (NS_CAP_MODEM) ? "Volume, vibration, ringtone"
                                                                 : "Volume, alarm sound");
    else if (e->page == ns_page_about)
      vibeos_list_row_set_title (r, ns_about_title ());
  }
}

static void
theme_changed (VibeosTheme *theme, gpointer data)
{
  GdkRGBA pill = vibeos_color (VIBEOS_SURFACE_CONTAINER_LOWEST);
  vibeos_search_bar_set_fill (VIBEOS_SEARCH_BAR (search_bar), &pill);
  for (guint i = 0; i < menu_rows->len; i++)
    set_circle (menu_rows->pdata[i], g_object_get_data (menu_rows->pdata[i], "ns-entry"));
}

static GtkWidget *
main_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (page, "ns-main");

  search_bar = vibeos_search_bar_new ("Search Settings");
  vibeos_search_bar_set_geometry (VIBEOS_SEARCH_BAR (search_bar), 72, 16, 24, 56);
  gtk_widget_add_css_class (search_bar, "ns-search-pill");
  g_signal_connect (search_bar, "activated", G_CALLBACK (search_open), NULL);
  gtk_box_append (GTK_BOX (page), search_bar);

  GtkWidget *list = vibeos_card_list_new ();
  vibeos_card_list_set_margins (VIBEOS_CARD_LIST (list), 6, 16);
  menu_rows = g_ptr_array_new ();
  gboolean group = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (entries); i++) {
    const Entry *e = &entries[i];
    if (!e->title) {
      group = TRUE;
      continue;
    }
    if (!installed (e))
      continue;
    if (group && menu_rows->len)
      vibeos_card_list_add_group (VIBEOS_CARD_LIST (list), NULL);
    group = FALSE;
    GtkWidget *r = vibeos_list_row_new (e->title, e->subtitle);
    g_object_set_data (G_OBJECT (r), "ns-entry", (gpointer) e);
    set_circle (r, e);
    if (e->needs)
      ns_caps_bind_visible (r, e->needs);
    g_signal_connect (r, "clicked", G_CALLBACK (menu_row_clicked), (gpointer) e);
    vibeos_card_list_append (VIBEOS_CARD_LIST (list), r);
    g_ptr_array_add (menu_rows, r);
  }
  entry_texts ();
  ns_caps_on_changed (G_CALLBACK (entry_texts), list);
  GtkWidget *sc = ns_scroller_new (list);
  gtk_widget_set_vexpand (sc, TRUE);
  gtk_box_append (GTK_BOX (page), sc);
  return page;
}

/* ---- the app ---- */

/* back (Alt+Left from VibeOS's edge swipe) and Escape: the page before, or out of Settings from the main menu (and
 * from an entry's page beside it) */
static gboolean
go_back (GtkWidget *win, GVariant *args, gpointer data)
{
  if (!ns_back ())
    gtk_window_close (GTK_WINDOW (win));
  return TRUE;
}

static void
add_back (GtkWidget *win)
{
  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
    gtk_shortcut_new (gtk_alternative_trigger_new (gtk_shortcut_trigger_parse_string ("<Alt>Left"),
                                                   gtk_shortcut_trigger_parse_string ("Escape")),
                      gtk_callback_action_new (go_back, NULL, NULL)));
  gtk_widget_add_controller (win, keys);
}

static void
activate (GtkApplication *app, gpointer data)
{
  GtkWindow *have = gtk_application_get_active_window (app);
  if (have) {
    gtk_window_present (have);
    return;
  }
  window = vibeos_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "Settings");
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 720);
  gtk_widget_add_css_class (window, "ns-window");

  GtkWidget *ld = vibeos_list_detail_new ();
  list_detail = VIBEOS_LIST_DETAIL (ld);
  ns_ui_init (list_detail);
  vibeos_list_detail_set_list (list_detail, main_page ());
  /* Back on an entry's page beside the menu leaves Settings, as on the menu itself */
  vibeos_list_detail_set_back_closes (list_detail, TRUE);
  g_signal_connect (ld, "notify::split", G_CALLBACK (split_changed), NULL);
  g_signal_connect (ld, "detail-closed", G_CALLBACK (detail_closed), NULL);
  vibeos_window_set_content (GTK_WINDOW (window), ld);
  g_signal_connect_object (vibeos_theme_get_default (), "changed", G_CALLBACK (theme_changed), window, 0);
  /* About phone or About tablet: a display-size change can make one the other */
  g_signal_connect_swapped (vibeos_layout_get_for_window (GTK_WINDOW (window)), "changed", G_CALLBACK (entry_texts),
                            NULL);
  theme_changed (NULL, NULL);
  add_back (window);
  gtk_window_present (GTK_WINDOW (window));
  split_changed (NULL, NULL, NULL);
}

/* the app list, read once in the background so that pages listing apps find it read */
static void
apps_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  g_list_free_full (g_app_info_get_all (), g_object_unref);
  g_task_return_boolean (task, TRUE);
}

static void
startup (GApplication *app, gpointer data)
{
  /* text follows Font size and Bold text */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  /* over libvibeos's, which is at APPLICATION priority too; its sizes in sp and dp follow Font size */
  vibeos_css_load_resource ("/vibeos/Settings/style.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  /* the buses once, here: the pages then get them without waiting */
  g_bus_get (G_BUS_TYPE_SYSTEM, NULL, NULL, NULL);
  ns_net_init ();
  ns_bt_init ();
  ns_sound_init ();
  ns_caps_init ();
  g_autoptr (GTask) t = g_task_new (NULL, NULL, NULL, NULL);
  g_task_run_in_thread (t, apps_thread);
}

/* a page by name: `--page home` (the launcher's "Settings" opens the home screen's), the panel names vibeos asks
 * for (a quick settings tile's long press, the volume panel's and the notification shade's settings buttons) and
 * GNOME Settings' names, which other apps ask for. "network" is the flight mode tile's: Network & internet has the
 * switch. A page that is not a menu entry itself opens as one, its entry's row selected (`entry`), or, `deep`, over
 * its entry's page. */
static const struct {
  const char *name;
  NsActivate  page;
  NsActivate  entry;        /* the menu's entry the page is under, NULL for an entry's own page */
  gboolean    deep;         /* the entry's page opens first and this one over it */
} direct[] = {
  { "home", ns_page_home, ns_page_wallpaper }, { "multitasking", ns_page_home, ns_page_wallpaper },
  { "wallpaper", ns_page_wallpaper }, { "background", ns_page_wallpaper }, { "appearance", ns_page_wallpaper },
  { "wifi", ns_page_wifi, ns_page_connections }, { "wlan", ns_page_wifi, ns_page_connections },
  { "hotspot", ns_page_hotspot, ns_page_connections }, { "wifi-hotspot", ns_page_hotspot, ns_page_connections },
  { "wwan", ns_page_mobile, ns_page_connections }, { "mobile-broadband", ns_page_mobile, ns_page_connections },
  { "mobile", ns_page_mobile, ns_page_connections },
  { "network", ns_page_connections }, { "connections", ns_page_connections },
  { "vpn", ns_page_network, ns_page_connections }, { "wired", ns_page_network, ns_page_connections },
  { "bluetooth", ns_page_bluetooth, ns_page_connected_devices },
  { "sharing", ns_page_sharing, ns_page_connected_devices },
  { "printers", ns_page_printers, ns_page_connected_devices },
  { "sound", ns_page_sound }, { "feedback", ns_page_sound },
  { "notifications", ns_page_notifications },
  { "power", ns_page_battery }, { "battery", ns_page_battery },
  { "battery-info", ns_page_battery_info, ns_page_battery, TRUE },
  { "battery-information", ns_page_battery_info, ns_page_battery, TRUE },
  { "display", ns_page_display }, { "displays", ns_page_display }, { "compositor", ns_page_display },
  { "display-size", ns_page_display_size, ns_page_display, TRUE },
  { "font-size", ns_page_display_size, ns_page_display, TRUE },
  { "night-light", ns_page_night_light, ns_page_display, TRUE },
  { "nightlight", ns_page_night_light, ns_page_display, TRUE },
  { "lockscreen", ns_page_lock_screen, ns_page_privacy }, { "lock-screen", ns_page_lock_screen, ns_page_privacy },
  { "screen-lock", ns_page_lock_screen, ns_page_privacy },
  { "keyboard", ns_page_keyboard, ns_page_general }, { "osk", ns_page_keyboard, ns_page_general },
  { "mouse", ns_page_mouse, ns_page_general },
  /* "privacy" comes from the location tile only */
  { "privacy", ns_page_location }, { "location", ns_page_location },
  { "online-accounts", ns_page_accounts }, { "accounts", ns_page_accounts },
  { "applications", ns_page_apps }, { "apps", ns_page_apps }, { "overview", ns_page_apps },
  { "default-apps", ns_page_default_apps, ns_page_apps },
  { "region", ns_page_language, ns_page_general }, { "language", ns_page_language, ns_page_general },
  { "datetime", ns_page_datetime, ns_page_general }, { "date-time", ns_page_datetime, ns_page_general },
  { "users", ns_page_users, ns_page_general }, { "user-accounts", ns_page_users, ns_page_general },
  { "system", ns_page_general },
  { "universal-access", ns_page_accessibility }, { "accessibility", ns_page_accessibility },
  { "info-overview", ns_page_about }, { "about", ns_page_about },
};

static void
open_page (GApplication *app, const char *name, const char *arg)
{
  g_application_activate (app);
  ns_pop_all ();
  if (arg && *arg && (!g_strcmp0 (name, "applications") || !g_strcmp0 (name, "apps"))) {
    select_row (row_for_page (ns_page_apps));
    ns_open_entry (ns_page_app_row, NULL, (gpointer) arg);
    return;
  }
  for (guint i = 0; name && *name && i < G_N_ELEMENTS (direct); i++)
    if (g_str_equal (name, direct[i].name)) {
      NsActivate entry = direct[i].entry ? direct[i].entry : direct[i].page;
      GtkWidget *row = row_for_page (entry);
      select_row (row);
      if (direct[i].deep) {
        ns_open_entry (entry, row, NULL);
        direct[i].page (NULL, NULL);
      } else {
        ns_open_entry (direct[i].page, row, NULL);
      }
      return;
    }
  if (name && *name)
    g_message ("no page for the panel '%s': the main menu", name);
}

/* org.gnome.Settings' "launch-panel" (and Mobile Settings' "set-panel"): (panel, [parameters]) */
static void
launch_panel (GSimpleAction *action, GVariant *param, gpointer app)
{
  const char *name = NULL;
  g_autoptr (GVariant) args = NULL;
  g_variant_get (param, "(&s@av)", &name, &args);
  /* the first parameter, a string: GNOME Settings' "applications" takes the app's id there */
  g_autoptr (GVariant) first = g_variant_n_children (args) ? g_variant_get_child_value (args, 0) : NULL;
  g_autoptr (GVariant) inner = first ? g_variant_get_variant (first) : NULL;
  const char *arg = inner && g_variant_is_of_type (inner, G_VARIANT_TYPE_STRING) ? g_variant_get_string (inner, NULL)
                                                                                   : NULL;
  open_page (app, name, arg);
}

static int
command_line (GApplication *app, GApplicationCommandLine *cl, gpointer data)
{
  const char *name = NULL, *id = NULL;
  GVariantDict *opts = g_application_command_line_get_options_dict (cl);
  g_variant_dict_lookup (opts, "page", "&s", &name);
  g_variant_dict_lookup (opts, "app", "&s", &id);
  if (id && !name)
    name = "applications";
  if (name)
    open_page (app, name, id);
  else
    g_application_activate (app);
  return 0;
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = gtk_application_new ("vibeos.Settings", G_APPLICATION_HANDLES_COMMAND_LINE);
  const GOptionEntry options[] = {
    { "page", 0, 0, G_OPTION_ARG_STRING, NULL, "Open a page: home, wallpaper, wifi, hotspot, bluetooth, power…", "PAGE" },
    { "app", 0, 0, G_OPTION_ARG_STRING, NULL, "Open an app's page (its desktop file id)", "ID" },
    { NULL }
  };
  g_application_add_main_option_entries (G_APPLICATION (app), options);
  static const GActionEntry actions[] = {
    { .name = "launch-panel", .activate = launch_panel, .parameter_type = "(sav)" },
    { .name = "set-panel", .activate = launch_panel, .parameter_type = "(sav)" },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (command_line), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
