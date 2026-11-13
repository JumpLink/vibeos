/* neo-calculator: the Calculator, LineageOS 23.2's ExactCalculator rebuilt in GTK 4 (spec/calculator.md).
 *
 * Exact arithmetic as the original's: numbers are an exact fraction times a constructive real (unified-real.h),
 * so 1÷3×3 is 1, 0.1+0.2 is 0.3 and sin(π) is 0, and an irrational result has as many digits as one scrolls to.
 * The history and the memory are kept in a database under the user's data directory (expression-db.c), the angle
 * mode and the memory's entries in GSettings (neo.Calculator). */
#include "calc-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <neo.h>
#include <spongy.h>

#define APP_ID "neo.Calculator"

static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/neo/Calculator/calculator.css");
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
  /* the formula's and the results' drags count in dp, which are the logical pixels here: spongy's density is
   * logical pixels per dp (a SpongyScrollView sets it from the monitor once it shows, the history's) */
  spongy_set_density (1.0);
}

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (!window)
    window = GTK_WINDOW (calc_window_new (GTK_APPLICATION (app), settings));
  gtk_window_present (window);
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = NULL;
  int status;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  settings = g_settings_new (APP_ID);
  app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&settings);
  return status;
}
