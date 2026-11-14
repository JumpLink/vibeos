/* neo-dictate: Dictate, speech to text on the phone itself.
 *
 * A start button and a document: what is said appears in the document as it is said, recognised on the phone by
 * Moonshine (dictate-engine.c), with nothing sent anywhere. The document is plain text, edited as any other. */
#include "dictate-window.h"

#include <glib/gi18n.h>
#include <gst/gst.h>
#include <locale.h>
#include <neo.h>

#define APP_ID "neo.Dictate"

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/neo/Dictate/dictate.css");
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

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (!window)
    window = GTK_WINDOW (dictate_window_new (GTK_APPLICATION (app)));
  gtk_window_present (window);
}

/* "dictate": the FAB's start and stop, for other programs over D-Bus (org.gtk.Actions on /neo/Dictate) */
static void
on_dictate (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkApplication *app = data;
  GtkWindow *window = gtk_application_get_active_window (app);

  if (!window)
    window = GTK_WINDOW (dictate_window_new (app));
  gtk_window_present (window);
  dictate_window_toggle_dictation (GTK_WIDGET (window));
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* Moonshine's ONNX Runtime on one thread: on both reference phones that recognises faster than on all eight
   * cores (0.22 against 0.32 s of work per second of speech on the S10e), and leaves the others idle. Set before
   * any thread starts. */
  g_setenv ("MOONSHINE_ORT_SINGLE_THREAD", "1", FALSE);
  gst_init (NULL, NULL);

  app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  {
    static const GActionEntry actions[] = {
      { .name = "dictate", .activate = on_dictate },
    };

    g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  }
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
