/* vibeos-terminal: the Terminal, a command line for a phone's touchscreen, on GNOME's terminal widget (VTE).
 *
 * A window runs the user's shell on a pseudo-terminal of its own (term-session.h) in a VTE that the app gives its
 * touches to (term-view.h): a tap brings the keyboard, a long press selects, two fingers scroll the history or, for a
 * program on the alternate screen or one that tracks the mouse, send what a mouse wheel would. The window keeps its
 * size when the keyboard comes up: the keys go over it (X-Vibeos-Keyboard=overlay in its desktop file) and the window
 * makes room itself (term-layout.h), so the program's lines are not reflowed. */
#include "term-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <spongy.h>
#include <vibeos.h>

#define APP_ID "vibeos.Terminal"

static void
on_startup (GApplication *app, gpointer data)
{
  /* the text follows Font size and Bold text; the stylesheet in sp and dp, over libvibeos's */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_resource ("/vibeos/Terminal/terminal.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  /* the fingers' travel counts in logical pixels, which are dp */
  spongy_set_density (1.0);
}

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (!window)
    window = GTK_WINDOW (term_window_new (GTK_APPLICATION (app)));
  gtk_window_present (window);
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
