/* Settings and About (cal-settings.h). */
#include "cal-settings.h"
#include "cal-accounts.h"
#include "cal-alerts.h"
#include "cal-material-switch.h"
#include "cal-tz-picker.h"
#include "cal-colors.h"
#include "cal-compat-switch.h"
#include "cal-dialogs.h"
#include "cal-format.h"
#include "cal-page.h"
#include "cal-prefs.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <neo.h>
#include <spongy-gtk.h>

#ifndef CAL_VERSION
#define CAL_VERSION "0"
#endif

/* --- General settings (GeneralPreferences, xml-v26/general_preferences.xml: the screen Android 8 and later get) */

typedef struct {
  GtkWidget *window;
  GSettings *settings, *interface;
  GtkWidget *pure_black, *home_tz, *home_tz_summary, *custom_snooze;
  gulong handlers[4];
} General;

static void
general_free (gpointer data)
{
  General *general = data;

  for (int i = 0; i < 3; i++)
    if (general->handlers[i])
      g_signal_handler_disconnect (general->settings, general->handlers[i]);
  if (general->handlers[3])
    g_signal_handler_disconnect (general->interface, general->handlers[3]);
  g_clear_object (&general->interface);
  g_object_unref (general->settings);
  g_free (general);
}

/* Pure black night mode only while the look is dark (removePreferenceRecursively when the theme is light, or the
 * system's and the system is not dark) */
static void
show_pure_black (General *general)
{
  gtk_widget_set_visible (general->pure_black, cal_palette->dark);
}

static void
on_theme_changed (GSettings *settings, const char *key, General *general)
{
  /* after cal-main's handler has set the look */
  show_pure_black (general);
}

/* Home time zone: the zone's name as the picker shows it; disabled unless "Use home time zone" is on
 * (app:dependency) */
static void
show_home_tz (General *general)
{
  g_autofree char *tzid = g_settings_get_string (general->settings, "home-tz");
  g_autofree char *label = cal_tz_label_markup (*tzid ? tzid : cal_phone_zone (), g_get_real_time () / G_USEC_PER_SEC,
                                                FALSE);

  cal_pref_set_summary_markup (general->home_tz_summary, label);
  gtk_widget_set_sensitive (general->home_tz, g_settings_get_boolean (general->settings, "home-tz-enabled"));
}

static void
on_home_tz_setting (GSettings *settings, const char *key, General *general)
{
  show_home_tz (general);
}

static void
on_home_tz_chosen (const char *tzid, gpointer data)
{
  General *general = data;

  g_settings_set_string (general->settings, "home-tz", tzid);
}

static void
on_home_tz (NeoPressable *row, General *general)
{
  g_autofree char *tzid = g_settings_get_string (general->settings, "home-tz");

  cal_tz_picker_show (GTK_WIDGET (row), *tzid ? tzid : cal_phone_zone (), g_get_real_time () / G_USEC_PER_SEC,
                      on_home_tz_chosen, general);
}

static void
on_home_tz_check (GObject *check, GParamSpec *pspec, General *general)
{
  gboolean on = cal_check_box_get_active (CAL_CHECK_BOX (check));
  g_autofree char *tzid = g_settings_get_string (general->settings, "home-tz");

  /* Etar keeps the zone it last had: the phone's at the time, the first time */
  if (on && !*tzid)
    g_settings_set_string (general->settings, "home-tz", cal_phone_zone ());
  g_settings_set_boolean (general->settings, "home-tz-enabled", on);
}

static void
on_check_row (NeoPressable *row, CalCheckBox *check)
{
  cal_check_box_set_active (check, !cal_check_box_get_active (check));
}

static void
on_clear_history (NeoPressable *row, General *general)
{
  g_settings_reset (general->settings, "search-history");
  cal_window_toast (general->window, _("Search history cleared."));
}

/* Ask for reminder snooze delay: enabled only with a snooze delay (handleUseCustomSnoozeDelayVisibility), its
 * summary per state (summaryOn, summaryOff) */
