/* Contact pictures (ct-avatar.h). */
#include "ct-avatar.h"
#include "ct-contact.h"
#include "ct-ui.h"

#include "avatar.h"

#include <vibeos.h>

#include <math.h>

#define RESOURCE "/vibeos/Contacts/images/"

static GdkTexture *
image (const char *name)
{
  static GHashTable *cache;
  GdkTexture *texture;

  if (!cache)
    cache = g_hash_table_new (g_str_hash, g_str_equal);
  texture = g_hash_table_lookup (cache, name);
  if (!texture)
    {
      g_autofree char *path = g_strconcat (RESOURCE, name, NULL);

      texture = gdk_texture_new_from_resource (path);
      g_hash_table_insert (cache, (gpointer) name, texture);
    }
  return texture;
}

GdkTexture *
ct_avatar_photo (GtkWidget *widget, const char *uid, GBytes *bytes)
{
  return avatar_photo (widget, uid, bytes);
}

GdkTexture *
ct_avatar_photo_now (GBytes *bytes)
{
  return avatar_texture_from_bytes (bytes);
}

int
ct_avatar_color_index (const char *uid)
{
  return ct_tile_color_index (uid);
}

/* The twelve tiles: the three accent palettes, each at four tones. Dark mode has deep tiles with a pale letter,
 * light mode pale tiles with a deep letter. */
static const VibeosPalette tile_palettes[3] = {
  VIBEOS_PALETTE_ACCENT1, VIBEOS_PALETTE_ACCENT2, VIBEOS_PALETTE_ACCENT3,
};
static const int tile_stops_dark[4] = { 700, 800, 600, 500 };
static const int tile_stops_light[4] = { 200, 100, 300, 400 };
/* QuickContact's header, under white text in either mode */
static const int header_stops[4] = { 600, 700, 600, 700 };

static gboolean
dark (void)
{
  return vibeos_theme_get_dark (vibeos_theme_get_default ());
}

GdkRGBA
ct_avatar_tile_color (const char *uid)
{
  int index = ct_avatar_color_index (uid);

  /* the default tile: the neutral palette */
  if (index < 0)
    return vibeos_palette_stop (VIBEOS_PALETTE_NEUTRAL2, dark () ? 700 : 200);
  return vibeos_palette_stop (tile_palettes[index % 3],
                              (dark () ? tile_stops_dark : tile_stops_light)[(index / 3) % 4]);
}

GdkRGBA
ct_avatar_letter_color (const char *uid)
{
  int index = ct_avatar_color_index (uid);

  return vibeos_palette_stop (index < 0 ? VIBEOS_PALETTE_NEUTRAL2 : tile_palettes[index % 3], dark () ? 100 : 800);
}

GdkRGBA
ct_avatar_header_color (const char *uid)
{
  int index = uid ? ct_avatar_color_index (uid) : -1;

  /* white text goes over it in either mode */
  return vibeos_palette_stop (index < 0 ? VIBEOS_PALETTE_NEUTRAL2 : tile_palettes[index % 3],
                              header_stops[(index < 0 ? 0 : index / 3) % 4]);
}

/* the disc, then the letter's ink box centred, or the silhouette */
void
ct_avatar_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter,
                    const char *uid, const char *display_name, GdkTexture *photo)
{
  double r = diameter / 2;
  graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (cx - r), (float) (cy - r), (float) diameter, (float) diameter);
  GskRoundedRect circle;
  gunichar letter;

  gsk_rounded_rect_init_from_rect (&circle, &box, (float) r);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
  if (photo)
    {
      /* thumbnails are square; anything else is cropped to the middle */
      double w = gdk_texture_get_width (photo), h = gdk_texture_get_height (photo);
      double scale = diameter / MIN (w, h);

      gtk_snapshot_append_texture (snapshot, photo,
                                   &GRAPHENE_RECT_INIT ((float) (cx - w * scale / 2), (float) (cy - h * scale / 2),
                                                        (float) (w * scale), (float) (h * scale)));
      gtk_snapshot_pop (snapshot);
      return;
    }
  {
    GdkRGBA fill = ct_avatar_tile_color (uid);

    gtk_snapshot_append_color (snapshot, &fill, &box);
  }
  /* no UID: the display name picks the colour and no letter is drawn */
  letter = uid && *uid ? ct_tile_letter (display_name) : 0;
  if (letter)
    {
      char text[8] = { 0 };
      g_autoptr(PangoLayout) layout = NULL;
      PangoRectangle ink, logical;
      GdkRGBA ink_color = ct_avatar_letter_color (uid);

      g_unichar_to_utf8 (letter, text);
      /* the letter's weight is not raised by Bold text */
      layout = text_layout (widget, CT_TEXT (diameter * CT_TILE_LETTER_RATIO, CT_WEIGHT_PAINT), text, 0);
      pango_layout_get_extents (layout, &ink, &logical);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot,
                              &GRAPHENE_POINT_INIT ((float) (cx - logical.width / (2.0 * PANGO_SCALE)),
                                                    (float) (cy - (ink.y + ink.height / 2.0) / PANGO_SCALE)));
      gtk_snapshot_append_layout (snapshot, layout, &ink_color);
      gtk_snapshot_restore (snapshot);
    }
  else
    {
      /* the silhouette in the letter's colour */
      GdkRGBA ink_color = ct_avatar_letter_color (uid);

      vibeos_snapshot_icon (snapshot, widget, "ct-person-symbolic", cx - diameter * 0.3, cy - diameter * 0.3,
                            diameter * 0.6, &ink_color);
    }
  gtk_snapshot_pop (snapshot);
}

