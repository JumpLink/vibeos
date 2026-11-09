/* neo-calendar: the Calendar app, Etar rebuilt in GTK 4 over evolution-data-server (spec/calendar.md).
 *
 * de.yesman.Calendar, D-Bus activatable: the launcher starts it; the reminder service (neo-calendar-reminders)
 * opens an event's info with the "open-event" action when its notification is tapped; an .ics file opened with it
 * goes into the editor as a new event (Etar's ImportActivity).
 *
 * The reminders are not here: the reminder service notifies for them, and wakes the phone for them, whether or not
 * this app runs. */
#include "cal-store.h"
#include "cal-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <neo.h>

#define APP_ID "de.yesman.Calendar"

static GSettings *settings;
static CalStore *store;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/Calendar/calendar.css");
  /* over libneo's, which is at APPLICATION priority too: the later provider wins at equal priority */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
  store = cal_store_new (FALSE);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  g_clear_object (&store);
}

static GtkWidget *
calendar_window (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  return window ? GTK_WIDGET (window) : cal_window_new (app, store, settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (calendar_window (GTK_APPLICATION (app))));
}

/* an .ics file (MimeType=text/calendar): its first event in the editor */
static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (app));

  gtk_window_present (GTK_WINDOW (window));
  if (n_files > 0)
    cal_window_import (window, files[0]);
}

/* the reminder service's "open this event": (calendar uid, event uid, recurrence id or "", start) */
static void
on_open_event (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = calendar_window (GTK_APPLICATION (data));
  const char *calendar, *uid, *rid;
  gint64 start;

  g_variant_get (parameter, "(&s&s&sx)", &calendar, &uid, &rid, &start);
  gtk_window_present (GTK_WINDOW (window));
  cal_window_open_reminder (window, calendar, uid, rid, start);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  static const GActionEntry actions[] = {
    { .name = "open-event", .activate = on_open_event, .parameter_type = "(sssx)" },
  };
  int status;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  settings = g_settings_new (APP_ID);
  app = gtk_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/yesman/Calendar");
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (settings);
  return status;
}
