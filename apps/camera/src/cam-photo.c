/* cam-photo: see cam-photo.h. */
#include "cam-photo.h"
#include "cam-constants.h"
#include "cam-devices.h"

#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <gst/video/video.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

void
cam_photo_free (CamPhoto *photo)
{
  if (!photo)
    return;
  g_clear_pointer (&photo->frame, gst_sample_unref);
  g_clear_pointer (&photo->time, g_date_time_unref);
  g_free (photo->path);
  g_free (photo);
}

/* ---- the picture ----------------------------------------------------------------------------------------------- */

static GdkPixbuf *
frame_to_pixbuf (GstSample *frame, GError **error)
{
  GstCaps *caps = gst_sample_get_caps (frame);
  GstVideoInfo info;
  g_autoptr(GstCaps) rgb = NULL;
  g_autoptr(GstSample) converted = NULL;
  GstVideoFrame video;
  GdkPixbuf *pixbuf;

  if (!caps || !gst_video_info_from_caps (&info, caps))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the frame has no picture format");
      return NULL;
    }
  /* whatever the camera gives (the soft ISP's RGBA, a UVC camera's YUY2, …) to 24-bit RGB */
  rgb = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT,
                             GST_VIDEO_INFO_WIDTH (&info), "height", G_TYPE_INT, GST_VIDEO_INFO_HEIGHT (&info),
                             "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
  converted = gst_video_convert_sample (frame, rgb, 5 * GST_SECOND, error);
  if (!converted)
    return NULL;
  gst_video_info_from_caps (&info, gst_sample_get_caps (converted));
  if (!gst_video_frame_map (&video, &info, gst_sample_get_buffer (converted), GST_MAP_READ))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read the frame");
      return NULL;
    }
  pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, GST_VIDEO_INFO_WIDTH (&info), GST_VIDEO_INFO_HEIGHT (&info));
  for (int y = 0; y < GST_VIDEO_INFO_HEIGHT (&info); y++)
    memcpy (gdk_pixbuf_get_pixels (pixbuf) + y * gdk_pixbuf_get_rowstride (pixbuf),
            (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&video, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE (&video, 0),
            GST_VIDEO_INFO_WIDTH (&info) * 3);
  gst_video_frame_unmap (&video);
  return pixbuf;
}

static GdkPixbuf *
compose (CamPhoto *photo, GError **error)
{
  g_autoptr(GdkPixbuf) pixbuf = frame_to_pixbuf (photo->frame, error);
  int w, h;

  if (!pixbuf)
    return NULL;
  w = gdk_pixbuf_get_width (pixbuf);
  h = gdk_pixbuf_get_height (pixbuf);
  if (photo->zoom > 1.001)
    {
      int cw = MAX (2, (int) round (w / photo->zoom)), ch = MAX (2, (int) round (h / photo->zoom));
      g_autoptr(GdkPixbuf) crop = gdk_pixbuf_new_subpixbuf (pixbuf, (w - cw) / 2, (h - ch) / 2, cw, ch);
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple (crop, w, h, GDK_INTERP_BILINEAR);

      g_object_unref (pixbuf);
      pixbuf = scaled;
    }
  if (photo->rotation % 360)
    {
      /* GdkPixbuf turns anticlockwise */
      GdkPixbuf *turned = gdk_pixbuf_rotate_simple (pixbuf, (360 - photo->rotation % 360) % 360);

      g_object_unref (pixbuf);
      pixbuf = turned;
    }
  if (photo->mirror)
    {
      GdkPixbuf *flipped = gdk_pixbuf_flip (pixbuf, TRUE);

      g_object_unref (pixbuf);
      pixbuf = flipped;
    }
  return g_steal_pointer (&pixbuf);
}

/* ---- EXIF: a little-endian TIFF with IFD0, the Exif IFD and the GPS IFD ----------------------------------------- */

enum { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5, T_UNDEFINED = 7 };

typedef struct {
  guint16 tag, type;
  guint32 count;
  guint8 *data;
  gsize   len;
} Entry;

typedef struct {
  GArray *entries;
} Ifd;

static void
entry_clear (gpointer data)
{
  g_free (((Entry *) data)->data);
}

static void
ifd_init (Ifd *ifd)
{
  ifd->entries = g_array_new (FALSE, TRUE, sizeof (Entry));
  g_array_set_clear_func (ifd->entries, entry_clear);
}

