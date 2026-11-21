/* SettingsPage: see settings-page.h.
 *
 * Left out, because the phone cannot do what they say ("the UI must not claim what the code can't back"):
 *  - "Device flip action" and "Device shake action". They need the accelerometer's readings as they come, and
 *    iio-sensor-proxy gives only the screen's orientation. DeskClock itself hides both on a phone without an
 *    accelerometer, which is what a GNOME phone is to an app.
 *  - "Select/enable screen saver" (Screen saver's third row). It opens the system's screen saver settings, and GNOME
 *    has none: nothing starts a screen saver app when the phone docks or charges. "Preview" (screensaver.c) and
 *    the screen saver's own settings are here; the owner chose to keep only those.
 *
 * The same widget is the screen saver's settings page: settings_page_new_screensaver. */
#include "settings-page.h"
#include "cities.h"
#include "clock-bar.h"
#include "clock-dialogs.h"
#include "clock-format.h"
#include "volume-slider.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <math.h>
#include <vibeos.h>
#include <spongy-gtk.h>
#include <stdlib.h>

/* the alarm volume's steps, 1 … 7 (the slider's 0 … 6). They are only the slider's stops: "alarm-volume" is their
 * fraction of the sound server's full volume, which any phone has */
#define VOLUME_STEPS     7
#define PREVIEW_MS       2000   /* the default alarm sound, two seconds */

/* a list row: its choices and what each stores */
typedef struct
{
  const char *title, *key;
  gboolean is_int;              /* an "i" key; else a string or enum key, read and written as its string */
  GPtrArray *labels, *values;
  GtkWidget *row, *summary;
} ListRow;

struct _SettingsPage
{
  GtkWidget parent;
  GSettings *settings;
  GtkWidget *box, *bar, *scroller, *list;
  GPtrArray *lists;             /* ListRow * */
  GtkWidget *seconds, *home_auto, *home_zone, *timer_vibrate, *timer_sound, *slider;
  /* the screen saver's page */
  gboolean saver;
  GtkWidget *saver_bold, *saver_ampm, *night, *night_dnd, *night_color, *brightness, *brightness_summary;
  gboolean syncing;             /* the widgets are being set from the settings: their handlers stay quiet */
};

G_DEFINE_FINAL_TYPE (SettingsPage, settings_page, GTK_TYPE_WIDGET)

enum { CLOSED, PICK_TIMER_SOUND, SHOW_SCREENSAVER, SCREENSAVER_SETTINGS, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- the rows */

/* a category: a divider over all but the first, then its accented title, 24 down from the top either way (the
 * divider's line counts) */
static void
add_category (SettingsPage *self, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = gtk_label_new (title);
  gboolean first = gtk_widget_get_first_child (self->list) == NULL;

  if (!first)
    {
      GtkWidget *divider = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

      gtk_widget_set_size_request (divider, -1, 1);
      gtk_widget_add_css_class (divider, "settings-divider");
      gtk_box_append (GTK_BOX (box), divider);
    }
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  /* the font padding first: it sets the margins, and the category's spacing goes on top of it */
  vibeos_label_set_font_padding_sp (label, 14);
  gtk_widget_set_margin_top (label, gtk_widget_get_margin_top (label) + (first ? 24 : 23));
  gtk_widget_set_margin_bottom (label, gtk_widget_get_margin_bottom (label) + 8);
  gtk_widget_set_margin_start (label, 16);
  gtk_widget_set_margin_end (label, 16);
  gtk_widget_add_css_class (label, "settings-category");
  gtk_box_append (GTK_BOX (box), label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (label), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (self->list), box);
}

/* a row: the title over a summary (wrapping, hidden when empty), and @end after them; the whole row is the touch
 * target */
static GtkWidget *
add_row (SettingsPage *self, const char *title, GtkWidget **summary, GtkWidget *end)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *row;

