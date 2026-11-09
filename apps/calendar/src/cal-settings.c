/* Settings and About (cal-settings.h). */
#include "cal-settings.h"
#include "cal-colors.h"
#include "cal-compat-switch.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#ifndef CAL_VERSION
#define CAL_VERSION "0"
#endif

/* --- rows (androidx.preference's preference_material.xml as Material 3 lays it out: a 24 dp icon 16 dp in, the
 * title 16 sp from 72 dp, the summary 14 sp under it, 16 dp above and below) */

static GtkWidget *
row_icon (const char *icon_name, guint32 color)
{
  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);
  gtk_widget_add_css_class (icon, "cal-pref-icon");
  if (color)
    {
      /* a calendar's circle: its own colour (setColorFilter SRC_IN) */
      g_autofree char *css = g_strdup_printf ("#%06x", color & 0xffffff);
      GtkCssProvider *provider = gtk_css_provider_new ();
      g_autofree char *rule = g_strdup_printf ("image { color: %s; }", css);

      gtk_css_provider_load_from_string (provider, rule);
      /* a per-widget colour has no non-deprecated form short of drawing the icon itself */
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_style_context_add_provider (gtk_widget_get_style_context (icon), GTK_STYLE_PROVIDER (provider),
                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
      G_GNUC_END_IGNORE_DEPRECATIONS
      g_object_unref (provider);
    }
  return icon;
}

typedef struct {
  GtkWidget *row, *title, *summary, *end;
} Pref;

static Pref
pref_row (const char *icon_name, guint32 color, const char *title, const char *summary, GtkWidget *end)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *texts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  Pref pref = { 0 };

  /* the icon frame keeps its space without an icon (isIconSpaceReserved) */
  gtk_widget_add_css_class (frame, "cal-pref-icon-frame");
  if (icon_name)
    gtk_box_append (GTK_BOX (frame), row_icon (icon_name, color));
  gtk_box_append (GTK_BOX (box), frame);
  pref.title = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (pref.title), 0);
  gtk_label_set_wrap (GTK_LABEL (pref.title), TRUE);
  gtk_widget_add_css_class (pref.title, "cal-pref-title");
  neo_label_set_font_padding (pref.title, 16);
  gtk_box_append (GTK_BOX (texts), pref.title);
  pref.summary = gtk_label_new (summary);
  gtk_label_set_xalign (GTK_LABEL (pref.summary), 0);
  gtk_label_set_wrap (GTK_LABEL (pref.summary), TRUE);
  gtk_widget_add_css_class (pref.summary, "cal-pref-summary");
  neo_label_set_font_padding (pref.summary, 14);
  gtk_widget_set_visible (pref.summary, summary && *summary);
  gtk_box_append (GTK_BOX (texts), pref.summary);
  gtk_widget_set_hexpand (texts, TRUE);
  gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (texts, "cal-pref-texts");
  gtk_box_append (GTK_BOX (box), texts);
  if (end)
    {
      gtk_widget_set_valign (end, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (end, "cal-pref-widget");
      gtk_box_append (GTK_BOX (box), end);
      pref.end = end;
    }
  gtk_widget_add_css_class (box, "cal-pref");
  pref.row = neo_pressable_new (box);
  neo_pressable_set_ripple (NEO_PRESSABLE (pref.row), NEO_RIPPLE_BOUNDED, 0);
  gtk_accessible_update_property (GTK_ACCESSIBLE (pref.row), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  return pref;
}

static void
set_summary (GtkWidget *summary, const char *text)
{
  gtk_label_set_label (GTK_LABEL (summary), text ? text : "");
  gtk_widget_set_visible (summary, text && *text);
}

/* PreferenceCategory: its title 14 sp in colorPrimary, 24 dp above, 8 dp below; an account's with its icon. One
 * after other rows has a 1 dp divider at the top of that space; the first on a page ("first") has none. */
static GtkWidget *
category (const char *icon_name, const char *title)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *frame = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new (title);

  gtk_widget_add_css_class (frame, "cal-pref-icon-frame");
  gtk_box_append (GTK_BOX (box), frame);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_add_css_class (label, "cal-pref-category-title");
  neo_label_set_font_padding (label, 14);
  gtk_box_append (GTK_BOX (box), label);
  gtk_widget_add_css_class (box, "cal-pref-category");
  gtk_accessible_update_property (GTK_ACCESSIBLE (box), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  if (icon_name)
    {
      /* The 24 dp icon is taller than the 19 dp title and adds nothing to the category's height (the title 8 dp
       * from both ends, 35 dp in all): over the row, 8 dp above the title's top and 7 px under its bottom. */
      GtkWidget *overlay = gtk_overlay_new ();
      GtkWidget *icon = row_icon (icon_name, 0);

      gtk_overlay_set_child (GTK_OVERLAY (overlay), box);
      gtk_widget_set_halign (icon, GTK_ALIGN_START);
      gtk_widget_set_valign (icon, GTK_ALIGN_END);
      gtk_widget_add_css_class (icon, "cal-pref-category-icon");
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), icon);
      return overlay;
    }
  return box;
}

