/* vibeos-browser: the Browser, LineageOS's Jelly rebuilt over WebKitGTK.
 *
 *   vibeos-browser                 the most recent tab (the launcher's tap), or the saved ones on a cold start
 *   vibeos-browser URL…            each URL in a new tab (a link from another app)
 *   vibeos-browser --incognito     a new incognito tab (the launcher's long-press "New incognito tab")
 *   vibeos-browser --shortcut=ID URL   a home-screen shortcut (Add shortcut)
 *
 * One process holds every tab; a second start hands its command line to it. */
#include "nb.h"

#include <gst/gst.h>
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

/* Web video decodes in software. A phone's V4L2 video decoders (stateful, or stateless from v4l2codecs) rank above
 * FFmpeg's and libvpx's, so WebKit picked them, and their frames failed in WebKit's GL video sink. On the S10e
 * (Exynos MFC, Mali on Panfrost) a frame whose rows are not aligned as Panfrost wants, a 540 px wide video's, is not
 * imported into GL ("WSI pitch not properly aligned"): the video played black, the poster gone. And after a loop or
 * a seek the decoder only ended the stream again, so a looping video stopped at 0. The web processes read
 * GST_PLUGIN_FEATURE_RANK at their start: every V4L2 video decoder in the registry (one per codec and device) goes in
 * as NAME:NONE, ahead of what the environment already says, which then still wins. GStreamer's start here takes
 * ~13 ms on the S10e. */
static void
web_video_in_software (void)
{
  if (!gst_init_check (NULL, NULL, NULL))
    return;
  GString *ranks = g_string_new (NULL);
  static const char *const plugins[] = { "video4linux2", "v4l2codecs" };
  for (guint i = 0; i < G_N_ELEMENTS (plugins); i++) {
    GList *features = gst_registry_get_feature_list_by_plugin (gst_registry_get (), plugins[i]);
    for (GList *l = features; l; l = l->next)
      if (GST_IS_ELEMENT_FACTORY (l->data) &&
          gst_element_factory_list_is_type (GST_ELEMENT_FACTORY (l->data),
                                            GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO))
        g_string_append_printf (ranks, "%s%s:NONE", ranks->len ? "," : "",
                                gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (l->data)));
    gst_plugin_feature_list_free (features);
  }
  const char *env = g_getenv ("GST_PLUGIN_FEATURE_RANK");
  if (ranks->len && env && *env)
    g_string_append_printf (ranks, ",%s", env);
  if (ranks->len)
    g_setenv ("GST_PLUGIN_FEATURE_RANK", ranks->str, TRUE);
  g_string_free (ranks, TRUE);
}

int
main (int argc, char **argv)
{
  /* before anything starts a thread or a web process */
  web_video_in_software ();
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
