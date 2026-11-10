/* Contact pictures (ct-avatar.h). */
#include "ct-avatar.h"
#include "ct-contact.h"
#include "ct-ui.h"

#include "avatar.h"

#include <math.h>

#define RESOURCE "/de/yesman/Contacts/images/"

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

guint32
ct_avatar_header_color (const char *uid)
{
  int index = ct_avatar_color_index (uid);

  /* the default grey's hue (0) is nearest accent2_600, the lowest hue in the table (§6.2) */
  return index < 0 ? 0xFF4A6366u : CT_QC_HEADER_BG[index];
}

/* LetterTileDrawable.draw (§6.2): the disc, then the letter's ink box centred, or the silhouette at alpha 138 */
void
ct_avatar_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter,
                    const char *uid, const char *display_name, GdkTexture *photo)
{
  double r = diameter / 2;
  graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (cx - r), (float) (cy - r), (float) diameter, (float) diameter);
  GskRoundedRect circle;
  int index;
  gunichar letter;

  gsk_rounded_rect_init_from_rect (&circle, &box, (float) r);
  gtk_snapshot_push_rounded_clip (snapshot, &circle);
  if (photo)
    {
      /* thumbnails are square; anything else is cropped to the middle, as CP2 crops its thumbnails */
      double w = gdk_texture_get_width (photo), h = gdk_texture_get_height (photo);
      double scale = diameter / MIN (w, h);

      gtk_snapshot_append_texture (snapshot, photo,
                                   &GRAPHENE_RECT_INIT ((float) (cx - w * scale / 2), (float) (cy - h * scale / 2),
                                                        (float) (w * scale), (float) (h * scale)));
      gtk_snapshot_pop (snapshot);
      return;
    }
  index = ct_avatar_color_index (uid);
  {
    GdkRGBA fill = ct_rgba (index < 0 ? CT_TILE_DEFAULT : CT_TILE_COLORS[index]);

    gtk_snapshot_append_color (snapshot, &fill, &box);
  }
  /* no lookup key: the display name picks the colour and no letter is drawn (ContactPhotoManager :397-405) */
  letter = uid && *uid ? ct_tile_letter (display_name) : 0;
  if (letter)
    {
      char text[8] = { 0 };
      g_autoptr(PangoLayout) layout = NULL;
      PangoRectangle ink, logical;
      GdkRGBA white = ct_rgba (CT_TILE_LETTER);

      g_unichar_to_utf8 (letter, text);
      /* Paint text is not touched by Bold text: sans-serif 400 at 67 % of the tile */
      layout = text_layout (widget, CT_TEXT (diameter * CT_TILE_LETTER_RATIO, CT_WEIGHT_PAINT), text, 0);
      pango_layout_get_extents (layout, &ink, &logical);
      gtk_snapshot_save (snapshot);
      gtk_snapshot_translate (snapshot,
                              &GRAPHENE_POINT_INIT ((float) (cx - logical.width / (2.0 * PANGO_SCALE)),
                                                    (float) (cy - (ink.y + ink.height / 2.0) / PANGO_SCALE)));
      gtk_snapshot_append_layout (snapshot, layout, &white);
      gtk_snapshot_restore (snapshot);
    }
  else
    {
      gtk_snapshot_push_opacity (snapshot, CT_TILE_SILHOUETTE_ALPHA / 255.0);
      gtk_snapshot_append_texture (snapshot, image ("ic_person_avatar.png"), &box);
      gtk_snapshot_pop (snapshot);
    }
  gtk_snapshot_pop (snapshot);
}

/* centerCrop: the picture scaled to cover @bounds, centred, cut to it */
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
ct_avatar_header_snapshot (GtkSnapshot *snapshot, const graphene_rect_t *bounds, guint32 tint, GdkTexture *photo)
{
  GdkRGBA background = ct_rgba (tint);
  /* the silhouette's white, lightness-inverted by force dark: black, its alpha kept */
  graphene_matrix_t to_black;
  graphene_vec4_t no_offset;

  if (photo)
    {
      append_cropped (snapshot, photo, bounds);
      return;
    }
  gtk_snapshot_append_color (snapshot, &background, bounds);
  graphene_matrix_init_from_float (&to_black, (float[16]) { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1 });
  graphene_vec4_init (&no_offset, 0, 0, 0, 0);
  gtk_snapshot_push_color_matrix (snapshot, &to_black, &no_offset);
  append_cropped (snapshot, image ("person_white_540dp.png"), bounds);
  gtk_snapshot_pop (snapshot);
}
