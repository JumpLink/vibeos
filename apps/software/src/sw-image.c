/* SwImage: see sw-image.h */
#include "sw-image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <spongy-gtk.h>
#include <string.h>
#include <vibeos.h>

#define MAX_ACTIVE    6
#define MEMORY_LIMIT  400          /* pictures kept in memory, as decoded; half go when it is reached */
#define MEMORY_BYTES  (160 << 20)  /* or when they take this much */
#define SIZED_BYTES   (64 << 20)   /* pictures made smaller for where they are drawn, at most */

/* ---- the loader: one per process ------------------------------------------------------------------------------- */

typedef struct {
  char   *url;
  GList  *waiters;     /* SwImage */
  gboolean started;
} Pending;

static GHashTable *memory;        /* url -> GdkTexture */
static GQueue     *memory_order;  /* urls, oldest first */
static gsize       memory_bytes;
static GHashTable *sized;         /* "WxH url" -> GdkTexture: a picture at the size it is drawn, in screen pixels */
static GQueue     *sized_order;   /* keys, least recently used first */
static gsize       sized_bytes;
static GHashTable *pending;       /* url -> Pending */
static GQueue     *queue;         /* Pending, not started */
static int         active;
static SoupSession *session;
static char       *cache_dir;

static void pump (void);
static void image_set_texture (SwImage *self, GdkTexture *texture, gboolean failed);

static void
loader_init (void)
{
  if (memory)
    return;
  memory = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  memory_order = g_queue_new ();
  sized = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  sized_order = g_queue_new ();
  pending = g_hash_table_new (g_str_hash, g_str_equal);
  queue = g_queue_new ();
  session = soup_session_new_with_options ("timeout", 30, "max-conns-per-host", MAX_ACTIVE, NULL);
  cache_dir = g_build_filename (g_get_user_cache_dir (), "vibeos-software", "images", NULL);
}

GdkTexture *
sw_image_cache_lookup (const char *url)
{
  loader_init ();
  return url ? g_hash_table_lookup (memory, url) : NULL;
}

static gsize
texture_bytes (GdkTexture *texture)
{
  return (gsize) gdk_texture_get_width (texture) * gdk_texture_get_height (texture) * 4;
}

static void
memory_add (const char *url, GdkTexture *texture)
{
  if (g_hash_table_contains (memory, url))
    return;
  if (g_hash_table_size (memory) >= MEMORY_LIMIT || memory_bytes + texture_bytes (texture) > MEMORY_BYTES)
    /* the oldest half go; the widgets showing them keep their own reference */
    while (!g_queue_is_empty (memory_order)
           && (g_hash_table_size (memory) > MEMORY_LIMIT / 2 || memory_bytes > MEMORY_BYTES / 2))
      {
        char *old = g_queue_pop_head (memory_order);
        GdkTexture *gone = g_hash_table_lookup (memory, old);

        if (gone)
          memory_bytes -= texture_bytes (gone);
        g_hash_table_remove (memory, old);
        g_free (old);
      }
  g_hash_table_insert (memory, g_strdup (url), g_object_ref (texture));
  g_queue_push_tail (memory_order, g_strdup (url));
  memory_bytes += texture_bytes (texture);
}

static GdkTexture *
sized_lookup (const char *key)
{
  GdkTexture *texture = g_hash_table_lookup (sized, key);
  GList *link;

  /* used again: the last to go */
  if (texture && (link = g_queue_find_custom (sized_order, key, (GCompareFunc) strcmp)))
    {
      g_queue_unlink (sized_order, link);
      g_queue_push_tail_link (sized_order, link);
    }
  return texture;
}

static void
sized_add (const char *key, GdkTexture *texture)
{
  if (g_hash_table_contains (sized, key))
    return;
  while (!g_queue_is_empty (sized_order) && sized_bytes + texture_bytes (texture) > SIZED_BYTES)
    {
      char *old = g_queue_pop_head (sized_order);
      GdkTexture *gone = g_hash_table_lookup (sized, old);

      if (gone)
        sized_bytes -= texture_bytes (gone);
      g_hash_table_remove (sized, old);
      g_free (old);
    }
  g_hash_table_insert (sized, g_strdup (key), g_object_ref (texture));
  g_queue_push_tail (sized_order, g_strdup (key));
  sized_bytes += texture_bytes (texture);
}

static char *
cache_path (const char *url)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, url, -1);

  return g_build_filename (cache_dir, hash, NULL);
}