static void
show_custom_snooze (General *general)
{
  GtkWidget *summary = g_object_get_data (G_OBJECT (general->custom_snooze), "cal-pref-summary");
  gboolean on = g_settings_get_boolean (general->settings, "custom-snooze");

  gtk_widget_set_sensitive (general->custom_snooze, g_settings_get_int (general->settings, "default-snooze") >= 0);
  cal_pref_set_summary (summary, on ? _("Ask for snooze delay whenever snoozing reminders")
                                          : _("Always use default snooze delay"));
}

static void
on_snooze_setting (GSettings *settings, const char *key, General *general)
{
  show_custom_snooze (general);
}

static void
on_notifications (NeoPressable *row, General *general)
{
  cal_window_push (general->window, cal_notifications_page_new (general->window));
}

static void
on_display_options (NeoPressable *row, General *general)
{
  cal_window_push (general->window, cal_display_options_page_new (general->window));
}

static void
on_quick_responses (NeoPressable *row, General *general)
{
  cal_window_push (general->window, cal_quick_responses_page_new (general->window));
}

static GtkWidget *
plain_pref (const char *title, const char *summary, GCallback clicked, gpointer data)
{
  CalPref pref = cal_pref_row (NULL, 0, title, summary, NULL);

  g_signal_connect (pref.row, "clicked", clicked, data);
  return pref.row;
}

