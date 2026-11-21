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

/* The entries' circles: S20's colours (0.1.2's Settings), darker, the glyph white on them (owner, 11-21: "the
 * darker colors with white on color"), not Android 16's pale tonal circles with a dark glyph */
typedef enum { TINT_BLUE, TINT_VIOLET, TINT_PURPLE, TINT_GREEN, TINT_LIME, TINT_ORANGE, TINT_PINK, TINT_GREY } Tint;

static const char *const tint_fill[] = {
  [TINT_BLUE] = "#3e7bf0", [TINT_VIOLET] = "#6f63d8", [TINT_PURPLE] = "#7a5af0", [TINT_GREEN] = "#42b05a",
  [TINT_LIME] = "#8cc63f", [TINT_ORANGE] = "#ef7d2c", [TINT_PINK] = "#e35a7a", [TINT_GREY] = "#6c6c80",
};

typedef struct {
  const char *icon;
  Tint        tint;
  const char *title, *subtitle, *keywords;
  NsActivate  page;            /* our own page */
  const char *needs_schema;    /* shown only if this settings schema is installed */
  NsCap       needs;           /* shown only while the device has this (a battery) */
} Entry;

/* The main menu, in Android's order and with its names. NULL title: the next card. Network and Internet's summary
 * and About's title follow the device (entry_texts). */