static void
pending_finish (Pending *p, GdkTexture *texture)
{
  GList *waiters = g_steal_pointer (&p->waiters);

  g_hash_table_remove (pending, p->url);
  if (texture)
    memory_add (p->url, texture);
  for (GList *l = waiters; l; l = l->next)
    image_set_texture (l->data, texture, texture == NULL);
  g_list_free (waiters);
  g_free (p->url);
  g_free (p);
  active--;
  pump ();
}

/* the picture's bytes as a texture: in a worker thread */
static GdkTexture *
decode (GBytes *bytes, GError **error)
{
  g_autoptr (GdkPixbufLoader) loader = gdk_pixbuf_loader_new ();
  GdkPixbuf *pixbuf;
  g_autoptr (GBytes) pixels = NULL;

  if (!gdk_pixbuf_loader_write_bytes (loader, bytes, error) || !gdk_pixbuf_loader_close (loader, error))
    return NULL;
  pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
  if (!pixbuf)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a picture");
      return NULL;
    }
  pixels = gdk_pixbuf_read_pixel_bytes (pixbuf);
  return gdk_memory_texture_new (gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf),
                                 gdk_pixbuf_get_has_alpha (pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                 pixels, gdk_pixbuf_get_rowstride (pixbuf));
}

typedef struct {
  char   *path;
  GBytes *bytes;     /* downloaded: decode and keep; NULL: read from the disk */
} Work;

static void
work_free (gpointer data)
{
  Work *work = data;

  g_free (work->path);
  g_clear_pointer (&work->bytes, g_bytes_unref);
  g_free (work);
}

static void
work_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Work *work = data;
  g_autoptr (GError) error = NULL;
  GdkTexture *texture;

  if (!work->bytes)
    {
      char *contents;
      gsize length;

      if (!g_file_get_contents (work->path, &contents, &length, NULL))
        {
          /* not kept yet: download it */
          g_task_return_pointer (task, NULL, NULL);
          return;
        }
      work->bytes = g_bytes_new_take (contents, length);
      if (!(texture = decode (work->bytes, &error)))
        g_unlink (work->path);
      g_task_return_pointer (task, texture, g_object_unref);
      return;
    }
  if ((texture = decode (work->bytes, &error)))
    {
      g_autofree char *dir = g_path_get_dirname (work->path);

      g_mkdir_with_parents (dir, 0700);
      g_file_set_contents (work->path, g_bytes_get_data (work->bytes, NULL), g_bytes_get_size (work->bytes), NULL);
    }
  else
    g_debug ("could not decode a picture: %s", error->message);
  g_task_return_pointer (task, texture, g_object_unref);
}

static void
on_decoded (GObject *source, GAsyncResult *res, gpointer data)
{
  Pending *p = data;
  g_autoptr (GdkTexture) texture = g_task_propagate_pointer (G_TASK (res), NULL);

  pending_finish (p, texture);
}

static void
on_downloaded (GObject *source, GAsyncResult *res, gpointer data)
{
  Pending *p = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  g_autoptr (GTask) task = NULL;
  Work *work;

  if (!bytes || !msg || soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      g_debug ("could not download %s: %s", p->url, error ? error->message : "not 200");
      pending_finish (p, NULL);
      return;
    }
  work = g_new0 (Work, 1);
  work->path = cache_path (p->url);
  work->bytes = g_bytes_ref (bytes);
  task = g_task_new (NULL, NULL, on_decoded, p);
  g_task_set_task_data (task, work, work_free);
  g_task_run_in_thread (task, work_thread);
}

static void
on_disk_read (GObject *source, GAsyncResult *res, gpointer data)
{
  Pending *p = data;
  g_autoptr (GdkTexture) texture = g_task_propagate_pointer (G_TASK (res), NULL);
  g_autoptr (SoupMessage) msg = NULL;

  if (texture)
    {
      pending_finish (p, texture);
      return;
    }
  msg = soup_message_new ("GET", p->url);
  if (!msg)
    {
      pending_finish (p, NULL);
      return;
    }
  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT, NULL, on_downloaded, p);
}

static void
start (Pending *p)
{
  g_autoptr (GTask) task = g_task_new (NULL, NULL, on_disk_read, p);
  Work *work = g_new0 (Work, 1);

  p->started = TRUE;
  active++;
  work->path = cache_path (p->url);
  g_task_set_task_data (task, work, work_free);
  g_task_run_in_thread (task, work_thread);
}

static void
pump (void)
{
  while (active < MAX_ACTIVE && !g_queue_is_empty (queue))
    start (g_queue_pop_tail (queue));
}