static GtkWidget *
general_page (GtkWidget *window)
{
  GSettings *settings = cal_window_get_settings (window);
  GtkWidget *list, *line, *page = cal_pref_page (window, _("General settings"), &list);
  General *general = g_new0 (General, 1);
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) interface = schemas ? g_settings_schema_source_lookup (schemas,
                                                                                    "org.gnome.desktop.interface",
                                                                                    TRUE) : NULL;
  CalListPref *pref;
  CalPref row;
  GtkWidget *check;
  /* preferences_default_duration_values */
  static const int durations[] = { 0, 15, 30, 60, 90, 120 };
  static const char *duration_labels[] = { N_("0 minutes"), N_("15 minutes"), N_("30 minutes"), N_("1 hour"),
                                           N_("1.5 hours"), N_("2 hours") };
  /* preferences_days_per_week_labels */
  static const char *days_labels[] = { N_("2 days"), N_("3 days"), N_("4 days"), N_("5 days"), N_("6 days"),
                                       N_("7 days") };
  /* preferences_default_snooze_delay_values ("None": no Snooze button) */
  static const int snoozes[] = { -1, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 720 };
  /* preferences_default_reminder_values (Integer.MIN_VALUE "None" is -1 here) */
  static const int reminders[] = { -1, 0, 1, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 360, 540, 720, 1440, 2880,
                                   10080, 20160, 40320 };

  general->window = window;
  general->settings = g_object_ref (settings);
  g_object_set_data_full (G_OBJECT (page), "cal-general", general, general_free);

  line = cal_pref_category (NULL, _("Calendar view setting"));
  gtk_widget_add_css_class (line, "first");
  gtk_box_append (GTK_BOX (list), line);

  /* pref_theme_entries (values-v29): System default, Light, Dark */
  pref = cal_list_pref_new (settings, "theme", _("Theme"), TRUE);
  cal_list_pref_add (pref, g_strdup (_("System default")), g_strdup ("system"));
  cal_list_pref_add (pref, g_strdup (_("Light")), g_strdup ("light"));
  cal_list_pref_add (pref, g_strdup (_("Dark")), g_strdup ("dark"));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));
  general->pure_black = cal_pref_switch (settings, "pure-black", _("Pure black night mode"));
  gtk_box_append (GTK_BOX (list), general->pure_black);
  show_pure_black (general);
  general->handlers[0] = g_signal_connect_after (settings, "changed::theme", G_CALLBACK (on_theme_changed), general);
  if (interface && g_settings_schema_has_key (interface, "color-scheme"))
    {
      general->interface = g_settings_new ("org.gnome.desktop.interface");
      general->handlers[3] = g_signal_connect_after (general->interface, "changed::color-scheme",
                                                     G_CALLBACK (on_theme_changed), general);
    }

  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, "real-event-colors", _("Use real event colors")));

  /* default_start_entries */
  pref = cal_list_pref_new (settings, "default-view", _("Default view"), TRUE);
  cal_list_pref_add (pref, g_strdup (_("Previously used view")), g_strdup ("previous"));
  cal_list_pref_add (pref, g_strdup (_("Day")), g_strdup ("day"));
  cal_list_pref_add (pref, g_strdup (_("Week")), g_strdup ("week"));
  cal_list_pref_add (pref, g_strdup (_("Month")), g_strdup ("month"));
  cal_list_pref_add (pref, g_strdup (_("Agenda")), g_strdup ("agenda"));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));

  gtk_box_append (GTK_BOX (list), plain_pref (_("Display options"), NULL, G_CALLBACK (on_display_options), general));
  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, "hide-declined", _("Hide declined events")));
  row = cal_pref_row (NULL, 0, _("Staggered display"), _("Display overlapping events in a staggered layout"),
                      cal_compat_switch_new ());
  cal_compat_switch_set_active (CAL_COMPAT_SWITCH (row.end), g_settings_get_boolean (settings, "staggered"));
  g_settings_bind (settings, "staggered", row.end, "active", G_SETTINGS_BIND_DEFAULT);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row.end), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Staggered display"),
                                  -1);
  g_signal_connect (row.row, "clicked", G_CALLBACK (cal_pref_toggle_row), row.end);
  gtk_box_append (GTK_BOX (list), row.row);
  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, "show-week-number", _("Show week number")));

  /* buildWeekStartEntries: "Locale default", then the weekdays from Sunday (values 1 … 7) */
  pref = cal_list_pref_new (settings, "week-start", _("Week starts on"), FALSE);
  cal_list_pref_add (pref, g_strdup (_("Locale default")), g_strdup ("-1"));
  for (int i = 0; i < 7; i++)
    {
      g_autoptr(GDateTime) sunday = g_date_time_new_local (2023, 1, 1 + i, 12, 0, 0);    /* 2023-01-01 a Sunday */

      cal_list_pref_add (pref, cal_format_weekday (cal_day_number (g_date_time_to_unix (sunday))),
                         g_strdup_printf ("%d", i + 1));
    }
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));

  pref = cal_list_pref_new (settings, "days-per-week", _("Days per week"), FALSE);
  for (int i = 0; i < 6; i++)
    cal_list_pref_add (pref, g_strdup (_(days_labels[i])), g_strdup_printf ("%d", i + 2));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));

  pref = cal_list_pref_new (settings, "default-event-duration", _("Default event duration"), FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (durations); i++)
    cal_list_pref_add (pref, g_strdup (_(duration_labels[i])), g_strdup_printf ("%d", durations[i]));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));

  /* Use home time zone: a CheckBoxPreference; Home time zone depends on it */
  check = cal_check_box_new ();
  cal_check_box_set_active (CAL_CHECK_BOX (check), g_settings_get_boolean (settings, "home-tz-enabled"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Use home time zone"), -1);
  g_signal_connect (check, "notify::active", G_CALLBACK (on_home_tz_check), general);
  row = cal_pref_row (NULL, 0, _("Use home time zone"),
                      _("Displays calendars and event times in your home time zone when traveling"), check);
  g_signal_connect (row.row, "clicked", G_CALLBACK (on_check_row), check);
  gtk_box_append (GTK_BOX (list), row.row);
  row = cal_pref_row (NULL, 0, _("Home time zone"), NULL, NULL);
  general->home_tz = row.row;
  general->home_tz_summary = row.summary;
  g_signal_connect (row.row, "clicked", G_CALLBACK (on_home_tz), general);
  gtk_box_append (GTK_BOX (list), row.row);
  show_home_tz (general);
  general->handlers[1] = g_signal_connect (settings, "changed", G_CALLBACK (on_home_tz_setting), general);

  gtk_box_append (GTK_BOX (list), plain_pref (_("Clear search history"), _("Remove all the searches you've performed"),
                                              G_CALLBACK (on_clear_history), general));

  /* Notifications & reminders: "Notifications" opens the channel's page on Android, its sound and vibration;
   * here the app's own (cal_notifications_page_new), what the reminder service does */
  gtk_box_append (GTK_BOX (list), cal_pref_category (NULL, _("Notifications & reminders")));
  gtk_box_append (GTK_BOX (list), plain_pref (_("Notifications"), NULL, G_CALLBACK (on_notifications), general));
  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, "popup", _("Pop-up notification")));
  general->custom_snooze = cal_pref_switch (settings, "custom-snooze", _("Ask for reminder snooze delay"));
  gtk_box_append (GTK_BOX (list), general->custom_snooze);
  gtk_box_append (GTK_BOX (list), cal_pref_minutes (settings, "default-snooze", _("Default snooze delay time"), snoozes,
                                                    G_N_ELEMENTS (snoozes)));
  show_custom_snooze (general);
  general->handlers[2] = g_signal_connect (settings, "changed", G_CALLBACK (on_snooze_setting), general);
  gtk_box_append (GTK_BOX (list), cal_pref_minutes (settings, "default-reminder", _("Default reminder time"), reminders,
                                                    G_N_ELEMENTS (reminders)));
  /* Quick responses are the texts of "Email guests", which needs a mail app: without one there is nothing to
   * answer with, and the row goes */
  if (cal_can_mail ())
    gtk_box_append (GTK_BOX (list), plain_pref (_("Quick responses"), _("Edit default responses when emailing guests"),
                                                G_CALLBACK (on_quick_responses), general));

  /* Experimental: Skip Reminders (preferences_skip_reminders_labels; no default, so no summary until one is
   * chosen). "Don't check battery optimization" is Android's Doze and has nothing here to stand for. */
  gtk_box_append (GTK_BOX (list), cal_pref_category (NULL, _("Experimental")));
  pref = cal_list_pref_new (settings, "skip-reminders", _("Skip Reminders"), TRUE);
  cal_list_pref_add (pref, g_strdup (_("Only if declined")), g_strdup ("declined"));
  cal_list_pref_add (pref, g_strdup (_("If declined or not responded")), g_strdup ("not_responded"));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));
  return page;
}