  gtk_widget_set_margin_start (box, 16);
  gtk_widget_set_margin_end (box, 16);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_valign (column, GTK_ALIGN_CENTER);
  /* one more under the text than over it: the text's box should be about ⅔ px taller than libvibeos's (its bottom
   * padding rounded up to a whole pixel, which libvibeos's margins cannot do), and without it the rows came out a
   * little short */
  gtk_widget_set_margin_top (column, 16);
  gtk_widget_set_margin_bottom (column, 17);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "settings-title");
  vibeos_label_set_font_padding_sp (label, 16);
  gtk_box_append (GTK_BOX (column), label);
  if (summary)
    {
      *summary = gtk_label_new (NULL);
      gtk_label_set_xalign (GTK_LABEL (*summary), 0);
      gtk_label_set_wrap (GTK_LABEL (*summary), TRUE);
      gtk_widget_add_css_class (*summary, "settings-summary");
      vibeos_label_set_font_padding_sp (*summary, 14);
      gtk_box_append (GTK_BOX (column), *summary);
    }
  gtk_box_append (GTK_BOX (box), column);
  if (end)
    {
      /* the switch or value at the end, centred on the row */
      gtk_widget_set_margin_start (end, 16);
      gtk_widget_set_valign (end, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (box), end);
    }
  row = vibeos_pressable_new (box);
  vibeos_pressable_set_ripple (VIBEOS_PRESSABLE (row), VIBEOS_RIPPLE_BOUNDED, 0);
  gtk_widget_add_css_class (row, "settings-row");
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  gtk_box_append (GTK_BOX (self->list), row);
  return row;
}

static void
set_summary (GtkWidget *summary, const char *text)
{
  gtk_widget_set_visible (summary, text && *text);
  gtk_label_set_text (GTK_LABEL (summary), text ? text : "");
}

/* --- list rows */

static void
list_row_free (gpointer data)
{
  ListRow *list = data;

  g_ptr_array_unref (list->labels);
  g_ptr_array_unref (list->values);
  g_free (list);
}

/* the stored value as one of the row's values; "Start week on" unset is the locale's first day (refresh) */
static char *
list_value (SettingsPage *self, ListRow *list)
{
  g_autofree char *value = NULL;

  if (list->is_int)
    return g_strdup_printf ("%d", g_settings_get_int (self->settings, list->key));
  value = g_settings_get_string (self->settings, list->key);
  if (g_str_equal (list->key, "week-start") && g_str_equal (value, "locale"))
    switch (clock_week_start (self->settings))
      {
      case G_DATE_SATURDAY: return g_strdup ("saturday");
      case G_DATE_SUNDAY: return g_strdup ("sunday");
      case G_DATE_MONDAY: return g_strdup ("monday");
      default: return g_strdup ("");
      }
  return g_steal_pointer (&value);
}

static int
list_index (SettingsPage *self, ListRow *list)
{
  g_autofree char *value = list_value (self, list);

  for (guint i = 0; i < list->values->len; i++)
    if (g_str_equal (list->values->pdata[i], value))
      return i;
  return -1;
}

/* the row's summary: the chosen entry, or none */
static void
show_list (SettingsPage *self, ListRow *list)
{
  int index = list_index (self, list);

  set_summary (list->summary, index >= 0 ? list->labels->pdata[index] : NULL);
}

typedef struct
{
  SettingsPage *page;
  ListRow *list;
} Pick;

static void
on_chosen (int chosen, gpointer data)
{
  Pick *pick = data;
  ListRow *list = pick->list;
  const char *value = chosen >= 0 ? list->values->pdata[chosen] : NULL;

  if (value && list->is_int)
    g_settings_set_int (pick->page->settings, list->key, atoi (value));
  else if (value)
    g_settings_set_string (pick->page->settings, list->key, value);
  g_object_unref (pick->page);
  g_free (pick);
}

