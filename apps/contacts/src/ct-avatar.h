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

/* the colour index of a contact's tile and QuickContact's header (−1 for the default grey) */
int         ct_avatar_color_index (const char *uid);
/* QuickContact's header colour for @uid in dark mode (the darkened colour nearest the tile's hue) */
guint32     ct_avatar_header_color (const char *uid);

/* QuickContact's and the editor's square picture over @bounds: @photo cropped to fill, else the
 * big person silhouette in black, centred on @tint */
void        ct_avatar_header_snapshot (GtkSnapshot *snapshot, const graphene_rect_t *bounds, guint32 tint,
                                       GdkTexture *photo);

G_END_DECLS
