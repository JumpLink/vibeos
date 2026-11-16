/* Contact pictures: the round photo or letter tile in the list rows, and the square header of
 * QuickContact and the editor (the photo, or the big person silhouette on the tint colour).
 *
 * The tile's colour comes from a hash of the vCard UID (Java's String.hashCode, ct_tile_color_index), so a contact
 * keeps its colour on every screen. It cannot be the colour the same person had on an Android phone, which hashed a
 * lookup key EDS does not have. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the photo of @uid as a texture, decoded off the UI thread (the Dialer's avatar.c cache): NULL while it decodes,
 * and @widget is redrawn when it is ready */
GdkTexture *ct_avatar_photo (GtkWidget *widget, const char *uid, GBytes *bytes);
/* the same, decoded now (QuickContact's header, the editor's banner: one picture) */
GdkTexture *ct_avatar_photo_now (GBytes *bytes);

/* the round avatar of @diameter centred at (cx, cy): @photo, else the tile of @uid with the first letter of
 * @display_name or the silhouette */
void        ct_avatar_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter,
                                const char *uid, const char *display_name, GdkTexture *photo);

/* the same avatar as a picture of @diameter, for a widget that shows a paintable; its colours are the theme's as
 * they are now, so it is made again when the theme changes */
GdkPaintable *ct_avatar_paintable (GtkWidget *widget, const char *uid, const char *display_name, GBytes *photo,
                                   double diameter);

/* the colour index of a contact's tile and QuickContact's header (−1 for the default, neutral one) */
int         ct_avatar_color_index (const char *uid);
/* a tile's colour and its letter's, from the theme's palettes as they are now: a contact keeps its place in the
 * table, and the table follows the theme and light or dark */
GdkRGBA     ct_avatar_tile_color   (const char *uid);
GdkRGBA     ct_avatar_letter_color (const char *uid);
/* QuickContact's header colour for @uid (NULL: the default): the tile's hue, dark enough for white text over it */
GdkRGBA     ct_avatar_header_color (const char *uid);

/* QuickContact's and the editor's square picture over @bounds: @photo cropped to fill, else the
 * big person silhouette centred on @tint */
void        ct_avatar_header_snapshot (GtkSnapshot *snapshot, const graphene_rect_t *bounds, const GdkRGBA *tint,
                                       GdkTexture *photo);

/* the round avatar as a widget, @size px square: the photo when there is one (decoded off the UI thread), else
 * the tile */
#define CT_TYPE_AVATAR_VIEW (ct_avatar_view_get_type ())
G_DECLARE_FINAL_TYPE (CtAvatarView, ct_avatar_view, CT, AVATAR_VIEW, GtkWidget)

GtkWidget  *ct_avatar_view_new (double size);
void        ct_avatar_view_set (GtkWidget *view, const char *uid, const char *display_name, GBytes *photo);

G_END_DECLS