static void
on_list (VibeosPressable *row, ListRow *list)
{
  SettingsPage *self = CLOCK_SETTINGS_PAGE (gtk_widget_get_ancestor (GTK_WIDGET (row), CLOCK_TYPE_SETTINGS_PAGE));
  Pick *pick = g_new0 (Pick, 1);

  pick->page = g_object_ref (self);
  pick->list = list;
  /* the dialog's title is the row's */
  clock_list_dialog (GTK_WIDGET (self), list->title, (const char *const *) list->labels->pdata, list->labels->len,
                     list_index (self, list), on_chosen, pick);
}

static ListRow *
add_list (SettingsPage *self, const char *title, const char *key, gboolean is_int)
{
  ListRow *list = g_new0 (ListRow, 1);

  list->title = title;
  list->key = key;
  list->is_int = is_int;
  list->labels = g_ptr_array_new_with_free_func (g_free);
  list->values = g_ptr_array_new_with_free_func (g_free);
  list->row = add_row (self, title, &list->summary, NULL);
  g_signal_connect (list->row, "clicked", G_CALLBACK (on_list), list);
  g_ptr_array_add (self->lists, list);
  return list;
}

static void
add_choice (ListRow *list, char *label, char *value)
{
  g_ptr_array_add (list->labels, label);
  g_ptr_array_add (list->values, value);
}

/* the "Home time zone" choices: "(GMT+1:00) Amsterdam, Berlin", by the offset now */
static int
by_offset (gconstpointer a, gconstpointer b, gpointer data)
{
  gint64 now = GPOINTER_TO_SIZE (data);
  g_autoptr(GTimeZone) za = clock_zone_new ((*(const HomeZoneData **) a)->zone);
  g_autoptr(GTimeZone) zb = clock_zone_new ((*(const HomeZoneData **) b)->zone);
  int oa = za ? clock_zone_offset (za, now) : 0, ob = zb ? clock_zone_offset (zb, now) : 0;

  return (oa > ob) - (oa < ob);
}

static void
add_home_zones (ListRow *list)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  g_autoptr(GArray) zones = g_array_new (FALSE, FALSE, sizeof (const HomeZoneData *));

  for (guint i = 0; i < home_zones_count (); i++)
    {
      const HomeZoneData *zone = home_zones_get (i);

      g_array_append_val (zones, zone);
    }
  /* Arrays.sort: stable, so zones with one offset keep the list's order */
  g_array_sort_with_data (zones, by_offset, GSIZE_TO_POINTER (now));
  for (guint i = 0; i < zones->len; i++)
    {
      const HomeZoneData *zone = g_array_index (zones, const HomeZoneData *, i);
      g_autoptr(GTimeZone) tz = clock_zone_new (zone->zone);
      int offset = tz ? clock_zone_offset (tz, now) : 0;

      add_choice (list, g_strdup_printf ("(GMT%c%d:%02d) %s", offset < 0 ? '-' : '+', ABS (offset) / 3600,
                                         ABS (offset) / 60 % 60, zone->name),
                  g_strdup (zone->zone));
    }
}

/* --- switch rows: the row toggles the switch, the switch the setting */

static void
on_switch_row (VibeosPressable *row, VibeosSwitch *toggle)
{
  vibeos_switch_set_active (toggle, !vibeos_switch_get_active (toggle));
}

static void
on_switch (VibeosSwitch *toggle, GParamSpec *pspec, SettingsPage *self)
{
  if (!self->syncing)
    g_settings_set_boolean (self->settings, g_object_get_data (G_OBJECT (toggle), "key"),
                            vibeos_switch_get_active (toggle));
}