static void
request (SwImage *image, const char *url)
{
  Pending *p;

  loader_init ();
  p = g_hash_table_lookup (pending, url);
  if (!p)
    {
      p = g_new0 (Pending, 1);
      p->url = g_strdup (url);
      g_hash_table_insert (pending, p->url, p);
      g_queue_push_tail (queue, p);
    }
  else if (!p->started)
    {
      /* asked again: to the front */
      g_queue_remove (queue, p);
      g_queue_push_tail (queue, p);
    }
  p->waiters = g_list_prepend (p->waiters, image);
  pump ();
}

static void
forget (SwImage *image, const char *url)
{
  Pending *p = url && pending ? g_hash_table_lookup (pending, url) : NULL;

  if (!p)
    return;
  p->waiters = g_list_remove (p->waiters, image);
  /* nobody wants it any more and it has not started: drop it */
  if (!p->waiters && !p->started)
    {
      g_queue_remove (queue, p);
      g_hash_table_remove (pending, p->url);
      g_free (p->url);
      g_free (p);
    }
}

/* ---- the widget ------------------------------------------------------------------------------------------------ */

struct _SwImage {
  GtkWidget     parent_instance;
  char         *url;
  char         *fallback;
  GdkPaintable *picture;
  GdkTexture   *sized;        /* the picture as drawn now, made for the widget's size in screen pixels */
  char         *sizing;       /* the key of the one being made */
  int           width, height;
  double        radius, aspect;
  gboolean      requested, failed, contain;
  GPtrArray    *watched;      /* GtkAdjustment */
  guint         tick;
};

G_DEFINE_FINAL_TYPE (SwImage, sw_image, GTK_TYPE_WIDGET)

enum { LOADED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
forget_sized (SwImage *self)
{
  g_clear_object (&self->sized);
  g_clear_pointer (&self->sizing, g_free);
}

static void
image_set_texture (SwImage *self, GdkTexture *texture, gboolean failed)
{
  forget_sized (self);
  g_set_object (&self->picture, (GdkPaintable *) texture);
  self->failed = failed;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[LOADED], 0);
}

/* ---- the picture at the size it is drawn ----------------------------------------------------------------------- *
 *
 * A picture drawn smaller than it is would be scaled down by the GPU every frame it is drawn (and, filtered well,
 * through mipmaps made on the way). Instead it is scaled once, in a worker thread, to exactly the widget's box in
 * screen pixels, cropped as the box crops it, and then drawn pixel for pixel. Until that copy is in (a frame or
 * two), the picture itself is drawn scaled. */

typedef struct {
  GdkTexture *texture;
  int         width, height;   /* the box, in screen pixels */
  char       *key;
  GWeakRef    image;
} Sizing;

static void
sizing_free (gpointer data)
{
  Sizing *s = data;

  g_object_unref (s->texture);
  g_free (s->key);
  g_weak_ref_clear (&s->image);
  g_free (s);
}

/* the texture scaled to cover width × height and cropped to it, centred: in a worker thread */
static GdkTexture *
scale_cover (GdkTexture *texture, int width, int height)
{
  int pw = gdk_texture_get_width (texture), ph = gdk_texture_get_height (texture);
  gboolean alpha = gdk_texture_get_format (texture) != GDK_MEMORY_R8G8B8;
  GdkTextureDownloader *downloader = gdk_texture_downloader_new (texture);
  g_autoptr (GBytes) pixels = NULL, out = NULL;
  g_autoptr (GdkPixbuf) source = NULL, scaled = NULL, cropped = NULL;
  double f = MAX ((double) width / pw, (double) height / ph);
  int sw = MAX (width, (int) ceil (pw * f)), sh = MAX (height, (int) ceil (ph * f));
  gsize stride;

  gdk_texture_downloader_set_format (downloader, alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8);
  pixels = gdk_texture_downloader_download_bytes (downloader, &stride);
  gdk_texture_downloader_free (downloader);
  source = gdk_pixbuf_new_from_bytes (pixels, GDK_COLORSPACE_RGB, alpha, 8, pw, ph, (int) stride);
  scaled = gdk_pixbuf_scale_simple (source, sw, sh, GDK_INTERP_BILINEAR);
  if (!scaled)
    return NULL;
  cropped = gdk_pixbuf_new_subpixbuf (scaled, (sw - width) / 2, (sh - height) / 2, width, height);
  g_set_object (&cropped, gdk_pixbuf_copy (cropped));
  out = gdk_pixbuf_read_pixel_bytes (cropped);
  return gdk_memory_texture_new (width, height, alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, out,
                                 gdk_pixbuf_get_rowstride (cropped));
}

