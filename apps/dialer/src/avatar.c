/* The avatar: see avatar.h. */
#include "avatar.h"

#include <neo.h>

/* the tile colours: accent1, accent2 and accent3 at 300, 400, 500 and 600, the phone's dynamic palette */
static const guint32 tile_colors[] = {
  0x81b4bb, 0x96b0b4, 0x8dafd1, 0x6799a0, 0x7c9599, 0x7394b5,
  0x4d7f86, 0x627b7f, 0x597a9a, 0x33666d, 0x4a6366, 0x406180,
};
#define DEFAULT_COLOR   0xcccccc        /* a tile with no identifier to pick a colour by */
#define LETTER_RATIO    0.67            /* the letter's size against the tile's */
#define ICON_RATIO      0.7             /* the person glyph's scale */

static GdkRGBA
rgb (guint32 value)
{
  return (GdkRGBA) { ((value >> 16) & 0xff) / 255.0, ((value >> 8) & 0xff) / 255.0, (value & 0xff) / 255.0, 1 };
}

/* java.lang.String.hashCode: s[0]·31^(n−1) + … + s[n−1] over the UTF-16 units, in 32-bit wrapping arithmetic */
static gint32
java_hash (const char *text)
{
  g_autofree gunichar2 *units = g_utf8_to_utf16 (text, -1, NULL, NULL, NULL);
  guint32 hash = 0;

  for (gunichar2 *u = units; u && *u; u++)
    hash = 31 * hash + *u;
  return (gint32) hash;
}

static GdkRGBA
tile_color (const char *identifier)
{
  gint32 hash;

  if (!identifier || !*identifier)
    return rgb (DEFAULT_COLOR);
  /* the absolute value: the one hash it cannot make positive is rare enough to fold in with the rest */
  hash = java_hash (identifier);
  return rgb (tile_colors[(hash < 0 ? -(gint64) hash : hash) % G_N_ELEMENTS (tile_colors)]);
}

/* the first character of the name, upper-cased, if it is a letter; else none */
static gboolean
first_letter (const char *name, char out[8])
{
  gunichar c;

  if (!name || !*name)
    return FALSE;
  c = g_utf8_get_char_validated (name, -1);
  if (c == (gunichar) -1 || c == (gunichar) -2 || !g_unichar_isalpha (c))
    return FALSE;
  out[g_unichar_to_utf8 (g_unichar_toupper (c), out)] = '\0';
  return TRUE;
}

static void
snapshot_letter (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter, const char *letter)
{
  g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout (widget, letter);
  g_autoptr(PangoFontDescription) font = pango_font_description_from_string ("Neo Roboto");
  PangoRectangle ink;

  /* 400, not 700: the letter is painted, and the Bold text setting does not reach painted text; centred on the
   * glyph's own ink */
  pango_font_description_set_absolute_size (font, diameter * LETTER_RATIO * PANGO_SCALE);
  pango_font_description_set_weight (font, PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (layout, font);
  pango_layout_get_pixel_extents (layout, &ink, NULL);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (cx - ink.x - ink.width / 2.0, cy - ink.y - ink.height / 2.0));
  gtk_snapshot_append_layout (snapshot, layout, &(GdkRGBA) { 1, 1, 1, 1 });
  gtk_snapshot_restore (snapshot);
}

void
avatar_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter,
                 const char *identifier, const char *letter_from, GdkTexture *photo)
{
  double r = diameter / 2;
  graphene_rect_t box = GRAPHENE_RECT_INIT (cx - r, cy - r, diameter, diameter);
  GskRoundedRect circle;
  char letter[8];

  gsk_rounded_rect_init_from_rect (&circle, &box, r);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
  if (photo)
    {
      /* centre-cropped to a square, as the circular thumbnail is */
      double w = gdk_texture_get_width (photo), h = gdk_texture_get_height (photo);
      double scale = diameter / MIN (w, h);

      gtk_snapshot_append_scaled_texture (snapshot, photo, GSK_SCALING_FILTER_TRILINEAR,
                                          &GRAPHENE_RECT_INIT (cx - w * scale / 2, cy - h * scale / 2,
                                                               w * scale, h * scale));
    }
  else
    {
      GdkRGBA color = tile_color (identifier);

      gtk_snapshot_append_color (snapshot, &color, &box);
      if (first_letter (letter_from, letter))
        snapshot_letter (snapshot, widget, cx, cy, diameter, letter);
      else
        {
          /* the person glyph, white */
          double size = diameter * ICON_RATIO;

          neo_snapshot_icon (snapshot, widget, "dialer-person-symbolic", cx - size / 2, cy - size / 2, size,
                             &(GdkRGBA) { 1, 1, 1, 1 });
        }
    }
  gtk_snapshot_pop (snapshot);
}

GdkTexture *
avatar_texture_from_bytes (GBytes *bytes)
{
  if (!bytes)
    return NULL;
  return gdk_texture_new_from_bytes (bytes, NULL);
}

/* --- photos for lists, decoded off the UI thread */

