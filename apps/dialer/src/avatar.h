/* A contact's round picture: the photo cut to a circle, or the Dialer's letter tile (LetterTileDrawable, spec §10).
 *
 * The tile is a disc in one of twelve colours, chosen from the contact's lookup key the way Android does
 * (String.hashCode), so a contact keeps its colour on every screen. A contact's tile shows the first letter of
 * the name; someone who is not a contact gets the person glyph instead, on a colour chosen from the number. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Draws the avatar centred at (cx, cy). @identifier picks the colour (a contact's uid, else the name or number
 * shown; "" or NULL gives the default grey). @letter_from: the name whose first letter shows, NULL for the person
 * glyph. @photo, if not NULL, is drawn instead of the tile. */
void avatar_snapshot (GtkSnapshot *snapshot, GtkWidget *widget, double cx, double cy, double diameter,
                      const char *identifier, const char *letter_from, GdkTexture *photo);

/* the photo bytes of a contact (contacts.h) as a texture, or NULL when they are not an image */
GdkTexture *avatar_texture_from_bytes (GBytes *bytes);

/* The same for a list: the texture if it has been decoded, else NULL while a worker thread decodes it; @widget is
 * redrawn when it is ready. A long list of contacts must not decode its photos on the UI thread as it scrolls.
 * @key is the contact's uid: a new picture for it (other bytes) is decoded again. */
GdkTexture *avatar_photo (GtkWidget *widget, const char *key, GBytes *bytes);

/* the same as a widget of its own, for lists */
#define DIALER_TYPE_AVATAR (dialer_avatar_get_type ())
G_DECLARE_FINAL_TYPE (DialerAvatar, dialer_avatar, DIALER, AVATAR, GtkWidget)

GtkWidget *dialer_avatar_new (double diameter);
void       dialer_avatar_set (DialerAvatar *self, const char *identifier, const char *letter_from, GBytes *photo);

G_END_DECLS
