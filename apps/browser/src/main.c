/* vibeos-browser: the Browser, LineageOS's Jelly rebuilt over WebKitGTK.
 *
 *   vibeos-browser                 the most recent tab (the launcher's tap), or the saved ones on a cold start
 *   vibeos-browser URL…            each URL in a new tab (a link from another app)
 *   vibeos-browser --incognito     a new incognito tab (the launcher's long-press "New incognito tab")
 *   vibeos-browser --shortcut=ID URL   a home-screen shortcut (Add shortcut)
 *
 * One process holds every tab; a second start hands its command line to it. */
#include "nb.h"

#include <vibeos.h>
#include <string.h>

static void
on_startup (GApplication *app, gpointer data)
{
  /* the text follows Font size and Bold text; the stylesheet in sp and dp, over libvibeos's */
  vibeos_init_full (VIBEOS_INIT_SCALED_TEXT);
  vibeos_css_load_resource ("/vibeos/Browser/browser.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  nb_app_startup (GTK_APPLICATION (app));
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  nb_app_shutdown ();
}

static void
on_activate (GApplication *app, gpointer data)
{
  nb_app_activate ();
}

/* an argument as a URL: a URI as it is, a file that exists as its file: URI, anything else through nb_smart_url */
static char *
arg_to_url (GApplicationCommandLine *cmd, const char *arg)
{
  g_autofree char *scheme = g_uri_parse_scheme (arg);
  if (scheme)
    return g_strdup (arg);
  g_autoptr (GFile) file = g_application_command_line_create_file_for_arg (cmd, arg);
  if (g_file_query_exists (file, NULL))
    return g_file_get_uri (file);
  return nb_smart_url (arg);
}

static int
on_command_line (GApplication *app, GApplicationCommandLine *cmd, gpointer data)
{
  GVariantDict *options = g_application_command_line_get_options_dict (cmd);
  gboolean incognito = FALSE;
  const char *shortcut = NULL;
  g_autofree const char **args = NULL;

  g_variant_dict_lookup (options, "incognito", "b", &incognito);
  g_variant_dict_lookup (options, "shortcut", "&s", &shortcut);
  g_variant_dict_lookup (options, G_OPTION_REMAINING, "^a&ay", &args);

  if (!incognito && !shortcut && (!args || !args[0])) {
    nb_app_activate ();
    return 0;
  }
  g_autoptr (GPtrArray) urls = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; args && args[i]; i++) {
    char *url = arg_to_url (cmd, args[i]);
    if (url)
      g_ptr_array_add (urls, url);
  }
  g_ptr_array_add (urls, NULL);
  nb_app_open ((const char *const *) urls->pdata, incognito, shortcut);
  return 0;
}

int
main (int argc, char **argv)
{
  g_autoptr (GtkApplication) app = gtk_application_new (NB_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
  static const GOptionEntry entries[] = {
    { "incognito", 0, 0, G_OPTION_ARG_NONE, NULL, "Open an incognito tab", NULL },
    { "shortcut", 0, 0, G_OPTION_ARG_STRING, NULL, "Open a home-screen shortcut", "ID" },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL, NULL, "URL…" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };

  g_application_add_main_option_entries (G_APPLICATION (app), entries);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "command-line", G_CALLBACK (on_command_line), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