static const Entry entries[] = {
  { "network-wireless-symbolic", TINT_BLUE, "Network and Internet", "Wi-Fi, hotspot",
    "wlan wifi internet sim mobile data apn vpn hotspot tethering flight airplane proxy wired", ns_page_connections },
  { "bluetooth-symbolic", TINT_BLUE, "Connected devices", "Bluetooth, pairing",
    "pair headphones speaker watch sharing visible device name printers printing", ns_page_connected_devices },
  { NULL },
  { "view-app-grid-symbolic", TINT_BLUE, "Apps", "Default apps, app settings",
    "permissions storage default browser email", ns_page_apps },
  { "preferences-system-notifications-symbolic", TINT_ORANGE, "Notifications",
    "App notifications, do not disturb", "dnd banner pop-up lock screen emergency alerts", ns_page_notifications },
  { "audio-volume-high-symbolic", TINT_VIOLET, "Sound and vibration", "Volume, vibration, ringtone",
    "ringtone alarm sound speaker microphone vibrate mute silent haptic dial pad tones", ns_page_sound },
  { "display-brightness-symbolic", TINT_LIME, "Display", "Dark theme, font size, brightness",
    "screen timeout adaptive brightness night light intensity warm amber rotate scale always on aod display size "
    "text bold zoom lock screen", ns_page_display },
  { "preferences-desktop-wallpaper-symbolic", TINT_PINK, "Wallpaper and style", "Wallpapers, colours, home screen",
    "background accent dark theme colour color parallax launcher grid columns rows layout icon size pack shape "
    "app names labels drawer", ns_page_wallpaper },
  { NULL },
  { "battery-full-symbolic", TINT_GREEN, "Battery", "Charge, power and screen blank",
    "power dim health temperature voltage capacity", ns_page_battery, NULL, NS_CAP_BATTERY },
  { "preferences-system-symbolic", TINT_VIOLET, "System", "Languages, keyboard, time",
    "region locale language keyboard clock date time users mouse touchpad updates software upgrade",
    ns_page_general },
  { "help-about-symbolic", TINT_GREY, "About phone", "Device name, versions",
    "phone tablet model kernel version memory storage name about", ns_page_about },
  { NULL },
  { "security-high-symbolic", TINT_PURPLE, "Security and privacy", "Screen lock, permissions",
    "password pin pattern swipe unlock lock screen camera microphone history trash", ns_page_privacy },
  { "find-location-symbolic", TINT_PURPLE, "Location", "Location services, app access",
    "gps geoclue", ns_page_location, "org.gnome.system.location" },
  { "avatar-default-symbolic", TINT_BLUE, "Accounts", "Online accounts",
    "google nextcloud email", ns_page_accounts },
  { NULL },
  { "preferences-desktop-accessibility-symbolic", TINT_GREEN, "Accessibility", "Display, interaction",
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
  GdkRGBA fill, glyph = { 1, 1, 1, 1 };

  gdk_rgba_parse (&fill, tint_fill[e->tint]);
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

/* ---- search: every word somewhere in an entry's title, subtitle or keywords, then the same for the pages and
 * settings inside the entries (as Android's search finds those too) ---- */

static gboolean show_panel (const char *name);
static const char *entry_summary (const Entry *e);

/* A page or setting inside an entry: its title, words it is searched by, the panel name that opens it (show_panel:
 * the page it is on), and the entry it is under, whose title is shown below it as where it is and whose icon it has.
 * `need`: what the device must have for it to be there. */
typedef struct {
  const char *title, *keywords, *panel;
  NsActivate  entry;
  NsCap       need;
} Inner;

static const Inner inner[] = {
  { "Wi-Fi", "wlan wireless networks saved password ssid internet", "wifi", ns_page_connections },
  { "Wi-Fi hotspot", "access point tethering share mobile data", "hotspot", ns_page_connections, NS_CAP_MODEM },
  { "SIMs", "sim mobile data roaming apn carrier operator pin", "mobile", ns_page_connections, NS_CAP_MODEM },
  { "Airplane mode", "flight mode offline radios", "network", ns_page_connections },
  { "Network and VPN", "vpn wired ethernet proxy", "vpn", ns_page_connections },
  { "Bluetooth", "pair pairing headphones headset speaker watch earbuds", "bluetooth", ns_page_connected_devices },
  { "Sharing", "device name visible visibility", "sharing", ns_page_connected_devices },
  { "Printers", "print printing printer", "printers", ns_page_connected_devices },
  { "Default apps", "browser email music photos video open with", "default-apps", ns_page_apps },
  { "Do not disturb", "dnd silence quiet pop-ups", "notifications", ns_page_notifications },
  { "Volume", "media alarm ring ringtone loudness mute", "sound", ns_page_sound },
  { "Vibration", "haptic haptics vibrate intensity", "sound", ns_page_sound },
  { "Brightness", "adaptive brightness dim light", "display", ns_page_display },
  { "Screen timeout", "sleep screen off timeout", "display", ns_page_display },
  { "Dark theme", "dark mode night black", "display", ns_page_display },
  { "Display size and text", "font size bold text zoom scale bigger smaller", "display-size", ns_page_display },
  { "Night light", "warm amber blue light filter evening", "night-light", ns_page_display },
  { "Wallpapers", "background picture photo parallax", "wallpapers", ns_page_wallpaper },
  { "Home screen", "launcher grid columns rows icons drawer app names", "home", ns_page_wallpaper },
  { "Battery usage", "since last full charge history chart graph", "battery-usage", ns_page_battery, NS_CAP_BATTERY },
  { "Battery information", "health temperature capacity voltage", "battery-info", ns_page_battery, NS_CAP_BATTERY },
  { "Power and screen blank", "suspend sleep dim screen blank power saving", "screen-blank", ns_page_battery,
    NS_CAP_BATTERY },
  { "Language and region", "language region locale formats translation", "language", ns_page_general },
  { "Keyboard", "on-screen keyboard osk layouts input typing", "keyboard", ns_page_general },
  { "Auto-correction", "autocorrect spelling typos suggestions next word prediction text correction", "keyboard",
    ns_page_general },
  { "Mouse and touchpad", "pointer speed scrolling cursor", "mouse", ns_page_general },
  { "Date and time", "clock time zone timezone 24-hour automatic", "datetime", ns_page_general },
  { "Users", "user account password name avatar picture", "users", ns_page_general },
  { "Software information", "version build kernel device name", "software-information", ns_page_about },
  { "Lock screen", "screen lock password pin pattern swipe unlock", "lock-screen", ns_page_privacy },
  { "Camera and microphone", "permission access", "security", ns_page_privacy },
  { "Recent files", "history remember clean up trash", "security", ns_page_privacy },
  { "Screen reader", "orca talkback speech read aloud blind", "accessibility", ns_page_accessibility },
  { "High contrast", "contrast colours colors outlines", "accessibility", ns_page_accessibility },
  { "Reduce animations", "motion animations movement", "accessibility", ns_page_accessibility },
  { "Sticky keys", "hardware keyboard slow keys bounce keys", "accessibility", ns_page_accessibility },
};

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

static gboolean
words_in (const char *a, const char *b, const char *d, char **words)
{
  g_autofree char *joined = g_strjoin (" ", a, b, d, NULL);
  g_autofree char *text = g_utf8_strdown (joined, -1);
  for (guint w = 0; words && words[w]; w++)
    if (!strstr (text, words[w]))
      return FALSE;
  return TRUE;
}

static void
inner_clicked (GtkWidget *row, gpointer data)
{
  GtkWidget *view = gtk_widget_get_ancestor (row, VIBEOS_TYPE_SEARCH_VIEW);
  const Inner *p = data;
  if (view)
    vibeos_search_view_close (VIBEOS_SEARCH_VIEW (view));
  show_panel (p->panel);
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
    GtkWidget *r = ns_row (results, NULL, e->page == ns_page_about ? ns_about_title () : e->title, entry_summary (e));
    set_circle (r, e);
    ns_row_on_activate (r, result_clicked, (gpointer) e);
  }
  /* then the pages and settings inside the entries: under an entry the device shows, if it has what they need */
  for (guint i = 0; i < G_N_ELEMENTS (inner); i++) {
    const Inner *p = &inner[i];
    GtkWidget *under = row_for_page (p->entry);
    if (!under || !gtk_widget_get_visible (under) || (p->need && !ns_caps_has (p->need)))
      continue;
    const Entry *e = g_object_get_data (G_OBJECT (under), "ns-entry");
    const char *where = e->page == ns_page_about ? ns_about_title () : e->title;
    if (!words_in (p->title, p->keywords, where, words))
      continue;
    GtkWidget *r = ns_row (results, NULL, p->title, where);
    set_circle (r, e);
    ns_row_on_activate (r, inner_clicked, (gpointer) p);
  }
}

static void
search_open (VibeosSearchBar *bar, gpointer data)
{
  GtkWidget *view = vibeos_search_view_new ("Search settings");
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

/* the texts that follow the device: a modem adds Mobile to Network and Internet and the ringtone to Sound and
 * vibration, a tablet is About tablet */
static const char *
entry_summary (const Entry *e)
{
  if (e->page == ns_page_connections)
    return ns_caps_has (NS_CAP_MODEM) ? "Mobile, Wi-Fi, hotspot" : "Wi-Fi, VPN";
  if (e->page == ns_page_sound)
    return ns_caps_has (NS_CAP_MODEM) ? "Volume, vibration, ringtone" : "Volume, alarm sound";
  return e->subtitle;
}

static void
entry_texts (void)
{
  for (guint i = 0; menu_rows && i < menu_rows->len; i++) {
    VibeosListRow *r = menu_rows->pdata[i];
    const Entry *e = g_object_get_data (G_OBJECT (r), "ns-entry");
    vibeos_list_row_set_summary (r, entry_summary (e));
    if (e->page == ns_page_about)
      vibeos_list_row_set_title (r, ns_about_title ());
  }
}

static void
theme_changed (VibeosTheme *theme, gpointer data)
{
  for (guint i = 0; i < menu_rows->len; i++)
    set_circle (menu_rows->pdata[i], g_object_get_data (menu_rows->pdata[i], "ns-entry"));
}

static GtkWidget *
main_page (void)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (page, "ns-main");

  search_bar = vibeos_search_bar_new ("Search settings");
  vibeos_search_bar_set_geometry (VIBEOS_SEARCH_BAR (search_bar), 72, 16, 24, 56);
  /* Android 17's: a line round the page's own colour, not a filled pill */
  vibeos_search_bar_set_outlined (VIBEOS_SEARCH_BAR (search_bar), TRUE);
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

static void own_gnome_settings_name (GApplication *app);

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
  own_gnome_settings_name (app);
  g_autoptr (GTask) t = g_task_new (NULL, NULL, NULL, NULL);
  g_task_run_in_thread (t, apps_thread);
}

/* a page by name: `--page home` (the launcher's "Settings" opens the home screen's), the panel names vibeos asks
 * for (a quick settings tile's long press, the volume panel's and the notification shade's settings buttons) and
 * GNOME Settings' names, which other apps ask for. "network" is the flight mode tile's: Network and Internet has the
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
  { "battery-usage", ns_page_battery_usage, ns_page_battery, TRUE },
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
  { "screen-blank", ns_page_power, ns_page_battery, TRUE }, { "wallpapers", ns_page_wallpapers, ns_page_wallpaper, TRUE },
  { "software-information", ns_page_software, ns_page_about, TRUE }, { "security", ns_page_privacy },
  /* GNOME Settings' panels VibeOS has no page of its own for: the nearest one */
  { "color", ns_page_display }, { "wacom", ns_page_mouse, ns_page_general },
  { "thunderbolt", ns_page_connected_devices },
};