typedef struct
{
  GBytes *bytes;                /* what the texture was made from */
  GdkTexture *texture;          /* NULL while it decodes, or if it is not an image */
  gboolean pending;
  GPtrArray *waiting;           /* GWeakRef: the widgets to redraw once it is ready */
} Photo;

static GHashTable *photos;      /* key -> Photo */

static void
photo_free (Photo *photo)
{
  g_bytes_unref (photo->bytes);
  g_clear_object (&photo->texture);
  g_ptr_array_unref (photo->waiting);
  g_free (photo);
}

static void
weak_ref_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

/* gdk_texture_new_from_bytes is thread-safe for this very use */
static void
decode (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GdkTexture *texture = gdk_texture_new_from_bytes (data, NULL);

  g_task_return_pointer (task, texture, texture ? g_object_unref : NULL);
}

static void
on_decoded (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree char *key = data;
  Photo *photo = g_hash_table_lookup (photos, key);
  GBytes *bytes = g_task_get_task_data (G_TASK (result));
  GdkTexture *texture = g_task_propagate_pointer (G_TASK (result), NULL);

  /* the contact's picture changed meanwhile: this one is stale, the newer decode will come */
  if (!photo || photo->bytes != bytes)
    {
      g_clear_object (&texture);
      return;
    }
  photo->texture = texture;
  photo->pending = FALSE;
  for (guint i = 0; i < photo->waiting->len; i++)
    {
      g_autoptr(GtkWidget) widget = g_weak_ref_get (photo->waiting->pdata[i]);

      if (widget)
        gtk_widget_queue_draw (widget);
    }
  g_ptr_array_set_size (photo->waiting, 0);
}

GdkTexture *
avatar_photo (GtkWidget *widget, const char *key, GBytes *bytes)
{
  Photo *photo;
  GWeakRef *ref;

  if (!bytes || !key)
    return NULL;
  if (!photos)
    photos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) photo_free);
  photo = g_hash_table_lookup (photos, key);
  if (photo && photo->bytes != bytes && !g_bytes_equal (photo->bytes, bytes))
    {
      g_hash_table_remove (photos, key);
      photo = NULL;
    }
  if (!photo)
    {
      GTask *task = g_task_new (NULL, NULL, on_decoded, g_strdup (key));

      photo = g_new0 (Photo, 1);
      photo->bytes = g_bytes_ref (bytes);
      photo->pending = TRUE;
      photo->waiting = g_ptr_array_new_with_free_func (weak_ref_free);
      g_hash_table_insert (photos, g_strdup (key), photo);
      g_task_set_task_data (task, g_bytes_ref (bytes), (GDestroyNotify) g_bytes_unref);
      g_task_run_in_thread (task, decode);
      g_object_unref (task);
    }
  if (photo->pending)
    {
      /* a row drawn twice before the photo is ready waits once */
      for (guint i = 0; i < photo->waiting->len; i++)
        {
          g_autoptr(GtkWidget) waiting = g_weak_ref_get (photo->waiting->pdata[i]);

          if (waiting == widget)
            return NULL;
        }
      ref = g_new0 (GWeakRef, 1);
      g_weak_ref_init (ref, widget);
      g_ptr_array_add (photo->waiting, ref);
    }
  return photo->texture;
}

/* --- the widget */

struct _DialerAvatar
{
  GtkWidget parent;
  double diameter;
  char *identifier, *letter_from;
  GdkTexture *photo;
};

G_DEFINE_FINAL_TYPE (DialerAvatar, dialer_avatar, GTK_TYPE_WIDGET)

static void
dialer_avatar_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                       int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = (int) DIALER_AVATAR (widget)->diameter;
}

static void
dialer_avatar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  DialerAvatar *self = DIALER_AVATAR (widget);

  avatar_snapshot (snapshot, widget, gtk_widget_get_width (widget) / 2.0, gtk_widget_get_height (widget) / 2.0,
                   self->diameter, self->identifier, self->letter_from, self->photo);
}

static void
dialer_avatar_finalize (GObject *object)
{
  DialerAvatar *self = DIALER_AVATAR (object);

  g_free (self->identifier);
  g_free (self->letter_from);
  g_clear_object (&self->photo);
  G_OBJECT_CLASS (dialer_avatar_parent_class)->finalize (object);
}

static void
dialer_avatar_class_init (DialerAvatarClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = dialer_avatar_finalize;
  widget_class->measure = dialer_avatar_measure;
  widget_class->snapshot = dialer_avatar_snapshot;
  gtk_widget_class_set_css_name (widget_class, "avatar");
}

static void
dialer_avatar_init (DialerAvatar *self)
{
}

GtkWidget *
dialer_avatar_new (double diameter)
{
  DialerAvatar *self = g_object_new (DIALER_TYPE_AVATAR, NULL);

  self->diameter = diameter;
  return GTK_WIDGET (self);
}

void
dialer_avatar_set (DialerAvatar *self, const char *identifier, const char *letter_from, GBytes *photo)
{
  g_free (self->identifier);
  self->identifier = g_strdup (identifier);
  g_free (self->letter_from);
  self->letter_from = g_strdup (letter_from);
  g_clear_object (&self->photo);
  self->photo = avatar_texture_from_bytes (photo);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