static void
sizing_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Sizing *s = data;

  g_task_return_pointer (task, scale_cover (s->texture, s->width, s->height), g_object_unref);
}

static void
on_sized (GObject *source, GAsyncResult *res, gpointer data)
{
  Sizing *s = g_task_get_task_data (G_TASK (res));
  g_autoptr (GdkTexture) texture = g_task_propagate_pointer (G_TASK (res), NULL);
  g_autoptr (SwImage) self = g_weak_ref_get (&s->image);

  if (!texture)
    return;
  sized_add (s->key, texture);
  /* still the same picture at the same size */
  if (self && !g_strcmp0 (self->sizing, s->key))
    {
      g_clear_pointer (&self->sizing, g_free);
      g_set_object (&self->sized, texture);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* the copy for a box of width × height screen pixels: at hand, or asked for */
static GdkTexture *
sized_for (SwImage *self, GdkTexture *texture, int width, int height)
{
  g_autofree char *key = NULL;
  GdkTexture *found;
  g_autoptr (GTask) task = NULL;
  Sizing *s;

  if (self->sized && gdk_texture_get_width (self->sized) == width && gdk_texture_get_height (self->sized) == height)
    return self->sized;
  if (!self->url)
    return NULL;
  key = g_strdup_printf ("%dx%d %s", width, height, self->url);
  if ((found = sized_lookup (key)))
    {
      g_clear_pointer (&self->sizing, g_free);
      g_set_object (&self->sized, found);
      return found;
    }
  if (!g_strcmp0 (self->sizing, key))
    return NULL;
  s = g_new0 (Sizing, 1);
  s->texture = g_object_ref (texture);
  s->width = width;
  s->height = height;
  s->key = g_strdup (key);
  g_weak_ref_init (&s->image, self);
  g_free (self->sizing);
  self->sizing = g_steal_pointer (&key);
  task = g_task_new (NULL, NULL, on_sized, NULL);
  g_task_set_task_data (task, s, sizing_free);
  g_task_run_in_thread (task, sizing_thread);
  return NULL;
}

static void
unwatch (SwImage *self)
{
  for (guint i = 0; i < self->watched->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->watched, i), self);
  g_ptr_array_set_size (self->watched, 0);
  if (self->tick)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
      self->tick = 0;
    }
}

/* whether the widget is within a screen of the window (where it will be seen once scrolled a little) */
static gboolean
near_screen (SwImage *self)
{
  GtkWidget *root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
  graphene_rect_t bounds;
  int w, h;

  if (!root || !gtk_widget_get_mapped (GTK_WIDGET (self))
      || !gtk_widget_compute_bounds (GTK_WIDGET (self), root, &bounds))
    return FALSE;
  w = gtk_widget_get_width (root);
  h = gtk_widget_get_height (root);
  return bounds.origin.x + bounds.size.width >= -w && bounds.origin.x <= 2 * w
         && bounds.origin.y + bounds.size.height >= -h && bounds.origin.y <= 2 * h;
}

static void
check (SwImage *self)
{
  if (self->requested || !self->url || self->picture || !near_screen (self))
    return;
  self->requested = TRUE;
  unwatch (self);
  request (self, self->url);
}

static void
on_scrolled (GtkAdjustment *adjustment, SwImage *self)
{
  check (self);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  SwImage *self = SW_IMAGE (widget);

  self->tick = 0;
  check (self);
  return G_SOURCE_REMOVE;
}

static void
watch (GtkAdjustment *adjustment, SwImage *self)
{
  if (!adjustment || g_ptr_array_find (self->watched, adjustment, NULL))
    return;
  g_ptr_array_add (self->watched, adjustment);
  g_signal_connect (adjustment, "value-changed", G_CALLBACK (on_scrolled), self);
  g_signal_connect (adjustment, "changed", G_CALLBACK (on_scrolled), self);
}

/* watches the scroll views it is in until it is near the screen */
static void
start_watching (SwImage *self)
{
  GtkWidget *widget = GTK_WIDGET (self);

  if (self->requested || !self->url || self->picture)
    return;
  for (GtkWidget *w = gtk_widget_get_parent (widget); w; w = gtk_widget_get_parent (w))
    if (SPONGY_IS_SCROLL_VIEW (w))
      {
        /* one of the two, the way it scrolls */
        watch (spongy_scroll_view_get_hadjustment (SPONGY_SCROLL_VIEW (w)), self);
        watch (spongy_scroll_view_get_vadjustment (SPONGY_SCROLL_VIEW (w)), self);
      }
    else if (GTK_IS_SCROLLED_WINDOW (w))
      {
        watch (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (w)), self);
        watch (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (w)), self);
      }
  /* once it has its place */
  if (!self->tick)
    self->tick = gtk_widget_add_tick_callback (widget, on_tick, NULL, NULL);
}