static GtkWidget *
add_switch (SettingsPage *self, const char *title, const char *summary, const char *key)
{
  GtkWidget *toggle = vibeos_switch_new ();
  GtkWidget *label = NULL;
  GtkWidget *row = add_row (self, title, summary ? &label : NULL, toggle);

  if (summary)
    set_summary (label, summary);
  /* the switch is not clickable itself: the row is */
  gtk_widget_set_can_target (toggle, FALSE);
  g_object_set_data (G_OBJECT (toggle), "key", (gpointer) key);
  g_object_set_data (G_OBJECT (row), "toggle", toggle);
  g_signal_connect (row, "clicked", G_CALLBACK (on_switch_row), toggle);
  g_signal_connect (toggle, "notify::active", G_CALLBACK (on_switch), self);
  return row;
}

static void
show_switch (SettingsPage *self, GtkWidget *row)
{
  VibeosSwitch *toggle = g_object_get_data (G_OBJECT (row), "toggle");

  vibeos_switch_set_active (toggle, g_settings_get_boolean (self->settings, g_object_get_data (G_OBJECT (toggle),
                                                                                             "key")));
  gtk_accessible_update_state (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_STATE_CHECKED,
                               vibeos_switch_get_active (toggle) ? GTK_ACCESSIBLE_TRISTATE_TRUE
                                                              : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

/* --- the alarm volume */

static void
on_volume (VolumeSlider *slider, int value, SettingsPage *self)
{
  /* as the slider moves: the volume at once */
  g_settings_set_double (self->settings, "alarm-volume", (value + 1) / (double) VOLUME_STEPS);
}

static void
on_volume_released (VolumeSlider *slider, SettingsPage *self)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());

  /* let go: the default alarm sound for two seconds, at the new volume, by the service */
  if (bus)
    g_dbus_connection_call (bus, "vibeos.Clock.Alarms", "/vibeos/Clock/App", "vibeos.Clock.App",
                            "PlaySound", g_variant_new ("(su)", "", PREVIEW_MS), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, NULL, NULL);
}

/* Alarm volume: the alarm icon, then the title over the slider */
static void
add_volume (SettingsPage *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *icon = gtk_image_new_from_icon_name ("clock-alarm-small-symbolic");
  GtkWidget *label = gtk_label_new (_("Alarm volume"));

  gtk_widget_set_margin_start (box, 16);
  gtk_widget_set_margin_end (box, 16);
  /* the icon at the start of its frame, not centred in it: the margin after it makes up the rest */
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_set_size_request (icon, 24, -1);
  gtk_widget_set_margin_end (icon, 20);
  gtk_widget_set_halign (icon, GTK_ALIGN_START);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "settings-volume-icon");
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_set_hexpand (column, TRUE);
  gtk_widget_set_margin_top (column, 8);
  gtk_widget_set_margin_bottom (column, 8);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start (label, 12);
  gtk_widget_add_css_class (label, "settings-title");
  vibeos_label_set_font_padding_sp (label, 16);
  gtk_box_append (GTK_BOX (column), label);
  self->slider = volume_slider_new (VOLUME_STEPS - 1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->slider), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Alarm volume"),
                                  -1);
  g_signal_connect (self->slider, "changed", G_CALLBACK (on_volume), self);
  g_signal_connect (self->slider, "released", G_CALLBACK (on_volume_released), self);
  gtk_box_append (GTK_BOX (column), self->slider);
  gtk_box_append (GTK_BOX (box), column);
  gtk_box_append (GTK_BOX (self->list), box);
}

/* --- the night mode's brightness, 0 … 100: the title and its "40%" over the slider, which writes the setting as
 * it moves */

static void
on_brightness (VolumeSlider *slider, int value, SettingsPage *self)
{
  g_settings_set_int (self->settings, "screensaver-night-mode-brightness", value);
}