static void
ifd_add (Ifd *ifd, guint16 tag, guint16 type, guint32 count, const void *data, gsize len)
{
  Entry entry = { tag, type, count, g_memdup2 (data, len), len };

  g_array_append_val (ifd->entries, entry);
}

static void
put16 (guint8 *p, guint16 v)
{
  p[0] = v & 0xff;
  p[1] = v >> 8;
}

static void
put32 (guint8 *p, guint32 v)
{
  for (int i = 0; i < 4; i++)
    p[i] = (v >> (8 * i)) & 0xff;
}

static void
add_ascii (Ifd *ifd, guint16 tag, const char *text)
{
  ifd_add (ifd, tag, T_ASCII, strlen (text) + 1, text, strlen (text) + 1);
}

static void
add_short (Ifd *ifd, guint16 tag, guint16 value)
{
  guint8 b[2];

  put16 (b, value);
  ifd_add (ifd, tag, T_SHORT, 1, b, 2);
}

static void
add_long (Ifd *ifd, guint16 tag, guint32 value)
{
  guint8 b[4];

  put32 (b, value);
  ifd_add (ifd, tag, T_LONG, 1, b, 4);
}

static void
add_rationals (Ifd *ifd, guint16 tag, int n, const guint32 *pairs)
{
  guint8 b[8 * 3];

  for (int i = 0; i < 2 * n; i++)
    put32 (b + 4 * i, pairs[i]);
  ifd_add (ifd, tag, T_RATIONAL, n, b, 8 * n);
}

static void
add_undefined (Ifd *ifd, guint16 tag, const void *data, gsize len)
{
  ifd_add (ifd, tag, T_UNDEFINED, len, data, len);
}

static int
compare_tags (gconstpointer a, gconstpointer b)
{
  return (int) ((const Entry *) a)->tag - (int) ((const Entry *) b)->tag;
}

static gsize
ifd_size (Ifd *ifd)
{
  gsize size = 2 + 12 * ifd->entries->len + 4;

  for (guint i = 0; i < ifd->entries->len; i++)
    {
      Entry *e = &g_array_index (ifd->entries, Entry, i);
      if (e->len > 4)
        size += (e->len + 1) & ~(gsize) 1;
    }
  return size;
}

static void
ifd_set_long (Ifd *ifd, guint16 tag, guint32 value)
{
  for (guint i = 0; i < ifd->entries->len; i++)
    {
      Entry *e = &g_array_index (ifd->entries, Entry, i);
      if (e->tag == tag)
        put32 (e->data, value);
    }
}

/* `offset`: where this IFD starts, from the TIFF header */
static void
ifd_write (GByteArray *out, Ifd *ifd, guint32 offset)
{
  guint32 data_at = offset + 2 + 12 * ifd->entries->len + 4;
  gsize start = out->len;
  guint8 b[12];

  g_array_sort (ifd->entries, compare_tags);
  put16 (b, ifd->entries->len);
  g_byte_array_append (out, b, 2);
  for (guint i = 0; i < ifd->entries->len; i++)
    {
      Entry *e = &g_array_index (ifd->entries, Entry, i);

      memset (b, 0, sizeof b);
      put16 (b, e->tag);
      put16 (b + 2, e->type);
      put32 (b + 4, e->count);
      if (e->len <= 4)
        memcpy (b + 8, e->data, e->len);
      else
        {
          put32 (b + 8, data_at);
          data_at += (e->len + 1) & ~(gsize) 1;
        }
      g_byte_array_append (out, b, 12);
    }
  memset (b, 0, 4);
  g_byte_array_append (out, b, 4);      /* no next IFD */
  for (guint i = 0; i < ifd->entries->len; i++)
    {
      Entry *e = &g_array_index (ifd->entries, Entry, i);
      if (e->len > 4)
        {
          g_byte_array_append (out, e->data, e->len);
          if (e->len & 1)
            g_byte_array_append (out, (const guint8 *) "", 1);
        }
    }
  g_assert (out->len - start == ifd_size (ifd));
}

static void
dms (double degrees, guint32 *pairs)
{
  double a = fabs (degrees);
  int d = (int) a, m = (int) ((a - d) * 60);
  double s = (a - d - m / 60.0) * 3600;

  pairs[0] = d, pairs[1] = 1;
  pairs[2] = m, pairs[3] = 1;
  pairs[4] = (guint32) round (s * 10000), pairs[5] = 10000;
}

