/* vibeos-clock: the Clock app, DeskClock rebuilt in GTK 4.
 *
 * One program, two applications:
 *  - vibeos.Clock, the app with its tabs. It is D-Bus activatable: the launcher starts it, and the clock
 *    service opens it at an alarm, a timer or the stopwatch with its "show-alarm", "show-timer" and
 *    "show-stopwatch" actions (a tap on their notifications).
 *  - vibeos.Clock.Ringing ("vibeos-clock --ringing"), the full-screen ringing alarm and the timers that ran out.
 *    The clock service has the shell start it when an alarm rings while the phone sleeps or is locked, and when a
 *    timer runs out; it has a desktop file of its own so the shell lets it over the lock screen.
 *
 * The alarms, timers and stopwatch themselves are not here: the clock service (vibeos-clock-alarms) keeps them, rings
 * them and wakes the phone for them, whether or not this app runs. */
#include "clock-window.h"
#include "expired-window.h"
#include "ringing-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>
#include <vibeos.h>

#define APP_ID        "vibeos.Clock"
#define RINGING_ID    "vibeos.Clock.Ringing"

static GSettings *settings;
static GtkWidget *alarm_window;         /* the Ringing app's ringing alarm, while it is open */

static void
on_startup (GApplication *app, gpointer data)
{
  gboolean ringing = GPOINTER_TO_INT (data);

  /* the text follows Font size and Bold text; the stylesheet in sp and dp, over libvibeos's. The ringing alarm and
   * the timers that ran out are dark in light mode too, as on the reference phone */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT | (ringing ? VIBEOS_INIT_ALWAYS_DARK : 0));
  vibeos_css_load_resource ("/vibeos/Clock/clock.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
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

/* ... at a timer */
static void
on_show_timer (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = clock_window (GTK_APPLICATION (data));

  clock_window_show_timer (window, g_variant_get_uint32 (parameter));
  gtk_window_present (GTK_WINDOW (window));
}

/* ... at the stopwatch */
static void
on_show_stopwatch (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = clock_window (GTK_APPLICATION (data));

  clock_window_show_stopwatch (window);
  gtk_window_present (GTK_WINDOW (window));
}

/* --- the ringing alarm and the timers that ran out */

/* a volume key, which the shell passes on while the expired timers' screen is in front (expired-window.c) */
/* a volume key from the shell, to whichever ringing window took the keys: the alarm's if it is in front and its
 * setting wants them, else the expired timers' */
static void
on_volume_key (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  if (alarm_window && ringing_window_volume_key (alarm_window))
    return;
  expired_window_volume_key ();
}

/* each has its window, either or both; with nothing ringing (any more) there is none, and the application ends */
static void
on_activate_ringing (GApplication *app, gpointer data)
{
  if (!alarm_window)
    {
      alarm_window = ringing_window_new (GTK_APPLICATION (app), settings);
      if (alarm_window)
        g_object_add_weak_pointer (G_OBJECT (alarm_window), (gpointer *) &alarm_window);
    }
  if (alarm_window)
    gtk_window_present (GTK_WINDOW (alarm_window));
  expired_window_show (GTK_APPLICATION (app));
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
  g_application_set_resource_base_path (G_APPLICATION (app), "/vibeos/Clock");
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), GINT_TO_POINTER (ringing));
  if (ringing)
    {
      static const GActionEntry actions[] = {
        { .name = "volume-key", .activate = on_volume_key, .parameter_type = "i" },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate_ringing), NULL);
    }
  else
    {
      static const GActionEntry actions[] = {
        { .name = "show-alarm", .activate = on_show_alarm, .parameter_type = "u" },
        { .name = "show-timer", .activate = on_show_timer, .parameter_type = "u" },
        { .name = "show-stopwatch", .activate = on_show_stopwatch },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    }

  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