static GtkWidget *
add_brightness (SettingsPage *self)
{
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *label = gtk_label_new (_("Brightness"));

  gtk_widget_set_margin_start (column, 16);
  gtk_widget_set_margin_end (column, 16);
  gtk_widget_set_margin_top (column, 16);
  gtk_widget_set_margin_bottom (column, 8);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "settings-title");
  vibeos_label_set_font_padding_sp (label, 16);
  gtk_box_append (GTK_BOX (column), label);
  self->brightness_summary = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (self->brightness_summary), 0);
  gtk_widget_add_css_class (self->brightness_summary, "settings-summary");
  vibeos_label_set_font_padding_sp (self->brightness_summary, 14);
  gtk_box_append (GTK_BOX (column), self->brightness_summary);
  /* negative margins take back the slider's own inset, so its track's ends line up with the text */
  self->brightness = volume_slider_new (100);
  gtk_widget_set_margin_start (self->brightness, -14);
  gtk_widget_set_margin_end (self->brightness, -14);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->brightness), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Brightness"),
                                  -1);
  g_signal_connect (self->brightness, "changed", G_CALLBACK (on_brightness), self);
  gtk_box_append (GTK_BOX (column), self->brightness);
  gtk_widget_add_css_class (column, "settings-row");
  gtk_box_append (GTK_BOX (self->list), column);
  return column;
}

/* --- plain rows */

static void
on_date_time_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  GDBusConnection *bus = G_DBUS_CONNECTION (source);

  if (reply || data)
    return;
  /* no phone Settings app: GNOME's, which has the same action */
  g_dbus_connection_call (bus, "org.gnome.Settings", "/org/gnome/Settings", "org.freedesktop.Application",
                          "ActivateAction", g_variant_new_parsed ("('launch-panel', [<('datetime', @av [])>], "
                                                                  "@a{sv} {})"),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_date_time_done, GINT_TO_POINTER (1));
}

/* the platform data of an activation of Settings: the token that lets its window come to the front (xdg-activation;
 * without it a Settings already running behind this app only posted "Settings is ready") */
static GVariant *
settings_platform_data (GtkWidget *from)
{
  g_autoptr (GDesktopAppInfo) info = g_desktop_app_info_new ("vibeos.Settings.desktop");
  g_autoptr (GdkAppLaunchContext) context = gdk_display_get_app_launch_context (gtk_widget_get_display (from));
  g_autofree char *token = info ? g_app_launch_context_get_startup_notify_id (G_APP_LAUNCH_CONTEXT (context),
                                                                             G_APP_INFO (info), NULL) : NULL;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (token) {
    g_variant_builder_add (&builder, "{sv}", "activation-token", g_variant_new_string (token));
    g_variant_builder_add (&builder, "{sv}", "desktop-startup-id", g_variant_new_string (token));
  }
  return g_variant_builder_end (&builder);
}

/* "Change date & time" (ACTION_DATE_SETTINGS): the phone's Settings app on its Date & Time page */
static void
on_date_time (VibeosPressable *row, SettingsPage *self)
{
  GDBusConnection *bus = g_application_get_dbus_connection (g_application_get_default ());

  if (bus)
    g_dbus_connection_call (bus, "vibeos.Settings", "/vibeos/Settings", "org.freedesktop.Application",
                            "ActivateAction",
                            g_variant_new ("(s@av@a{sv})", "launch-panel",
                                           g_variant_new_parsed ("[<('datetime', @av [])>]"),
                                           settings_platform_data (GTK_WIDGET (row))),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_date_time_done, NULL);
}

static void
on_timer_sound (VibeosPressable *row, SettingsPage *self)
{
  g_signal_emit (self, signals[PICK_TIMER_SOUND], 0);
}

static void
on_preview (VibeosPressable *row, SettingsPage *self)
{
  g_signal_emit (self, signals[SHOW_SCREENSAVER], 0);
}

static void
on_screensaver_settings (VibeosPressable *row, SettingsPage *self)
{
  g_signal_emit (self, signals[SCREENSAVER_SETTINGS], 0);
}

/* a plain row with a summary that does not change */
static GtkWidget *
add_plain (SettingsPage *self, const char *title, const char *text, GCallback clicked)
{
  GtkWidget *summary;
  GtkWidget *row = add_row (self, title, &summary, NULL);

  set_summary (summary, text);
  g_signal_connect (row, "clicked", clicked, self);
  return row;
}