static GtkWidget *
page_with_list (GtkWidget *window, const char *title, GtkWidget **list)
{
  GtkWidget *page = cal_page_new (window);
  GtkWidget *scroll = spongy_scroll_view_new ();

  gtk_box_append (GTK_BOX (page), cal_page_toolbar (CAL_PAGE (page), title, NULL));
  *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (*list, "cal-pref-list");
  spongy_scroll_view_set_child (SPONGY_SCROLL_VIEW (scroll), *list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (page), scroll);
  return page;
}

/* --- a switch preference: the row toggles it, as a tap on SwitchPreference does */

static void
on_switch_row (NeoPressable *row, CalCompatSwitch *toggle)
{
  cal_compat_switch_set_active (toggle, !cal_compat_switch_get_active (toggle));
}

static GtkWidget *
switch_pref (GSettings *settings, const char *key, const char *title)
{
  GtkWidget *toggle = cal_compat_switch_new ();
  Pref pref;

  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (toggle), g_settings_get_boolean (settings, key));
  gtk_accessible_update_property (GTK_ACCESSIBLE (toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
  g_settings_bind (settings, key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
  pref = pref_row (NULL, 0, title, NULL, toggle);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_switch_row), toggle);
  return pref.row;
}

/* --- a list preference: the entry as the summary, a dialog of the entries (ListPreference) */

typedef struct {
  GSettings *settings;
  char *key, *title;
  gboolean is_string;
  GPtrArray *labels;            /* the entries */
  GPtrArray *values;            /* the values, as strings */
  GtkWidget *summary;
} ListPref;

static void
list_pref_free (gpointer data)
{
  ListPref *list = data;

  g_object_unref (list->settings);
  g_free (list->key);
  g_free (list->title);
  g_ptr_array_unref (list->labels);
  g_ptr_array_unref (list->values);
  g_free (list);
}

static char *
list_value (ListPref *list)
{
  return list->is_string ? g_settings_get_string (list->settings, list->key)
                         : g_strdup_printf ("%d", g_settings_get_int (list->settings, list->key));
}

static int
list_index (ListPref *list)
{
  g_autofree char *value = list_value (list);

  for (guint i = 0; i < list->values->len; i++)
    if (g_str_equal (list->values->pdata[i], value))
      return (int) i;
  return -1;
}

static void
show_list_summary (ListPref *list)
{
  int index = list_index (list);

  set_summary (list->summary, index >= 0 ? list->labels->pdata[index] : NULL);
}

static void
on_list_chosen (int index, gpointer data)
{
  ListPref *list = data;

  if (index < 0)
    return;
  if (list->is_string)
    g_settings_set_string (list->settings, list->key, list->values->pdata[index]);
  else
    g_settings_set_int (list->settings, list->key, atoi (list->values->pdata[index]));
  show_list_summary (list);
}

static void
on_list_row (NeoPressable *row, ListPref *list)
{
  cal_list_dialog (GTK_WIDGET (row), list->title, (const char *const *) list->labels->pdata, (int) list->labels->len,
                   list_index (list), on_list_chosen, list);
}

static ListPref *
list_new (GSettings *settings, const char *key, const char *title, gboolean is_string)
{
  ListPref *list = g_new0 (ListPref, 1);

  list->settings = g_object_ref (settings);
  list->key = g_strdup (key);
  list->title = g_strdup (title);
  list->is_string = is_string;
  list->labels = g_ptr_array_new_with_free_func (g_free);
  list->values = g_ptr_array_new_with_free_func (g_free);
  return list;
}

static void
list_add (ListPref *list, char *label, char *value)
{
  g_ptr_array_add (list->labels, label);
  g_ptr_array_add (list->values, value);
}

static GtkWidget *
list_pref (ListPref *list)
{
  Pref pref = pref_row (NULL, 0, list->title, NULL, NULL);

  list->summary = pref.summary;
  show_list_summary (list);
  g_object_set_data_full (G_OBJECT (pref.row), "cal-list", list, list_pref_free);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_list_row), list);
  return pref.row;
}

/* EventViewUtils.constructReminderLabel: "None" below 0 */
static char *
minutes_label (int minutes)
{
  return minutes < 0 ? g_strdup (_("None")) : cal_format_reminder_minutes (minutes);
}

