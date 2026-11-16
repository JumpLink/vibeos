/* vibeos-dialer: the Phone app, LineageOS's Dialer rebuilt in GTK 4.
 *
 * One program, two applications, as the Clock has them:
 *  - vibeos.Dialer, the app with its tabs (Favorites, Recents, Contacts) and the dialpad. It is D-Bus
 *    activatable: the launcher starts it, a tel: link opens it at the dialpad with the number, another app has a
 *    number called at once ("call", below), and the call service opens it at the call log ("show-call-log", a tap
 *    on a missed call) or to add a call ("add-call").
 *  - vibeos.Dialer.InCall ("vibeos-dialer --incall"), the screen of a call: the answer screen while it rings, the
 *    in-call screen once it is up. The call service has the shell start it for a ringing call; it has a desktop
 *    file of its own so the shell lets it over the lock screen.
 *
 * The calls themselves are not here: the call service (vibeos-dialer-calls) has them, rings, keeps the call log and
 * posts the notifications, whether or not this app runs. */
#include "call-types.h"
#include "calls-client.h"
#include "dialer-window.h"
#include "incall-window.h"
#include "phone.h"
#include "phone-number.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <string.h>
#include <vibeos.h>

static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/vibeos/Dialer/dialer.css");
  /* over libvibeos's, which is at APPLICATION priority too: the later provider wins at equal priority */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  vibeos_init ();
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

/* The number in a tel: URI. A link opened through GIO comes as its GFile's URI, tel:///+49… for tel:+49… (an
 * empty authority), and the slashes are no part of the number. */
static char *
tel_number (const char *uri)
{
  const char *number = uri + strlen ("tel:");

  while (*number == '/')
    number++;
  return g_uri_unescape_string (number, NULL);
}

/* a tel: link: the dialpad opens with the number in it, to be called with the dial button */
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
      number = tel_number (uri);
      dialer_window_show_dialpad (window, number);
      break;
    }
  gtk_window_present (GTK_WINDOW (window));
}

/* "call" (s): another app's call to a number (the Contacts app's call button), activated with
 * org.freedesktop.Application.ActivateAction on vibeos.Dialer, or `gapplication action vibeos.Dialer call
 * "'+49…'"`. The number may come as a tel: URI. The call goes out at once and the in-call screen comes up, no window
 * of the Dialer's in between. Except:
 *  - an emergency number is not called this way: the dialpad opens with it, to be called with the dial button;
 *  - no number: the dialpad, empty;
 *  - a call the call service refuses: the dialpad with the number, and why in the snackbar. */
typedef struct {
  GApplication *app;
  char *number;
} CallRequest;

static void
on_called (guint id, const GError *error, gpointer data)
{
  CallRequest *request = data;

  if (error)
    {
      GtkWidget *window = dialer_window (GTK_APPLICATION (request->app));

      dialer_window_show_dialpad (window, request->number);
      gtk_window_present (GTK_WINDOW (window));
      dialer_window_show_message (window, error->message);
    }
  else
    phone_start_app (INCALL_APP_ID);
  g_application_release (request->app);
  g_free (request->number);
  g_free (request);
}

static void
on_call (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GApplication *app = data;
  const char *given = g_variant_get_string (parameter, NULL);
  g_autofree char *number = g_str_has_prefix (given, "tel:") ? tel_number (given) : g_strdup (given);
  CallRequest *request;

  g_strstrip (number);
  if (!*number || phone_number_is_emergency (number))
    {
      GtkWidget *window = dialer_window (GTK_APPLICATION (app));

      dialer_window_show_dialpad (window, number);
      gtk_window_present (GTK_WINDOW (window));
      return;
    }
  /* kept running until the call service has answered, though no window of ours is open */
  request = g_new0 (CallRequest, 1);
  request->app = app;
  request->number = g_steal_pointer (&number);
  g_application_hold (app);
  calls_client_dial (calls_client_get (), request->number, on_called, request);
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
  g_application_set_resource_base_path (G_APPLICATION (app), "/vibeos/Dialer");
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
        { .name = "call", .activate = on_call, .parameter_type = "s" },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
      g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
      g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
    }

  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