/* --- everything from the settings (refresh) */

/* "Bold clock text" and "Show AM/PM indicator" only for the digital clock, the AM/PM greyed with 24-hour time, and
 * the night mode's rows greyed with it off */
static void
refresh_screensaver (SettingsPage *self)
{
  gboolean digital = g_settings_get_enum (self->settings, "screensaver-clock-style") != 0;
  gboolean night = g_settings_get_boolean (self->settings, "screensaver-night-mode");
  int brightness = g_settings_get_int (self->settings, "screensaver-night-mode-brightness");
  g_autofree char *percent = g_strdup_printf (_("%d%%"), brightness);

  show_switch (self, self->saver_bold);
  show_switch (self, self->saver_ampm);
  show_switch (self, self->night);
  show_switch (self, self->night_dnd);
  gtk_widget_set_visible (self->saver_bold, digital);
  gtk_widget_set_visible (self->saver_ampm, digital);
  gtk_widget_set_sensitive (self->saver_ampm, !vibeos_clock_uses_24h ());
  gtk_widget_set_sensitive (self->night_dnd, night);
  gtk_widget_set_sensitive (self->night_color, night);
  gtk_widget_set_sensitive (self->brightness, night);
  gtk_widget_set_sensitive (gtk_widget_get_parent (self->brightness), night);
  gtk_label_set_text (GTK_LABEL (self->brightness_summary), percent);
  volume_slider_set_value (CLOCK_VOLUME_SLIDER (self->brightness), brightness);
}

static void
refresh (SettingsPage *self)
{
  g_autofree char *timer_sound = NULL;
  double volume;

  self->syncing = TRUE;
  for (guint i = 0; i < self->lists->len; i++)
    show_list (self, self->lists->pdata[i]);
  if (self->saver)
    {
      refresh_screensaver (self);
      self->syncing = FALSE;
      return;
    }
  timer_sound = clock_timer_sound_title (self->settings);
  volume = g_settings_get_double (self->settings, "alarm-volume");
  show_switch (self, self->seconds);
  show_switch (self, self->home_auto);
  show_switch (self, self->timer_vibrate);
  /* the home time zone only counts with the automatic home clock */
  gtk_widget_set_sensitive (self->home_zone, g_settings_get_boolean (self->settings, "automatic-home-clock"));
  set_summary (g_object_get_data (G_OBJECT (self->timer_sound), "summary"), timer_sound);
  /* the alarm stream's 1 … 7 from the 0 … 1 volume, less its minimum */
  volume_slider_set_value (CLOCK_VOLUME_SLIDER (self->slider), CLAMP ((int) round (volume * VOLUME_STEPS), 1,
                                                                      VOLUME_STEPS) - 1);
  self->syncing = FALSE;
}

static void
on_settings (GSettings *settings, const char *key, SettingsPage *self)
{
  refresh (self);
}

/* the bar lifts (a shade lighter) once the list leaves its top */
static void
on_scrolled (GtkAdjustment *adjustment, SettingsPage *self)
{
  if (gtk_adjustment_get_value (adjustment) > 0)
    gtk_widget_add_css_class (self->bar, "lifted");
  else
    gtk_widget_remove_css_class (self->bar, "lifted");
}

static void
on_up (VibeosPressable *button, SettingsPage *self)
{
  g_signal_emit (self, signals[CLOSED], 0);
}

/* --- the widget */

static void
settings_page_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (settings_page_parent_class)->map (widget);
  refresh (CLOCK_SETTINGS_PAGE (widget));
}

static void
settings_page_dispose (GObject *object)
{
  SettingsPage *self = CLOCK_SETTINGS_PAGE (object);

  if (self->settings)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_pointer (&self->lists, g_ptr_array_unref);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (settings_page_parent_class)->dispose (object);
}

