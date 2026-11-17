/* vibeos-software: Software, VibeOS's app store.
 *
 * The catalog is the store at vibeos.top (API v1, sw-api.h); installing, updating and uninstalling are App
 * Installer's, through its D-Bus interface vibeos.Installer1 (sw-installer.h): Software never sees a password, polkit
 * asks for it in the shell. What is installed comes from dpkg's status file, read by Software itself.
 *
 * The environment, for tests and pictures:
 *
 *   VIBEOS_STORE_URL              the store (default https://vibeos.top): a local copy of the server for anything that
 *                                 writes (an account, reviews)
 *   VIBEOS_SOFTWARE_TOKEN_FILE    keep the account's token in this file (0600) instead of the login keyring
 *   VIBEOS_SOFTWARE_DPKG_STATUS   read the installed packages from this file instead of /var/lib/dpkg/status
 *   VIBEOS_SOFTWARE_PAGE          steps to walk once the window shows, ';' between them ("tab:apps;app:org.gnome.
 *                                 Maps"; sw-window.c has the list)
 *   VIBEOS_INSTALLER_DEMO         App Installer's canned states (its inst-demo.h), when it runs with it
 *   G_MESSAGES_DEBUG              vibeos-software-frames: how smoothly each run of frames went (sw-window.c);
 *                                 vibeos-page-stack: every frame of a page's transition (libvibeos)
 *
 * Closing the window while a job runs hides it: Software stays until App Installer reports the job's end, so that
 * the result is still told (and recorded) and the account's device list stays right. */
#include "sw-account.h"
#include "sw-device.h"
#include "sw-installer.h"
#include "sw-pages.h"
#include "sw-updates.h"
#include "sw-window.h"

#include <locale.h>
#include <vibeos.h>

#define SW_APP_ID "vibeos.Software"

static GSettings *settings;
static gboolean held;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/vibeos/Software/software.css");
  /* over libvibeos's, which is at APPLICATION priority too */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

/* Software's own light or dark, from its setting */
static void
apply_scheme (void)
{
  g_autofree char *scheme = settings ? g_settings_get_string (settings, "color-scheme") : NULL;

  vibeos_theme_set_scheme (vibeos_theme_get_default (),
                           !g_strcmp0 (scheme, "light") ? VIBEOS_SCHEME_LIGHT
                           : !g_strcmp0 (scheme, "dark") ? VIBEOS_SCHEME_DARK : VIBEOS_SCHEME_SYSTEM);
}

static void
on_setting_changed (GSettings *s, const char *key, gpointer data)
{
  if (!g_strcmp0 (key, "color-scheme"))
    apply_scheme ();
  else if (!g_strcmp0 (key, "update-mode"))
    /* a mode that allows more now: look at once */
    sw_updates_check (sw_updates_get_default (), FALSE);
}

/* the hidden window's jobs are done: it goes, and Software with it */
static void
on_jobs_changed (SwInstaller *installer, const char *package, GApplication *app)
{
  GtkWindow *window;

  if (!held || sw_installer_get_jobs (installer)->len)
    return;
  held = FALSE;
  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window && !gtk_widget_get_visible (GTK_WIDGET (window)))
    gtk_window_destroy (window);
  g_application_release (app);
}

static gboolean
on_close_request (GtkWindow *window, GApplication *app)
{
  if (!sw_installer_get_jobs (sw_installer_get_default ())->len || held)
    return FALSE;
  held = TRUE;
  g_application_hold (app);
  gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
  return TRUE;
}

static void
on_startup (GApplication *app, gpointer data)
{
  GSettingsSchema *schema;

  vibeos_init ();
  load_style ();
  schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), SW_APP_ID, TRUE);
  if (schema)
    {
      settings = g_settings_new (SW_APP_ID);
      g_signal_connect (settings, "changed", G_CALLBACK (on_setting_changed), NULL);
      g_settings_schema_unref (schema);
    }
  else
    g_warning ("the settings schema %s is not installed: the defaults stand", SW_APP_ID);
  apply_scheme ();
  sw_device_load (NULL, NULL);
  sw_installer_get_default ();
  sw_updates_get_default ();
  sw_account_load (sw_account_get_default ());
  g_signal_connect_object (sw_installer_get_default (), "changed", G_CALLBACK (on_jobs_changed), app, 0);
}

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
  const char *steps = g_getenv ("VIBEOS_SOFTWARE_PAGE");

  if (!window)
    {
      window = GTK_WINDOW (sw_window_new (GTK_APPLICATION (app)));
      g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), app);
      if (steps && *steps)
        sw_window_run_steps (GTK_WIDGET (window), steps);
    }
  /* a window hidden while its jobs finished: shown again, as it was */
  if (held)
    {
      held = FALSE;
      g_application_release (app);
    }
  gtk_window_present (window);
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = NULL;
  int status;

  setlocale (LC_ALL, "");
  app = gtk_application_new (SW_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
