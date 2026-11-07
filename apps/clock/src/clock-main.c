/* neo-clock: the Clock app, DeskClock rebuilt in GTK 4 (spec/clock.md).
 *
 * One program, two applications:
 *  - de.yesman.Clock, the app with its tabs. It is D-Bus activatable: the launcher starts it, and the alarm
 *    service opens it at an alarm with its "show-alarm" action (a tap on an alarm notification).
 *  - de.yesman.Clock.Ringing ("neo-clock --ringing"), the full-screen ringing alarm. The alarm service has the shell
 *    start it when an alarm rings while the phone sleeps or is locked; it has a desktop file of its own so the
 *    shell lets it over the lock screen.
 *
 * The alarms themselves are not here: the alarm service (neo-clock-alarms) keeps them, rings them and wakes the
 * phone for them, whether or not this app runs. */
#include "clock-window.h"
#include "ringing-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>
#include <neo.h>

#define APP_ID        "de.yesman.Clock"
#define RINGING_ID    "de.yesman.Clock.Ringing"

static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/Clock/clock.css");
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
}

/* --- the app */

static GtkWidget *
clock_window (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  return window ? GTK_WIDGET (window) : clock_window_new (app, settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (clock_window (GTK_APPLICATION (app))));
}

/* the alarm service's "open the app at this alarm" */
static void
on_show_alarm (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = clock_window (GTK_APPLICATION (data));

  clock_window_show_alarm (window, g_variant_get_uint32 (parameter));
  gtk_window_present (GTK_WINDOW (window));
}

/* --- the ringing alarm */

static void
on_activate_ringing (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  /* nothing ringing (any more): no window, and with none the application ends */
  if (!window)
    window = GTK_WINDOW (ringing_window_new (GTK_APPLICATION (app), settings));
  if (window)
    gtk_window_present (window);
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  gboolean ringing = FALSE;
  int status;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* --ringing is ours, not GApplication's: take it out before GApplication sees the arguments */
  for (int i = 1; i < argc; i++)
    if (g_str_equal (argv[i], "--ringing"))
      {
        ringing = TRUE;
        memmove (&argv[i], &argv[i + 1], (argc - i) * sizeof *argv);
        argc--;
        break;
      }

  settings = g_settings_new (APP_ID);
  app = gtk_application_new (ringing ? RINGING_ID : APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  /* both find the icons and the style under the app's path (otherwise the Ringing one would look in .../Ringing) */
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/yesman/Clock");
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  if (ringing)
    g_signal_connect (app, "activate", G_CALLBACK (on_activate_ringing), NULL);
  else
    {
      static const GActionEntry actions[] = {
        { .name = "show-alarm", .activate = on_show_alarm, .parameter_type = "u" },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    }

  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
