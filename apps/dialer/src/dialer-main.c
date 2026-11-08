/* neo-dialer: the Phone app, LineageOS's Dialer rebuilt in GTK 4 (spec/dialer.md).
 *
 * One program, two applications, as the Clock has them:
 *  - de.yesman.Dialer, the app with its tabs (Favorites, Recents, Contacts) and the dialpad. It is D-Bus
 *    activatable: the launcher starts it, a tel: link opens it at the dialpad with the number, and the call service
 *    opens it at the call log ("show-call-log", a tap on a missed call) or to add a call ("add-call").
 *  - de.yesman.Dialer.InCall ("neo-dialer --incall"), the screen of a call: the answer screen while it rings, the
 *    in-call screen once it is up. The call service has the shell start it for a ringing call; it has a desktop
 *    file of its own so the shell lets it over the lock screen (InCallActivity's showWhenLocked).
 *
 * The calls themselves are not here: the call service (neo-dialer-calls) has them, rings, keeps the call log and
 * posts the notifications, whether or not this app runs. */
#include "call-types.h"
#include "dialer-window.h"
#include "incall-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>
#include <neo.h>

static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/de/yesman/Dialer/dialer.css");
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
dialer_window (GtkApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (app);

  return window ? GTK_WIDGET (window) : dialer_window_new (app, settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (dialer_window (GTK_APPLICATION (app))));
}

/* a tel: link: the dialpad opens with the number in it, to be called with the dial button (Intent.ACTION_DIAL) */
static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = dialer_window (GTK_APPLICATION (app));

  for (int i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);
      g_autofree char *number = NULL;

      if (!g_str_has_prefix (uri, "tel:"))
        continue;
      number = g_uri_unescape_string (uri + strlen ("tel:"), NULL);
      dialer_window_show_dialpad (window, number);
      break;
    }
  gtk_window_present (GTK_WINDOW (window));
}

/* the call service's "open the call log" (a tap on the missed-call notification) */
static void
on_show_call_log (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = dialer_window (GTK_APPLICATION (data));

  dialer_window_show_recents (window);
  gtk_window_present (GTK_WINDOW (window));
}

/* the in-call screen's Add call: the dialpad, for the second number */
static void
on_add_call (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWidget *window = dialer_window (GTK_APPLICATION (data));

  dialer_window_show_dialpad (window, NULL);
  gtk_window_present (GTK_WINDOW (window));
}

/* --- the in-call screen */

/* a volume key, which the shell passes on while the answer screen is in front (incall-window.c) */
static void
on_volume_key (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  incall_window_volume_key (g_variant_get_int32 (parameter));
}

static void
on_activate_incall (GApplication *app, gpointer data)
{
  incall_window_present (GTK_APPLICATION (app));
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;
  gboolean in_call = FALSE;
  int status;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* --incall is ours, not GApplication's: take it out before GApplication sees the arguments */
  for (int i = 1; i < argc; i++)
    if (g_str_equal (argv[i], "--incall"))
      {
        in_call = TRUE;
        memmove (&argv[i], &argv[i + 1], (argc - i) * sizeof *argv);
        argc--;
        break;
      }

  settings = g_settings_new (DIALER_APP_ID);
  app = gtk_application_new (in_call ? INCALL_APP_ID : DIALER_APP_ID,
                             in_call ? G_APPLICATION_DEFAULT_FLAGS : G_APPLICATION_HANDLES_OPEN);
  /* both find the icons and the style under the app's path (otherwise the InCall one would look in .../InCall) */
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/yesman/Dialer");
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  if (in_call)
    {
      static const GActionEntry actions[] = {
        { .name = "volume-key", .activate = on_volume_key, .parameter_type = "i" },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate_incall), NULL);
    }
  else
    {
      static const GActionEntry actions[] = {
        { .name = "show-call-log", .activate = on_show_call_log },
        { .name = "add-call", .activate = on_add_call },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
      g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
    }

  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