static void
sw_image_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (sw_image_parent_class)->map (widget);
  start_watching (SW_IMAGE (widget));
}

static void
sw_image_unmap (GtkWidget *widget)
{
  unwatch (SW_IMAGE (widget));
  GTK_WIDGET_CLASS (sw_image_parent_class)->unmap (widget);
}

static void
sw_image_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  SwImage *self = SW_IMAGE (widget);

  if (!self->requested && self->url && !self->picture && !self->tick && gtk_widget_get_mapped (widget))
    self->tick = gtk_widget_add_tick_callback (widget, on_tick, NULL, NULL);
}

static GtkSizeRequestMode
sw_image_get_request_mode (GtkWidget *widget)
{
  SwImage *self = SW_IMAGE (widget);

  return self->aspect > 0 && self->height < 0 ? GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void
sw_image_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_base,
                  int *nat_base)
{
  SwImage *self = SW_IMAGE (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      int w = self->width >= 0 ? self->width
              : self->height >= 0 && self->aspect > 0 ? (int) (self->height * self->aspect) : 0;

      *min = self->width >= 0 ? w : 0;
      *nat = w;
    }
  else
    {
      int h = self->height >= 0 ? self->height : self->aspect > 0 && for_size > 0 ? (int) (for_size / self->aspect) : 0;

      *min = *nat = h;
    }
}

static void
sw_image_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  SwImage *self = SW_IMAGE (widget);
  int w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  graphene_rect_t box = GRAPHENE_RECT_INIT (0, 0, w, h);
  GskRoundedRect rounded;

  if (w <= 0 || h <= 0)
    return;
  gsk_rounded_rect_init_from_rect (&rounded, &box, self->radius);
  if (self->radius > 0)
    gtk_snapshot_push_rounded_clip (snapshot, &rounded);
  if (self->picture && self->contain)
    {
      /* all of it, as large as fits, centred: the full-screen viewer's pictures, rarely scaled down */
      double pw = gdk_paintable_get_intrinsic_width (self->picture);
      double ph = gdk_paintable_get_intrinsic_height (self->picture);
      double scale = pw > 0 && ph > 0 ? MIN (w / pw, h / ph) : 1;
      double dw = pw > 0 ? pw * scale : w, dh = ph > 0 ? ph * scale : h;

      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((w - dw) / 2, (h - dh) / 2));
      /* a picture larger than the screen (a full-size screenshot) through mipmaps, made once, so that it does not
       * shimmer drawn smaller; it is drawn at many sizes as it zooms, so no copy is made for one of them */
      if (GDK_IS_TEXTURE (self->picture))
        gtk_snapshot_append_scaled_texture (snapshot, GDK_TEXTURE (self->picture),
                                            scale * vibeos_device_scale (widget) < 0.95 ? GSK_SCALING_FILTER_TRILINEAR
                                                                                        : GSK_SCALING_FILTER_LINEAR,
                                            &GRAPHENE_RECT_INIT (0, 0, dw, dh));
      else
        gdk_paintable_snapshot (self->picture, snapshot, dw, dh);
      gtk_snapshot_restore (snapshot);
    }
  else if (self->picture)
    {
      double pw = gdk_paintable_get_intrinsic_width (self->picture);
      double ph = gdk_paintable_get_intrinsic_height (self->picture);
      double scale = pw > 0 && ph > 0 ? MAX (w / pw, h / ph) : 1;
      double dw = pw > 0 ? pw * scale : w, dh = ph > 0 ? ph * scale : h;
      double device = vibeos_device_scale (widget);
      int sw = (int) ceil (w * device), sh = (int) ceil (h * device);
      GdkTexture *copy = NULL;

      /* drawn smaller than it is: the copy made for this box, pixel for pixel */
      if (GDK_IS_TEXTURE (self->picture) && pw > 0 && ph > 0 && scale * device < 0.95)
        copy = sized_for (self, GDK_TEXTURE (self->picture), sw, sh);
      if (copy)
        {
          gtk_snapshot_append_texture (snapshot, copy, &box);
          if (self->radius > 0)
            gtk_snapshot_pop (snapshot);
          return;
        }
      /* fill the box, centred, cropping what sticks out (screenshots of slightly different shapes) */
      gtk_snapshot_push_clip (snapshot, &box);
      if (GDK_IS_TEXTURE (self->picture))
        gtk_snapshot_append_scaled_texture (snapshot, GDK_TEXTURE (self->picture), GSK_SCALING_FILTER_LINEAR,
                                            &GRAPHENE_RECT_INIT ((w - dw) / 2, (h - dh) / 2, dw, dh));
      else
        {
          gtk_snapshot_save (snapshot);
          gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((w - dw) / 2, (h - dh) / 2));
          gdk_paintable_snapshot (self->picture, snapshot, dw, dh);
          gtk_snapshot_restore (snapshot);
        }
      gtk_snapshot_pop (snapshot);
    }
  else
    {
      GdkRGBA fill = vibeos_color (VIBEOS_SURFACE_CONTAINER_HIGHEST);

      gtk_snapshot_append_color (snapshot, &fill, &box);
      if (self->fallback && (self->failed || !self->url))
        {
          GdkRGBA color = vibeos_color (VIBEOS_ON_SURFACE_VARIANT);
          double size = MIN (w, h) * 0.5;

          vibeos_snapshot_icon (snapshot, widget, self->fallback, (w - size) / 2, (h - size) / 2, size, &color);
        }
    }
  if (self->radius > 0)
    gtk_snapshot_pop (snapshot);
}

