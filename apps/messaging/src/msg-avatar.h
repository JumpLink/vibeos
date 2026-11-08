/* MsgAvatar: Messaging's avatar (spec §6), a circle: the contact's photo, else the letter tile (the name's first
 * letter on a colour picked from its identifier), else the person glyph on that colour; a group's is up to four of
 * them in one box. Checked, it is the blue checkmark circle a selected conversation or picked contact shows. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_AVATAR (msg_avatar_get_type ())
G_DECLARE_FINAL_TYPE (MsgAvatar, msg_avatar, MSG, AVATAR, GtkWidget)

GtkWidget *msg_avatar_new (int size);

/* one person: @name for the letter (NULL or not a letter: the glyph), @identifier for the colour (the contact's
 * lookup key when known, else the name or number), @photo or NULL */
void msg_avatar_set_person (MsgAvatar *self, const char *name, const char *identifier, GBytes *photo);
/* a group: the first four of @names, @identifiers, @photos (NULL-terminated, the same length; @photos may be NULL) */
void msg_avatar_set_group (MsgAvatar *self, const char * const *names, const char * const *identifiers,
                           GBytes * const *photos);
void msg_avatar_set_checked (MsgAvatar *self, gboolean checked);

/* letter_tile_colors[abs(identifier.hashCode()) % 12], or primary_color #33666d: a contact's incoming bubbles have
 * the colour of its avatar (§2.3) */
GdkRGBA msg_avatar_color (const char *identifier);

G_END_DECLS