/* the page a panel name opens, over the menu (FALSE: no page has that name) */
static gboolean
show_panel (const char *name)
{
  ns_pop_all ();
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
      return TRUE;
    }
  return FALSE;
}

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
  if (!show_panel (name) && name && *name)
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

/* ---- GNOME Settings' name. VibeOS Settings replaces GNOME Settings (gnome-control-center): it owns
 * org.gnome.Settings on the session bus too and answers org.freedesktop.Application there, so whatever opens a GNOME
 * Settings panel the D-Bus way (GNOME Shell's network menu, an app's Online Accounts or Date & Time button:
 * ActivateAction ("launch-panel", [<(panel, [args])>], platform data) at /org/gnome/Settings) opens this app's page.
 * Its desktop files and gnome-control-center command are this package's too (data/). */
#define GNOME_SETTINGS_NAME "org.gnome.Settings"
#define GNOME_SETTINGS_PATH "/org/gnome/Settings"

static const char gnome_settings_xml[] =
  "<node><interface name='org.freedesktop.Application'>"
  "<method name='Activate'><arg type='a{sv}' direction='in'/></method>"
  "<method name='Open'><arg type='as' direction='in'/><arg type='a{sv}' direction='in'/></method>"
  "<method name='ActivateAction'><arg type='s' direction='in'/><arg type='av' direction='in'/>"
  "<arg type='a{sv}' direction='in'/></method>"
  "</interface></node>";