/* --- Display options (ViewDetailsPreferences, view_details_preferences.xml): the Month view's events, upright and
 * on its side */

static GtkWidget *
month_view_prefs (GSettings *settings, GtkWidget *list, const char *title, const char *which)
{
  g_autofree char *lines = g_strconcat ("month-lines-", which, NULL);
  g_autofree char *time = g_strconcat ("month-time-", which, NULL);
  g_autofree char *location = g_strconcat ("month-location-", which, NULL);
  CalListPref *pref;
  GtkWidget *category = cal_pref_category (NULL, title);

  gtk_box_append (GTK_BOX (list), category);
  pref = cal_list_pref_new (settings, lines, _("Max number of lines in event"), FALSE);
  for (int i = 1; i <= 7; i++)
    cal_list_pref_add (pref, g_strdup_printf ("%d", i), g_strdup_printf ("%d", i));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));
  /* show_time: a phone (show_time_in_month false) drops the last entry, "Start and end time below the event",
   * and shows no time by default */
  pref = cal_list_pref_new (settings, time, _("Display time"), FALSE);
  cal_list_pref_add (pref, g_strdup (_("Don't show time")), g_strdup ("0"));
  cal_list_pref_add (pref, g_strdup (_("Only start time")), g_strdup ("1"));
  cal_list_pref_add (pref, g_strdup (_("Start and end time")), g_strdup ("2"));
  gtk_box_append (GTK_BOX (list), cal_list_pref_widget (pref));
  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, location, _("Display location")));
  return category;
}

