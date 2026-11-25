/* cam-storage: see cam-storage.h. */
#include "cam-storage.h"
#include "cam-startup.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>

struct _CamStorage {
  GObject       parent;
  char         *dirs[2];        /* photos, videos */
  GFileMonitor *monitors[2];
  GRegex       *names;
  char         *newest;
  guint         rescan;
};

G_DEFINE_FINAL_TYPE (CamStorage, cam_storage, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static char *
camera_dir (GUserDirectory kind)
{
  const char *base = g_get_user_special_dir (kind);

  /* no user-dirs entry: the XDG default */
  if (!base)
    return g_build_filename (g_get_home_dir (), kind == G_USER_DIRECTORY_VIDEOS ? "Videos" : "Pictures", "Camera",
                             NULL);
  return g_build_filename (base, "Camera", NULL);
}

static void
scan (CamStorage *self)
{
  g_autofree char *best = NULL;
  gint64 best_time = 0;

  for (int i = 0; i < 2; i++)
    {
      g_autoptr(GDir) dir = g_dir_open (self->dirs[i], 0, NULL);
      const char *name;

      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *path = NULL;
          GStatBuf st;
          gint64 time;

          if (!g_regex_match (self->names, name, 0, NULL))
            continue;
          path = g_build_filename (self->dirs[i], name, NULL);
          if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode))
            continue;
          time = (gint64) st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
          if (!best || time > best_time || (time == best_time && g_strcmp0 (name, best) > 0))
            {
              g_free (best);
              best = g_steal_pointer (&path);
              best_time = time;
            }
        }
    }
  if (g_strcmp0 (best, self->newest))
    {
      g_free (self->newest);
      self->newest = g_steal_pointer (&best);
      g_signal_emit (self, signals[SIG_CHANGED], 0);
    }
}

static gboolean
on_rescan (gpointer data)
{
  CamStorage *self = data;

  self->rescan = 0;
  scan (self);
  return G_SOURCE_REMOVE;
}

static void
on_dir_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  CamStorage *self = data;

  if (event == G_FILE_MONITOR_EVENT_CHANGED || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    return;
  if (!self->rescan)
    self->rescan = g_timeout_add (100, on_rescan, self);
}

static void
cam_storage_dispose (GObject *object)
{
  CamStorage *self = CAM_STORAGE (object);

  g_clear_handle_id (&self->rescan, g_source_remove);
  for (int i = 0; i < 2; i++)
    {
      g_clear_object (&self->monitors[i]);
      g_clear_pointer (&self->dirs[i], g_free);
    }
  g_clear_pointer (&self->names, g_regex_unref);
  g_clear_pointer (&self->newest, g_free);
  G_OBJECT_CLASS (cam_storage_parent_class)->dispose (object);
}

static void
cam_storage_class_init (CamStorageClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = cam_storage_dispose;
  signals[SIG_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 0);
}

static void
cam_storage_init (CamStorage *self)
{
  self->dirs[0] = camera_dir (G_USER_DIRECTORY_PICTURES);
  self->dirs[1] = camera_dir (G_USER_DIRECTORY_VIDEOS);
  /* yyyy-MM-dd-HH-mm-ss-SSS, and -n for a second capture in one millisecond */
  self->names = g_regex_new ("^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{3}(-[0-9]+)?\\.(jpg|mp4)$",
                             G_REGEX_OPTIMIZE, 0, NULL);
  for (int i = 0; i < 2; i++)
    {
      g_autoptr(GFile) dir = NULL;

      g_mkdir_with_parents (self->dirs[i], 0755);
      dir = g_file_new_for_path (self->dirs[i]);
      self->monitors[i] = g_file_monitor_directory (dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
      if (self->monitors[i])
        g_signal_connect (self->monitors[i], "changed", G_CALLBACK (on_dir_changed), self);
    }
  scan (self);
}

CamStorage *
cam_storage_new (void)
{
  return g_object_new (CAM_TYPE_STORAGE, NULL);
}

char *
cam_storage_new_path (CamStorage *self, gboolean video, GDateTime *time)
{
  g_autofree char *stamp = g_date_time_format (time, "%Y-%m-%d-%H-%M-%S");
  const char *ext = video ? "mp4" : "jpg";
  const char *dir = self->dirs[video ? 1 : 0];

  g_mkdir_with_parents (dir, 0755);
  for (int n = 0;; n++)
    {
      g_autofree char *name = n ? g_strdup_printf ("%s-%03d-%d.%s", stamp, g_date_time_get_microsecond (time) / 1000,
                                                   n, ext)
                                : g_strdup_printf ("%s-%03d.%s", stamp, g_date_time_get_microsecond (time) / 1000,
                                                   ext);
      g_autofree char *pending_name = g_strdup_printf (".pending-%s", name);
      g_autofree char *path = g_build_filename (dir, name, NULL);
      g_autofree char *pending = g_build_filename (dir, pending_name, NULL);

      if (!g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (pending, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&path);
    }
}

const char *
cam_storage_get_newest (CamStorage *self)
{
  return self->newest;
}

/* ---- thumbnails ------------------------------------------------------------------------------------------------ */

/* a single frame does not need the phone's hardware decoder, and a software one cannot wedge it */
static int
on_autoplug_select (GstElement *bin, GstPad *pad, GstCaps *caps, GstElementFactory *factory, gpointer data)
{
  const char *klass = gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS);

  return klass && strstr (klass, "Hardware") ? 2 /* GST_AUTOPLUG_SELECT_SKIP */ : 0 /* TRY */;
}

static GdkPixbuf *
video_first_frame (const char *path, GError **error)
{
  g_autofree char *quoted = g_strescape (path, NULL);
  /* turned upright by the track's rotation (mp4mux's matrix, which qtdemux gives back as image-orientation) */
  g_autofree char *launch = g_strdup_printf ("filesrc location=\"%s\" ! decodebin name=decode ! videoconvert "
                                             "! videoflip video-direction=auto ! videoconvertscale "
                                             "! video/x-raw,format=RGB,pixel-aspect-ratio=1/1 "
                                             "! appsink name=sink sync=false", quoted);
  g_autoptr(GstElement) pipeline = NULL;
  g_autoptr(GstElement) decode = NULL;
  g_autoptr(GstElement) sink = NULL;
  g_autoptr(GstSample) sample = NULL;
  GstVideoInfo info;
  GstVideoFrame frame;
  GdkPixbuf *pixbuf = NULL;

  pipeline = gst_parse_launch (launch, error);
  if (!pipeline)
    return NULL;
  decode = gst_bin_get_by_name (GST_BIN (pipeline), "decode");
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  g_signal_connect (decode, "autoplug-select", G_CALLBACK (on_autoplug_select), NULL);
  gst_element_set_state (pipeline, GST_STATE_PAUSED);
  sample = gst_app_sink_try_pull_preroll (GST_APP_SINK (sink), 5 * GST_SECOND);
  if (sample && gst_video_info_from_caps (&info, gst_sample_get_caps (sample))
      && gst_video_frame_map (&frame, &info, gst_sample_get_buffer (sample), GST_MAP_READ))
    {
      pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, info.width, info.height);
      for (int y = 0; y < info.height; y++)
        memcpy (gdk_pixbuf_get_pixels (pixbuf) + y * gdk_pixbuf_get_rowstride (pixbuf),
                (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&frame, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0),
                info.width * 3);
      gst_video_frame_unmap (&frame);
    }
  gst_element_set_state (pipeline, GST_STATE_NULL);
  if (!pixbuf)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no frame in %s", path);
  return pixbuf;
}