static void
settings_page_class_init (SettingsPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = settings_page_dispose;
  widget_class->map = settings_page_map;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "settings-page");
  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  signals[PICK_TIMER_SOUND] = g_signal_new ("pick-timer-sound", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SHOW_SCREENSAVER] = g_signal_new ("show-screensaver", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                            NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SCREENSAVER_SETTINGS] = g_signal_new ("show-screensaver-settings", G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
settings_page_init (SettingsPage *self)
{
  self->lists = g_ptr_array_new_with_free_func (list_row_free);
  self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));

  /* the list, in from the sides */
  self->list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (self->list, 12);
  gtk_widget_set_margin_end (self->list, 12);
  self->scroller = spongy_scroll_view_new ();
  gtk_widget_set_vexpand (self->scroller, TRUE);
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (self->scroller), self->list);
  g_signal_connect (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (self->scroller)), "value-changed",
                    G_CALLBACK (on_scrolled), self);
  gtk_box_append (GTK_BOX (self->box), self->scroller);
}

/* the rows, top to bottom */
static void
build (SettingsPage *self)
{
  static const char *const minutes_values[] = { "1", "5", "10", "15", "20", "25" };
  ListRow *list;
  GtkWidget *summary;

  add_category (self, _("Clock"));
  list = add_list (self, _("Style"), "clock-style", FALSE);
  add_choice (list, g_strdup (_("Analog")), g_strdup ("analog"));
  add_choice (list, g_strdup (_("Digital")), g_strdup ("digital"));
  self->seconds = add_switch (self, _("Display time with seconds"), NULL, "display-clock-seconds");
  self->home_auto = add_switch (self, _("Automatic home clock"),
                                _("While traveling in an area where the time is different, add a clock for home"),
                                "automatic-home-clock");
  list = add_list (self, _("Home time zone"), "home-time-zone", FALSE);
  add_home_zones (list);
  self->home_zone = list->row;
  g_signal_connect (add_row (self, _("Change date & time"), NULL, NULL), "clicked", G_CALLBACK (on_date_time), self);

  add_category (self, _("Screen saver"));
  add_plain (self, _("Preview"), _("Starts the screen saver immediately"), G_CALLBACK (on_preview));
  add_plain (self, _("Settings"), _("Opens Clock's screen saver settings"), G_CALLBACK (on_screensaver_settings));
  /* (no "Select/enable screen saver": see the top) */

  add_category (self, _("Alarms"));
  list = add_list (self, _("Silence after"), "auto-silence", TRUE);
  for (guint i = 0; i < G_N_ELEMENTS (minutes_values); i++)
    {
      int n = atoi (minutes_values[i]);

      add_choice (list, g_strdup_printf (ngettext ("%d minute", "%d minutes", n), n), g_strdup (minutes_values[i]));
    }
  add_choice (list, g_strdup (_("Never")), g_strdup ("-1"));
  list = add_list (self, _("Snooze length"), "snooze-duration", TRUE);
  for (int n = 1; n <= 30; n++)
    add_choice (list, g_strdup_printf (ngettext ("%d minute", "%d minutes", n), n), g_strdup_printf ("%d", n));
  add_volume (self);
  list = add_list (self, _("Gradually increase volume"), "alarm-crescendo-duration", TRUE);
  for (int s = 0; s <= 60; s += 5)
    add_choice (list, s ? g_strdup_printf (ngettext ("%d second", "%d seconds", s), s) : g_strdup (_("Off")),
                g_strdup_printf ("%d", s));
  list = add_list (self, _("Volume buttons"), "volume-buttons", FALSE);
  add_choice (list, g_strdup (_("Snooze")), g_strdup ("snooze"));
  add_choice (list, g_strdup (_("Dismiss")), g_strdup ("dismiss"));
  add_choice (list, g_strdup (_("Control volume")), g_strdup ("nothing"));
  /* (no "Device flip action" or "Device shake action": see the top) */
  list = add_list (self, _("Start week on"), "week-start", FALSE);
  add_choice (list, g_strdup (_("Saturday")), g_strdup ("saturday"));
  add_choice (list, g_strdup (_("Sunday")), g_strdup ("sunday"));
  add_choice (list, g_strdup (_("Monday")), g_strdup ("monday"));

  add_category (self, _("Timers"));
  self->timer_sound = add_row (self, _("Timer sound"), &summary, NULL);
  g_object_set_data (G_OBJECT (self->timer_sound), "summary", summary);
  g_signal_connect (self->timer_sound, "clicked", G_CALLBACK (on_timer_sound), self);
  list = add_list (self, _("Gradually increase volume"), "timer-crescendo-duration", TRUE);
  for (int s = 0; s <= 60; s += 5)
    add_choice (list, s ? g_strdup_printf (ngettext ("%d second", "%d seconds", s), s) : g_strdup (_("Off")),
                g_strdup_printf ("%d", s));
  self->timer_vibrate = add_switch (self, _("Timer vibrate"), NULL, "timer-vibrate");
  /* hidden on a device without a vibrator */
  gtk_widget_set_visible (self->timer_vibrate, clock_has_vibrator ());
}