GtkWidget *
cal_display_options_page_new (GtkWidget *window)
{
  GSettings *settings = cal_window_get_settings (window);
  GtkWidget *list, *page = cal_pref_page (window, _("Display options"), &list);

  gtk_widget_add_css_class (month_view_prefs (settings, list, _("Month view layout (Portrait)"), "portrait"), "first");
  month_view_prefs (settings, list, _("Month view layout (Landscape)"), "landscape");
  return page;
}

/* --- Notifications: what Android's page for Etar's reminder channel holds that the reminder service does: whether
 * it notifies, the sound, the vibration (the last only on a phone that vibrates) */

GtkWidget *
cal_notifications_page_new (GtkWidget *window)
{
  GSettings *settings = cal_window_get_settings (window);
  GtkWidget *list, *page = cal_pref_page (window, _("Notifications"), &list);
  GtkWidget *sound, *vibrate = NULL;
  CalListPref *pref;
  g_autoptr(GPtrArray) sounds = cal_alerts_sounds ();

  gtk_box_append (GTK_BOX (list), cal_pref_switch (settings, "notifications", _("Show notifications")));
  /* RingtonePreference's picker: the default sound, none, the phone's notification sounds */
  pref = cal_list_pref_new (settings, "reminder-sound", _("Sound"), TRUE);
  cal_list_pref_add (pref, cal_alerts_sound_label ("default"), g_strdup ("default"));
  cal_list_pref_add (pref, cal_alerts_sound_label (""), g_strdup (""));
  for (guint i = 0; i < sounds->len; i++)
    {
      CalSound *s = sounds->pdata[i];

      cal_list_pref_add (pref, g_strdup (s->name), g_strdup (s->path));
    }
  sound = cal_list_pref_widget (pref);
  gtk_box_append (GTK_BOX (list), sound);
  g_settings_bind (settings, "notifications", sound, "sensitive", G_SETTINGS_BIND_GET);
  if (cal_alerts_can_vibrate ())
    {
      vibrate = cal_pref_switch (settings, "reminder-vibrate", _("Vibrate"));
      g_settings_bind (settings, "notifications", vibrate, "sensitive", G_SETTINGS_BIND_GET);
      gtk_box_append (GTK_BOX (list), vibrate);
    }
  return page;
}

/* --- Quick responses (QuickResponsePreferences): the responses sorted, each an EditTextPreference */

typedef struct {
  GtkWidget *window, *list;
  GSettings *settings;
} Responses;

static void fill_responses (Responses *responses);

static void
responses_free (gpointer data)
{
  Responses *responses = data;

  g_object_unref (responses->settings);
  g_free (responses);
}

typedef struct {
  Responses *responses;
  int index;
} ResponseEdit;

static void
on_response_edited (const char *text, gpointer data)
{
  ResponseEdit *edit = data;
  g_auto(GStrv) all = g_settings_get_strv (edit->responses->settings, "quick-responses");

  if (text && edit->index < (int) g_strv_length (all) && g_strcmp0 (all[edit->index], text) != 0)
    {
      g_free (all[edit->index]);
      all[edit->index] = g_strdup (text);
      g_settings_set_strv (edit->responses->settings, "quick-responses", (const char *const *) all);
      fill_responses (edit->responses);
    }
  g_free (edit);
}

static void
on_response_row (NeoPressable *row, Responses *responses)
{
  ResponseEdit *edit = g_new0 (ResponseEdit, 1);
  g_auto(GStrv) all = g_settings_get_strv (responses->settings, "quick-responses");

  edit->responses = responses;
  edit->index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "cal-index"));
  cal_text_dialog (GTK_WIDGET (row), _("Quick response"), all[edit->index], on_response_edited, edit);
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (*(char * const *) a, *(char * const *) b);
}