static void
gnome_settings_call (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *method,
                     GVariant *params, GDBusMethodInvocation *call, gpointer app)
{
  GApplicationClass *klass = G_APPLICATION_GET_CLASS (app);
  /* the platform data is each method's last argument; its activation token gives the window the focus */
  g_autoptr (GVariant) platform = g_variant_get_child_value (params, g_variant_n_children (params) - 1);
  g_autoptr (GVariant) value = NULL;
  const char *action = NULL;

  if (g_str_equal (method, "ActivateAction")) {
    g_autoptr (GVariant) args = g_variant_get_child_value (params, 1);
    g_variant_get_child (params, 0, "&s", &action);
    if (g_variant_n_children (args)) {
      g_autoptr (GVariant) boxed = g_variant_get_child_value (args, 0);
      value = g_variant_get_variant (boxed);
    }
    const GVariantType *type = NULL;
    gboolean enabled = FALSE;
    if (!g_action_group_query_action (G_ACTION_GROUP (app), action, &enabled, &type, NULL, NULL, NULL) || !enabled ||
        (type == NULL) != (value == NULL) || (type && !g_variant_is_of_type (value, type))) {
      g_dbus_method_invocation_return_error (call, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "VibeOS Settings has no action '%s' taking that parameter", action);
      return;
    }
  }
  klass->before_emit (G_APPLICATION (app), platform);
  if (action)
    g_action_group_activate_action (G_ACTION_GROUP (app), action, value);
  else
    g_application_activate (G_APPLICATION (app));
  klass->after_emit (G_APPLICATION (app), platform);
  g_dbus_method_invocation_return_value (call, NULL);
}

static void
own_gnome_settings_name (GApplication *app)
{
  GDBusConnection *bus = g_application_get_dbus_connection (app);
  if (!bus)
    return;
  static const GDBusInterfaceVTable vtable = { gnome_settings_call, NULL, NULL, { NULL } };
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (gnome_settings_xml, NULL);
  g_autoptr (GError) error = NULL;
  if (!g_dbus_connection_register_object (bus, GNOME_SETTINGS_PATH, node->interfaces[0], &vtable, app, NULL, &error)) {
    g_warning ("cannot answer for GNOME Settings: %s", error->message);
    return;
  }
  g_bus_own_name_on_connection (bus, GNOME_SETTINGS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
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