/* the screen saver page's rows */
static void
build_screensaver (SettingsPage *self)
{
  static const char *const colors[][2] = {
    { N_("White"), "white" }, { N_("Red"), "red" }, { N_("Green"), "green" }, { N_("Blue"), "blue" },
  };
  ListRow *list;

  list = add_list (self, _("Style"), "screensaver-clock-style", FALSE);
  add_choice (list, g_strdup (_("Analog")), g_strdup ("analog"));
  add_choice (list, g_strdup (_("Digital")), g_strdup ("digital"));
  list = add_list (self, _("Color"), "screensaver-clock-color", FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (colors); i++)
    add_choice (list, g_strdup (_(colors[i][0])), g_strdup (colors[i][1]));
  self->saver_bold = add_switch (self, _("Bold clock text"), NULL, "screensaver-bold-text");
  self->saver_ampm = add_switch (self, _("Show AM/PM indicator"), NULL, "screensaver-show-ampm");

  add_category (self, _("Night mode"));
  self->night = add_switch (self, _("Enabled"), _("Very dim display (for dark rooms)"), "screensaver-night-mode");
  self->night_dnd = add_switch (self, _("Follow DND state"), _("Night mode follows Do Not Disturb state"),
                                "screensaver-night-mode-dnd");
  list = add_list (self, _("Color"), "screensaver-night-mode-color", FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (colors); i++)
    add_choice (list, g_strdup (_(colors[i][0])), g_strdup (colors[i][1]));
  self->night_color = list->row;
  add_brightness (self);
}

static GtkWidget *
make (GSettings *settings, gboolean saver)
{
  SettingsPage *self = g_object_new (CLOCK_TYPE_SETTINGS_PAGE, NULL);

  self->settings = g_object_ref (settings);
  self->saver = saver;
  /* the screen saver's page has a title of its own */
  self->bar = clock_bar_new (saver ? _("Screen saver settings") : _("Settings"));
  gtk_widget_add_css_class (self->bar, "settings");
  g_signal_connect (clock_bar_get_up (self->bar), "clicked", G_CALLBACK (on_up), self);
  gtk_box_prepend (GTK_BOX (self->box), self->bar);
  if (saver)
    build_screensaver (self);
  else
    build (self);
  g_signal_connect (settings, "changed", G_CALLBACK (on_settings), self);
  return GTK_WIDGET (self);
}

GtkWidget *
settings_page_new (GSettings *settings)
{
  return make (settings, FALSE);
}

GtkWidget *
settings_page_new_screensaver (GSettings *settings)
{
  return make (settings, TRUE);
}