static GByteArray *
build_exif (CamPhoto *photo, int width, int height)
{
  Ifd ifd0, exif, gps;
  GByteArray *tiff = g_byte_array_new ();
  g_autofree char *stamp = g_date_time_format (photo->time, "%Y:%m:%d %H:%M:%S");
  g_autofree char *offset = g_date_time_format (photo->time, "%:z");
  g_autofree char *subsec = g_strdup_printf ("%03d", g_date_time_get_microsecond (photo->time) / 1000);
  static const guint32 dpi[] = { 72, 1 };
  guint8 header[8] = { 'I', 'I', 0x2a, 0, 8, 0, 0, 0 };
  gboolean with_gps = photo->location.valid;

  ifd_init (&ifd0);
  ifd_init (&exif);
  ifd_init (&gps);
  if (*cam_device_make ())
    add_ascii (&ifd0, 0x010f, cam_device_make ());
  if (*cam_device_model ())
    add_ascii (&ifd0, 0x0110, cam_device_model ());
  add_short (&ifd0, 0x0112, 1);                       /* Orientation: the pixels are upright */
  add_rationals (&ifd0, 0x011a, 1, dpi);
  add_rationals (&ifd0, 0x011b, 1, dpi);
  add_short (&ifd0, 0x0128, 2);                       /* inches */
  add_ascii (&ifd0, 0x0132, stamp);
  add_short (&ifd0, 0x0213, 1);                       /* YCbCr centred */
  add_long (&ifd0, 0x8769, 0);                        /* the Exif IFD, set below */
  if (with_gps)
    add_long (&ifd0, 0x8825, 0);

  if (photo->exposure_us > 0)
    {
      guint32 t[2];
      if (photo->exposure_us < 1000000)
        t[0] = 1, t[1] = (guint32) round (1e6 / photo->exposure_us);
      else
        t[0] = photo->exposure_us, t[1] = 1000000;
      add_rationals (&exif, 0x829a, 1, t);
    }
  /* libcamera's own Android HAL reports the sensitivity as analogue gain × 100 */
  if (photo->gain > 0)
    add_short (&exif, 0x8827, (guint16) CLAMP (round (photo->gain * 100), 1, 65535));
  add_undefined (&exif, 0x9000, "0232", 4);
  add_ascii (&exif, 0x9003, stamp);
  add_ascii (&exif, 0x9004, stamp);
  add_ascii (&exif, 0x9010, offset);
  add_ascii (&exif, 0x9011, offset);
  add_ascii (&exif, 0x9012, offset);
  add_undefined (&exif, 0x9101, "\1\2\3\0", 4);
  /* Flash: fired, not fired, or no flash function */
  add_short (&exif, 0x9209, !photo->has_flash ? 0x20 : photo->flash_fired ? 0x01 : 0x00);
  add_ascii (&exif, 0x9290, subsec);
  add_ascii (&exif, 0x9291, subsec);
  add_ascii (&exif, 0x9292, subsec);
  add_undefined (&exif, 0xa000, "0100", 4);
  add_short (&exif, 0xa001, 1);                       /* sRGB */
  add_long (&exif, 0xa002, width);
  add_long (&exif, 0xa003, height);

  if (with_gps)
    {
      g_autoptr(GDateTime) fix = g_date_time_new_from_unix_utc (photo->location.time);
      g_autofree char *date = g_date_time_format (fix, "%Y:%m:%d");
      guint32 lat[6], lon[6], when[6] = { g_date_time_get_hour (fix), 1, g_date_time_get_minute (fix), 1,
                                          g_date_time_get_second (fix), 1 };
      static const guint8 version[4] = { 2, 2, 0, 0 };

      dms (photo->location.latitude, lat);
      dms (photo->location.longitude, lon);
      ifd_add (&gps, 0x0000, T_BYTE, 4, version, 4);
      add_ascii (&gps, 0x0001, photo->location.latitude >= 0 ? "N" : "S");
      add_rationals (&gps, 0x0002, 3, lat);
      add_ascii (&gps, 0x0003, photo->location.longitude >= 0 ? "E" : "W");
      add_rationals (&gps, 0x0004, 3, lon);
      if (photo->location.altitude != -G_MAXDOUBLE)
        {
          guint8 below = photo->location.altitude < 0;
          guint32 alt[2] = { (guint32) round (fabs (photo->location.altitude) * 100), 100 };

          ifd_add (&gps, 0x0005, T_BYTE, 1, &below, 1);
          add_rationals (&gps, 0x0006, 1, alt);
        }
      add_rationals (&gps, 0x0007, 3, when);
      add_ascii (&gps, 0x001d, date);
    }

  guint32 exif_at = 8 + ifd_size (&ifd0);
  guint32 gps_at = exif_at + ifd_size (&exif);
  ifd_set_long (&ifd0, 0x8769, exif_at);
  if (with_gps)
    ifd_set_long (&ifd0, 0x8825, gps_at);
  g_byte_array_append (tiff, header, 8);
  ifd_write (tiff, &ifd0, 8);
  ifd_write (tiff, &exif, exif_at);
  if (with_gps)
    ifd_write (tiff, &gps, gps_at);
  g_array_unref (ifd0.entries);
  g_array_unref (exif.entries);
  g_array_unref (gps.entries);
  return tiff;
}