static GtkWidget *
minutes_pref (GSettings *settings, const char *key, const char *title, const int *values, int n_values)
{
  ListPref *list = list_new (settings, key, title, FALSE);

  for (int i = 0; i < n_values; i++)
    list_add (list, minutes_label (values[i]), g_strdup_printf ("%d", values[i]));
  return list_pref (list);
}

/* --- General settings (GeneralPreferences, general_preferences.xml) */

static GtkWidget *
general_page (GtkWidget *window)
{
  GSettings *settings = cal_window_get_settings (window);
  GtkWidget *list, *line, *page = page_with_list (window, _("General settings"), &list);
  ListPref *pref;
  /* preferences_default_duration_values */
  static const int durations[] = { 0, 15, 30, 60, 90, 120 };
  static const char *duration_labels[] = { N_("0 minutes"), N_("15 minutes"), N_("30 minutes"), N_("1 hour"),
                                           N_("1.5 hours"), N_("2 hours") };
  /* preferences_default_snooze_delay_values ("None": no Snooze button) */
  static const int snoozes[] = { -1, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 720 };
  /* preferences_default_reminder_values (Integer.MIN_VALUE "None" is -1 here) */
  static const int reminders[] = { -1, 0, 1, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 360, 540, 720, 1440, 2880,
                                   10080, 20160, 40320 };

  line = category (NULL, _("Calendar view setting"));
  gtk_widget_add_css_class (line, "first");
  gtk_box_append (GTK_BOX (list), line);
  gtk_box_append (GTK_BOX (list), switch_pref (settings, "real-event-colors", _("Use real event colors")));

  /* default_start_entries */
  pref = list_new (settings, "default-view", _("Default view"), TRUE);
  list_add (pref, g_strdup (_("Previously used view")), g_strdup ("previous"));
  list_add (pref, g_strdup (_("Day")), g_strdup ("day"));
  list_add (pref, g_strdup (_("Week")), g_strdup ("week"));
  list_add (pref, g_strdup (_("Month")), g_strdup ("month"));
  list_add (pref, g_strdup (_("Agenda")), g_strdup ("agenda"));
  gtk_box_append (GTK_BOX (list), list_pref (pref));

  /* buildWeekStartEntries: "Locale default", then the weekdays from Sunday (values 1 … 7) */
  pref = list_new (settings, "week-start", _("Week starts on"), FALSE);
  list_add (pref, g_strdup (_("Locale default")), g_strdup ("-1"));
  for (int i = 0; i < 7; i++)
    {
      g_autoptr(GDateTime) sunday = g_date_time_new_local (2023, 1, 1 + i, 12, 0, 0);    /* 2023-01-01 a Sunday */

      list_add (pref, cal_format_weekday (cal_day_number (g_date_time_to_unix (sunday))),
                g_strdup_printf ("%d", i + 1));
    }
  gtk_box_append (GTK_BOX (list), list_pref (pref));

  pref = list_new (settings, "default-event-duration", _("Default event duration"), FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (durations); i++)
    list_add (pref, g_strdup (_(duration_labels[i])), g_strdup_printf ("%d", durations[i]));
  gtk_box_append (GTK_BOX (list), list_pref (pref));

  gtk_box_append (GTK_BOX (list), category (NULL, _("Notifications & reminders")));
  gtk_box_append (GTK_BOX (list), switch_pref (settings, "notifications", _("Notifications")));
  gtk_box_append (GTK_BOX (list), minutes_pref (settings, "default-snooze", _("Default snooze delay time"), snoozes,
                                                G_N_ELEMENTS (snoozes)));
  gtk_box_append (GTK_BOX (list), minutes_pref (settings, "default-reminder", _("Default reminder time"), reminders,
                                                G_N_ELEMENTS (reminders)));
  return page;
}

/* --- a calendar's page (CalendarPreferences) */

typedef struct {
  GtkWidget *window;
  char *uid;
} CalendarPage;

static void
calendar_page_free (gpointer data)
{
  CalendarPage *page = data;

  g_free (page->uid);
  g_free (page);
}

static void
on_visible (GObject *toggle, GParamSpec *pspec, CalendarPage *page)
{
  cal_store_set_visible (cal_window_get_store (page->window), page->uid,
                         cal_compat_switch_get_active (CAL_COMPAT_SWITCH (toggle)));
}