static void
fill_responses (Responses *responses)
{
  g_auto(GStrv) all = g_settings_get_strv (responses->settings, "quick-responses");
  guint n = g_strv_length (all);
  GtkWidget *child;

  /* Arrays.sort, written back sorted so the rows' indices are the key's */
  qsort (all, n, sizeof (char *), compare_strings);
  g_settings_set_strv (responses->settings, "quick-responses", (const char *const *) all);
  while ((child = gtk_widget_get_first_child (responses->list)))
    gtk_box_remove (GTK_BOX (responses->list), child);
  for (guint i = 0; i < n; i++)
    {
      CalPref pref = cal_pref_row (NULL, 0, all[i], NULL, NULL);

      g_object_set_data (G_OBJECT (pref.row), "cal-index", GINT_TO_POINTER (i));
      g_signal_connect (pref.row, "clicked", G_CALLBACK (on_response_row), responses);
      gtk_box_append (GTK_BOX (responses->list), pref.row);
    }
}

GtkWidget *
cal_quick_responses_page_new (GtkWidget *window)
{
  Responses *responses = g_new0 (Responses, 1);
  GtkWidget *page = cal_pref_page (window, _("Quick responses"), &responses->list);

  responses->window = window;
  responses->settings = g_object_ref (cal_window_get_settings (window));
  g_object_set_data_full (G_OBJECT (page), "cal-responses", responses, responses_free);
  fill_responses (responses);
  return page;
}

/* a mail app to write with (Email guests, Quick responses): the mailto: handler */
gboolean
cal_can_mail (void)
{
  g_autoptr(GAppInfo) app = g_app_info_get_default_for_uri_scheme ("mailto");

  return app != NULL;
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
    cal_window_push (settings->window, cal_calendar_page_new (settings->window, calendar));
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
      CalPref pref;

      if (!account || g_strcmp0 (account, calendar->account) != 0)
        {
          account = calendar->account;
          gtk_box_append (GTK_BOX (settings->calendars), cal_pref_category ("cal-account-symbolic", account));
        }
      pref = cal_pref_row (calendar->visible ? "cal-circle-symbolic" : "cal-circle-outline-symbolic",
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
  GtkWidget *list, *page = cal_pref_page (window, _("Settings"), &list);
  CalPref pref;

  settings->window = window;
  settings->list = list;
  settings->store = g_object_ref (cal_window_get_store (window));
  g_object_set_data_full (G_OBJECT (page), "cal-settings", settings, settings_free);

  pref = cal_pref_row ("cal-settings-symbolic", 0, _("General settings"), NULL, NULL);
  g_signal_connect (pref.row, "clicked", G_CALLBACK (on_general), settings);
  gtk_box_append (GTK_BOX (list), pref.row);
  /* MainListPreferences' "Add CalDAV calendar" (DAVx⁵ on Android; EDS's webdav backend here, hidden without it),
   * "Add offline calendar". "Add EteSync calendar" is left out: EDS has no EteSync backend on Debian, and the row
   * would only promise one. */
  if (cal_accounts_can_caldav ())
    {
      pref = cal_pref_row ("cal-add-symbolic", 0, _("Add CalDAV calendar"), NULL, NULL);
      g_signal_connect_swapped (pref.row, "clicked", G_CALLBACK (cal_accounts_add_caldav), window);
      gtk_box_append (GTK_BOX (list), pref.row);
    }
  pref = cal_pref_row ("cal-add-symbolic", 0, _("Add offline calendar"), NULL, NULL);
  g_signal_connect_swapped (pref.row, "clicked", G_CALLBACK (cal_accounts_add_offline), window);
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
  GtkWidget *list, *page = cal_pref_page (window, _("About Calendar"), &list);
  GtkWidget *icon = gtk_image_new_from_icon_name ("de.yesman.Calendar");
  CalPref pref;

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 120);
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (icon, "cal-about-icon");
  gtk_box_append (GTK_BOX (list), icon);
  /* Authors, the licence and the source code link to Etar's project pages: left out, they are not this app's */
  pref = cal_pref_row ("cal-pencil-symbolic", 0, _("Build version"), CAL_VERSION, NULL);
  gtk_widget_add_css_class (pref.row, "cal-about-row");
  gtk_box_append (GTK_BOX (list), pref.row);
  return page;
}
