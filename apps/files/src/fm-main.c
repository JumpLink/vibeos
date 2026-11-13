/* neo-files: the Files app, File Manager+ (com.alphainventor.filemanager 3.3.6) rebuilt in GTK 4 for any Mobian phone
 *, on GIO and GVfs: local storage, removable media, the Recycle Bin, Recent files, the Local Network
 * and saved remotes over SMB, SFTP, FTP/FTPS and WebDAV.
 *
 * neo.Files is D-Bus activatable. A folder's URI opened with it (`neo-files URI`, or Open over D-Bus from
 * another app) browses that folder; a file's URI browses the folder it is in. */
#include "fm-share.h"
#include "fm-window.h"

#include <glib/gi18n.h>
#include <locale.h>
#include <neo.h>

#define FM_APP_ID "neo.Files"

static GSettings *settings;

static void
load_style (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_resource (provider, "/neo/Files/files.css");
  /* over libneo's, which is at APPLICATION priority too */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref (provider);
}

static void
on_startup (GApplication *app, gpointer data)
{
  neo_init ();
  load_style ();
  settings = g_settings_new (FM_APP_ID);
  fm_share_init ();
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  g_clear_object (&settings);
}

/* the one window, made on the first need */
static GtkWidget *
files_window (GApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  return window ? GTK_WIDGET (window) : fm_window_new (GTK_APPLICATION (app), settings);
}

static void
on_activate (GApplication *app, gpointer data)
{
  gtk_window_present (GTK_WINDOW (files_window (app)));
}

static void
on_type (GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWidget *window = data;
  FmShell *shell = g_object_get_data (G_OBJECT (window), "fm-shell");
  g_autoptr(GFileInfo) info = g_file_query_info_finish (G_FILE (source), result, NULL);
  GFile *file = G_FILE (source);

  if (shell && info && g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr(GFile) parent = g_file_get_parent (file);

      if (parent)
        fm_shell_open_location (shell, parent);
    }
  else if (shell)
    /* a folder, or a server's share or host that is not mounted yet (the browser mounts it) */
    fm_shell_open_location (shell, file);
  g_object_unref (window);
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint, gpointer data)
{
  GtkWidget *window = files_window (app);

  gtk_window_present (GTK_WINDOW (window));
  /* one window shows one folder: the last one asked for */
  if (n_files > 0)
    g_file_query_info_async (files[n_files - 1], G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE,
                             G_PRIORITY_DEFAULT, NULL, on_type, g_object_ref (window));
}

int
main (int argc, char **argv)
{
  g_autoptr(GtkApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain ("neo-files", LOCALEDIR);
  bind_textdomain_codeset ("neo-files", "UTF-8");
  textdomain ("neo-files");
  g_set_application_name (_("Files"));
  app = gtk_application_new (FM_APP_ID, G_APPLICATION_HANDLES_OPEN);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