static GtkWidget *
calendar_page (GtkWidget *window, const CalCalendar *calendar)
{
  GtkWidget *list, *page;
  GtkWidget *toggle = cal_compat_switch_new ();
  CalendarPage *data = g_new0 (CalendarPage, 1);
  Pref pref;

  page = page_with_list (window, calendar->name && *calendar->name ? calendar->name : _("Unnamed calendar"), &list);
  data->window = window;
  data->uid = g_strdup (calendar->uid);
  g_object_set_data_full (G_OBJECT (page), "cal-calendar", data, calendar_page_free);
  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (toggle), calendar->visible);
  gtk_accessible_update_property (GTK_ACCESSIBLE (toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Display events"), -1);
  g_signal_connect (toggle, "notify::active", G_CALLBACK (on_visible), data);
  pref = pref_row (NULL, 0, _("Display events"), NULL, toggle);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_switch_row), toggle);
  gtk_box_append (GTK_BOX (list), pref.row);
  return page;
}

/* --- the main list (MainListPreferences) */

typedef struct {
  GtkWidget *window, *list, *calendars;
  gulong handler;
  CalStore *store;
} Settings;

static void
settings_free (gpointer data)
{
  Settings *settings = data;

  g_signal_handler_disconnect (settings->store, settings->handler);
  g_object_unref (settings->store);
  g_free (settings);
}

static void
on_general (NeoPressable *row, Settings *settings)
{
  cal_window_push (settings->window, general_page (settings->window));
}

static void
on_calendar (NeoPressable *row, Settings *settings)
{
  const CalCalendar *calendar = cal_store_find_calendar (settings->store, g_object_get_data (G_OBJECT (row), "cal-uid"));

  if (calendar)
    cal_window_push (settings->window, calendar_page (settings->window, calendar));
}

static void
fill_calendars (Settings *settings)
{
  GPtrArray *calendars = cal_store_get_calendars (settings->store);
  const char *account = NULL;
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (settings->calendars)))
    gtk_box_remove (GTK_BOX (settings->calendars), child);
  /* per account a header (outline_account_circle, its name), then its calendars: the circle in the calendar's
   * colour, an outline when its events are not displayed */
  for (guint i = 0; i < calendars->len; i++)
    {
      const CalCalendar *calendar = calendars->pdata[i];
      Pref pref;

      if (!account || g_strcmp0 (account, calendar->account) != 0)
        {
          account = calendar->account;
          gtk_box_append (GTK_BOX (settings->calendars), category ("cal-account-symbolic", account));
        }
      pref = pref_row (calendar->visible ? "cal-circle-symbolic" : "cal-circle-outline-symbolic",
                       0xff000000 | calendar->color,
                       calendar->name && *calendar->name ? calendar->name : _("Unnamed calendar"),
                       calendar->visible ? NULL : _("Events are not displayed"), NULL);
      g_object_set_data_full (G_OBJECT (pref.row), "cal-uid", g_strdup (calendar->uid), g_free);
      g_signal_connect (pref.row, "clicked", G_CALLBACK (on_calendar), settings);
      gtk_box_append (GTK_BOX (settings->calendars), pref.row);
    }
}

static void
on_calendars_changed (CalStore *store, Settings *settings)
{
  fill_calendars (settings);
}

GtkWidget *
cal_settings_page_new (GtkWidget *window)
{
  Settings *settings = g_new0 (Settings, 1);
  GtkWidget *list, *page = page_with_list (window, _("Settings"), &list);
  Pref pref;

  settings->window = window;
  settings->list = list;
  settings->store = g_object_ref (cal_window_get_store (window));
  g_object_set_data_full (G_OBJECT (page), "cal-settings", settings, settings_free);

  pref = pref_row ("cal-settings-symbolic", 0, _("General settings"), NULL, NULL);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_general), settings);
  gtk_box_append (GTK_BOX (list), pref.row);

  settings->calendars = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (list), settings->calendars);
  fill_calendars (settings);
  settings->handler = g_signal_connect (settings->store, "calendars-changed", G_CALLBACK (on_calendars_changed),
                                        settings);
  return page;
}

/* --- About (about.xml: the launcher icon 120 dp, then "Build version") */

GtkWidget *
cal_about_page_new (GtkWidget *window)
{
  GtkWidget *list, *page = page_with_list (window, _("About Calendar"), &list);
  GtkWidget *icon = gtk_image_new_from_icon_name ("de.yesman.Calendar");
  Pref pref;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 120);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "cal-about-icon");
  gtk_box_append (GTK_BOX (list), icon);
  /* Authors, the licence and the source code link to Etar's project pages: left out, they are not this app's */
  pref = pref_row ("cal-pencil-symbolic", 0, _("Build version"), CAL_VERSION, NULL);
  gtk_widget_add_css_class (pref.row, "cal-about-row");
  gtk_box_append (GTK_BOX (list), pref.row);
  return page;
}