static GdkPixbuf *
cover_square (GdkPixbuf *source, int size)
{
  int w = gdk_pixbuf_get_width (source), h = gdk_pixbuf_get_height (source);
  double scale = (double) size / MIN (w, h);
  int sw = MAX (size, (int) (w * scale + 0.5)), sh = MAX (size, (int) (h * scale + 0.5));
  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple (source, sw, sh, GDK_INTERP_BILINEAR);
  g_autoptr(GdkPixbuf) square = gdk_pixbuf_new_subpixbuf (scaled, (sw - size) / 2, (sh - size) / 2, size, size);

  return gdk_pixbuf_copy (square);
}

static void
thumbnail_thread (GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
  const char *path = data;
  int size = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "size"));
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) source = NULL;
  g_autoptr(GdkPixbuf) square = NULL;
  g_autoptr(GBytes) bytes = NULL;
  int w = 0, h = 0;

  if (g_str_has_suffix (path, ".mp4"))
    {
      /* a video's picture is GStreamer's; the thumbnail is asked for again when it has started */
      if (cam_gst_ready ())
        source = video_first_frame (path, &error);
      else
        g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING, "GStreamer has not started yet");
    }
  else if (gdk_pixbuf_get_file_info (path, &w, &h))
    {
      /* decoded at the size it is shown, as Coil does */
      g_autoptr(GdkPixbuf) loaded = gdk_pixbuf_new_from_file_at_scale (path, w < h ? size : -1, w < h ? -1 : size,
                                                                       TRUE, &error);
      if (loaded)
        source = gdk_pixbuf_apply_embedded_orientation (loaded);
    }
  else
    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a picture: %s", path);
  if (!source)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  square = cover_square (source, size);
  bytes = g_bytes_new (gdk_pixbuf_get_pixels (square), (gsize) gdk_pixbuf_get_rowstride (square) * (size - 1)
                                                       + size * gdk_pixbuf_get_n_channels (square));
  g_task_return_pointer (task,
                         gdk_memory_texture_new (size, size, gdk_pixbuf_get_has_alpha (square)
                                                               ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                 bytes, gdk_pixbuf_get_rowstride (square)),
                         g_object_unref);
}

void
cam_storage_thumbnail_async (const char *path, int size, GCancellable *cancel, GAsyncReadyCallback done,
                             gpointer data)
{
  g_autoptr(GTask) task = g_task_new (NULL, cancel, done, data);

  g_object_set_data (G_OBJECT (task), "size", GINT_TO_POINTER (size));
  g_task_set_task_data (task, g_strdup (path), g_free);
  g_task_run_in_thread (task, thumbnail_thread);
}

GdkTexture *
cam_storage_thumbnail_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- the gallery ----------------------------------------------------------------------------------------------- */

gboolean
cam_storage_open_gallery (CamStorage *self, GdkAppLaunchContext *context)
{
  if (self->newest)
    {
      g_autoptr(GFile) file = g_file_new_for_path (self->newest);
      const char *type = g_str_has_suffix (self->newest, ".mp4") ? "video/mp4" : "image/jpeg";
      g_autoptr(GAppInfo) app = g_app_info_get_default_for_type (type, FALSE);
      GList files = { file, NULL, NULL };

      return app && g_app_info_launch (app, &files, G_APP_LAUNCH_CONTEXT (context), NULL);
    }
  else
    {
      /* nothing captured yet: the gallery itself (the default app for image/jpeg) */
      g_autoptr(GAppInfo) app = g_app_info_get_default_for_type ("image/jpeg", FALSE);

      return app && g_app_info_launch (app, NULL, G_APP_LAUNCH_CONTEXT (context), NULL);
    }
}