static void
sw_image_dispose (GObject *object)
{
  SwImage *self = SW_IMAGE (object);

  unwatch (self);
  forget (self, self->url);
  forget_sized (self);
  g_clear_object (&self->picture);
  G_OBJECT_CLASS (sw_image_parent_class)->dispose (object);
}

static void
sw_image_finalize (GObject *object)
{
  SwImage *self = SW_IMAGE (object);

  g_free (self->url);
  g_free (self->fallback);
  g_ptr_array_unref (self->watched);
  G_OBJECT_CLASS (sw_image_parent_class)->finalize (object);
}

static void
sw_image_class_init (SwImageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = sw_image_dispose;
  G_OBJECT_CLASS (klass)->finalize = sw_image_finalize;
  widget_class->map = sw_image_map;
  widget_class->unmap = sw_image_unmap;
  widget_class->size_allocate = sw_image_size_allocate;
  widget_class->get_request_mode = sw_image_get_request_mode;
  widget_class->measure = sw_image_measure;
  widget_class->snapshot = sw_image_snapshot;
  gtk_widget_class_set_css_name (widget_class, "sw-image");
  signals[LOADED] = g_signal_new ("loaded", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
  gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
sw_image_init (SwImage *self)
{
  self->watched = g_ptr_array_new ();
  self->width = self->height = -1;
}

GtkWidget *
sw_image_new (int width, int height, double radius)
{
  SwImage *self = g_object_new (SW_TYPE_IMAGE, NULL);

  self->width = width;
  self->height = height;
  self->radius = radius;
  return GTK_WIDGET (self);
}

void
sw_image_set_url (SwImage *self, const char *url)
{
  GdkTexture *texture;

  if (!g_strcmp0 (self->url, url) && (self->picture || self->requested))
    return;
  unwatch (self);
  forget (self, self->url);
  g_free (self->url);
  self->url = url && *url ? g_strdup (url) : NULL;
  self->requested = self->failed = FALSE;
  texture = sw_image_cache_lookup (self->url);
  forget_sized (self);
  g_set_object (&self->picture, (GdkPaintable *) texture);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    start_watching (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
sw_image_set_aspect (SwImage *self, double aspect)
{
  self->aspect = aspect;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
sw_image_set_fallback (SwImage *self, const char *icon_name)
{
  g_free (self->fallback);
  self->fallback = g_strdup (icon_name);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
sw_image_set_paintable (SwImage *self, GdkPaintable *paintable)
{
  unwatch (self);
  forget (self, self->url);
  forget_sized (self);
  g_clear_pointer (&self->url, g_free);
  g_set_object (&self->picture, paintable);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
sw_image_has_picture (SwImage *self)
{
  return self->picture != NULL;
}

void
sw_image_set_contain (SwImage *self, gboolean contain)
{
  self->contain = contain;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
sw_image_set_size (SwImage *self, int width, int height)
{
  if (self->width == width && self->height == height)
    return;
  self->width = width;
  self->height = height;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
