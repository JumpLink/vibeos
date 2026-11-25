/* vibeos-camera: the Camera, LineageOS 23.2's Aperture rebuilt in GTK 4 on libcamera and GStreamer.
 *
 * Photos are the live stream's current frame (about 2 MP, the sizes libcamera lists), saved as JPEG with EXIF to
 * ~/Pictures/Camera; videos are H.264 + AAC MP4 in ~/Videos/Camera; SCAN reads barcodes with zxing. Nothing here
 * knows a phone: the cameras, their facing and rotation come from libcamera, the flash from the LED class, the
 * encoder from a measurement (cam-encoder.h). The app's settings are GSettings vibeos.Camera.
 *
 * `vibeos-camera --probe-encoder ELEMENT` is the encoder measurement's child process (cam-encoder.h).
 *
 * Nothing of GStreamer runs here: gst_init may wait on a driver, so the window comes first and the start follows on
 * a thread of its own (cam-startup.h). */
#include "cam-constants.h"
#include "cam-encoder.h"
#include "cam-shutdown.h"
#include "cam-window.h"

#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>
#include <vibeos.h>
#include <pango/pangocairo.h>
#include <spongy.h>

#define APP_ID "vibeos.Camera"

static const char *
data_dir (void)
{
  const char *dir = g_getenv ("VIBEOS_CAMERA_DATADIR");

  return dir ? dir : DATADIR;
}

/* Droid Sans Mono, the mode and pro-row labels' font: shipped with the app (Apache-2.0), not in Debian */
static void
load_font (void)
{
  g_autofree char *path = g_build_filename (data_dir (), "fonts", "DroidSansMono.ttf", NULL);
  g_autoptr(GError) error = NULL;

  if (!pango_font_map_add_font_file (pango_cairo_font_map_get_default (), path, &error))
    g_warning ("camera: %s: %s", path, error->message);
}

static void
on_startup (GApplication *app, gpointer data)
{
  /* dark in light mode too, as the reference phone's camera; the text follows Font size and Bold text, the
   * stylesheet in sp and dp over libvibeos's */
  vibeos_init_full (VIBEOS_INIT_ALWAYS_DARK | VIBEOS_INIT_SCALED_TEXT);
  load_font ();
  vibeos_css_load_resource ("/vibeos/Camera/camera.css", GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  spongy_set_density (1.0);
}

static void
on_activate (GApplication *app, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (window)
    cam_window_activated (window);
  else
    window = cam_window_new (GTK_APPLICATION (app));
  gtk_window_present (window);
}

/* SIGTERM (the session ending) and SIGINT close the windows as a close would: a recording is finished (the app is
 * held until its file is whole) and the flash LED goes off */
static gboolean
on_terminate (gpointer data)
{
  GList *windows = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (data)));

  for (GList *l = windows; l; l = l->next)
    gtk_window_destroy (l->data);
  g_list_free (windows);
  return G_SOURCE_CONTINUE;
}

/* the windows are gone, and the camera may still be closing in the pipeline's worker (cam-shutdown.h): the process
 * ends after it. Here, before GTK's own shutdown, the main context is still the application's and GDK still up. */
static void
on_shutdown (GApplication *app, gpointer data)
{
  if (!cam_shutdown_wait (CAM_MS_CLOSE_WAIT))
    g_warning ("camera: still closing after %d ms, ending all the same", CAM_MS_CLOSE_WAIT);
}

/* the shell's volume keys (extension/service.js TakeVolumeKeys): +1 up, -1 down, on the press */
static void
on_volume_key (GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (data));

  if (window)
    cam_window_volume_key (window, g_variant_get_int32 (parameter));
}

int
main (int argc, char **argv)
{
  static const GActionEntry actions[] = {
    { .name = "volume-key", .activate = on_volume_key, .parameter_type = "i" },
  };
  g_autoptr(GtkApplication) app = NULL;

  if (argc == 3 && g_str_equal (argv[1], "--probe-encoder"))
    return cam_encoder_probe_main (argv[2]);

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_action_map_add_action_entries (G_ACTION_MAP (app), actions, G_N_ELEMENTS (actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  g_unix_signal_add (SIGTERM, on_terminate, app);
  g_unix_signal_add (SIGINT, on_terminate, app);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
