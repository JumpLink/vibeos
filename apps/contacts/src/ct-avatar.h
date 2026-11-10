/* Contact pictures (spec §6): the round photo or LetterTileDrawable in the list rows, and the square header of
 * QuickContact and the editor (the photo, or the big person silhouette on the tint colour).
 *
 * The tile's colour comes from the vCard UID the way Android's comes from the lookup key (Java's String.hashCode,
 * ct_tile_color_index), so a contact keeps its colour on every screen; it cannot be the colour the same person had
 * on an Android phone, which hashed a lookup key EDS does not have (§6.5). */
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
/* QuickContact's header colour for @uid in dark mode (makeDark of the hue-mapped primary, §3.4) */
guint32     ct_avatar_header_color (const char *uid);

/* QuickContact's and the editor's square picture over @bounds: @photo cropped to fill (centerCrop), else the
 * person_white_540dp silhouette, which force dark draws black, centred on @tint */
void        ct_avatar_header_snapshot (GtkSnapshot *snapshot, const graphene_rect_t *bounds, guint32 tint,
                                       GdkTexture *photo);

G_END_DECLS