GdkPaintable *
ct_avatar_paintable (GtkWidget *widget, const char *uid, const char *display_name, GBytes *photo, double diameter)
{
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  g_autoptr(GdkTexture) picture = photo ? ct_avatar_photo_now (photo) : NULL;

  ct_avatar_snapshot (snapshot, widget, diameter / 2, diameter / 2, diameter, uid, display_name, picture);
  return gtk_snapshot_free_to_paintable (snapshot, &GRAPHENE_SIZE_INIT (diameter, diameter));
}

/* cropped to fill: the picture scaled to cover @bounds, centred, cut to it */
static void
append_cropped (GtkSnapshot *snapshot, GdkTexture *texture, const graphene_rect_t *bounds)
{
  double w = gdk_texture_get_width (texture), h = gdk_texture_get_height (texture);
  double scale = MAX (bounds->size.width / w, bounds->size.height / h);
  double dw = w * scale, dh = h * scale;

  gtk_snapshot_push_clip (snapshot, bounds);
  gtk_snapshot_append_texture (snapshot, texture,
                               &GRAPHENE_RECT_INIT ((float) (bounds->origin.x + (bounds->size.width - dw) / 2),
                                                    (float) (bounds->origin.y + (bounds->size.height - dh) / 2),
                                                    (float) dw, (float) dh));
  gtk_snapshot_pop (snapshot);
}

void
ct_avatar_header_snapshot (GtkSnapshot *snapshot, const graphene_rect_t *bounds, const GdkRGBA *tint,
                           GdkTexture *photo)
{
  if (photo)
    {
      append_cropped (snapshot, photo, bounds);
      return;
    }
  gtk_snapshot_append_color (snapshot, tint, bounds);
  /* the white silhouette, in the scrim's colour in dark mode (its alpha kept), as a lightness inversion would */
  if (dark ())
    {
      GdkRGBA ink = vibeos_color (VIBEOS_SCRIM);
      graphene_matrix_t alpha_only;
      graphene_vec4_t colour;

      graphene_matrix_init_from_float (&alpha_only, (float[16]) { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1 });
      graphene_vec4_init (&colour, ink.red, ink.green, ink.blue, 0);
      gtk_snapshot_push_color_matrix (snapshot, &alpha_only, &colour);
    }
  append_cropped (snapshot, image ("person_white_540dp.png"), bounds);
  if (dark ())
    gtk_snapshot_pop (snapshot);
}

/* ===== CtAvatarView ============================================================================================ */

struct _CtAvatarView {
  GtkWidget parent;
  double size;
  char *uid, *name;
  GBytes *photo;
};

G_DEFINE_FINAL_TYPE (CtAvatarView, ct_avatar_view, GTK_TYPE_WIDGET)

static void
ct_avatar_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
                        int *minimum_baseline, int *natural_baseline)
{
  *minimum = *natural = (int) ceil (CT_AVATAR_VIEW (widget)->size);
}

static void
ct_avatar_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  CtAvatarView *self = CT_AVATAR_VIEW (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  GdkTexture *photo = self->photo ? ct_avatar_photo (widget, self->uid, self->photo) : NULL;

  ct_avatar_snapshot (snapshot, widget, w / 2, h / 2, MIN (w, h), self->uid, self->name, photo);
}

static void
ct_avatar_view_finalize (GObject *object)
{
  CtAvatarView *self = CT_AVATAR_VIEW (object);

  g_free (self->uid);
  g_free (self->name);
  g_clear_pointer (&self->photo, g_bytes_unref);
  G_OBJECT_CLASS (ct_avatar_view_parent_class)->finalize (object);
}

static void
ct_avatar_view_class_init (CtAvatarViewClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = ct_avatar_view_finalize;
  GTK_WIDGET_CLASS (klass)->measure = ct_avatar_view_measure;
  GTK_WIDGET_CLASS (klass)->snapshot = ct_avatar_view_snapshot;
  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "ct-avatar");
}

static void
ct_avatar_view_init (CtAvatarView *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
ct_avatar_view_new (double size)
{
  CtAvatarView *self = g_object_new (CT_TYPE_AVATAR_VIEW, NULL);

  self->size = size;
  return GTK_WIDGET (self);
}

void
ct_avatar_view_set (GtkWidget *widget, const char *uid, const char *name, GBytes *photo)
{
  CtAvatarView *self = CT_AVATAR_VIEW (widget);

  g_free (self->uid);
  g_free (self->name);
  self->uid = g_strdup (uid);
  self->name = g_strdup (name);
  if (photo)
    g_bytes_ref (photo);
  g_clear_pointer (&self->photo, g_bytes_unref);
  self->photo = photo;
  gtk_widget_queue_draw (widget);
}