/* the JPEG with an APP1 Exif segment right after SOI, in place of GdkPixbuf's JFIF APP0 */
static GByteArray *
add_exif (const guint8 *jpeg, gsize len, GByteArray *tiff)
{
  GByteArray *out = g_byte_array_sized_new (len + tiff->len + 16);
  gsize rest = 2;
  guint8 app1[10] = { 0xff, 0xe1, 0, 0, 'E', 'x', 'i', 'f', 0, 0 };
  gsize segment = 2 + 6 + tiff->len;

  if (len < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || segment > 0xffff)
    {
      g_byte_array_append (out, jpeg, len);
      return out;
    }
  if (jpeg[2] == 0xff && jpeg[3] == 0xe0 && len > 6)
    rest = 4 + ((jpeg[4] << 8) | jpeg[5]);
  app1[2] = segment >> 8;
  app1[3] = segment & 0xff;
  g_byte_array_append (out, jpeg, 2);
  g_byte_array_append (out, app1, sizeof app1);
  g_byte_array_append (out, tiff->data, tiff->len);
  g_byte_array_append (out, jpeg + rest, len - rest);
  return out;
}

/* ---- saving ---------------------------------------------------------------------------------------------------- */

static gboolean
write_atomically (const char *path, const guint8 *data, gsize len, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  g_autofree char *pending = g_strdup_printf ("%s/.pending-%s", dir, base);
  int fd = g_open (pending, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  gsize done = 0;

  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", pending, g_strerror (errno));
      return FALSE;
    }
  while (done < len)
    {
      gssize n = write (fd, data + done, len - done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", pending, g_strerror (errno));
          close (fd);
          g_unlink (pending);
          return FALSE;
        }
      done += n;
    }
  fsync (fd);
  close (fd);
  if (g_rename (pending, path) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", path, g_strerror (errno));
      g_unlink (pending);
      return FALSE;
    }
  return TRUE;
}

static void
save_thread (GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
  CamPhoto *photo = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) pixbuf = compose (photo, &error);
  g_autofree char *jpeg = NULL;
  g_autofree char *quality = g_strdup_printf ("%d", CAM_JPEG_QUALITY);
  gsize len = 0;
  GByteArray *tiff, *file;
  gboolean ok;

  if (!pixbuf || !gdk_pixbuf_save_to_buffer (pixbuf, &jpeg, &len, "jpeg", &error, "quality", quality, NULL))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  tiff = build_exif (photo, gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf));
  file = add_exif ((guint8 *) jpeg, len, tiff);
  ok = write_atomically (photo->path, file->data, file->len, &error);
  g_byte_array_unref (tiff);
  g_byte_array_unref (file);
  if (!ok)
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_pointer (task, g_strdup (photo->path), g_free);
}

void
cam_photo_save_async (CamPhoto *photo, GCancellable *cancel, GAsyncReadyCallback done, gpointer data)
{
  g_autoptr(GTask) task = g_task_new (NULL, cancel, done, data);

  g_task_set_task_data (task, photo, (GDestroyNotify) cam_photo_free);
  g_task_run_in_thread (task, save_thread);
}

char *
cam_photo_save_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
