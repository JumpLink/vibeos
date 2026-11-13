/* Share: the files go to the Email portal (org.freedesktop.portal.Email.ComposeEmail) as attachments,
 * the one "send to" the portal backends on Mobian have. Share shows only when the portal has that interface and
 * every file has a local path (a server's file has one through GVfs's FUSE mount). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* asks the portal once, in the background, whether it can compose an email */
void     fm_share_init     (void);
/* whether Share can take these files (GFile) */
gboolean fm_share_possible (GPtrArray *files);
void     fm_share_files    (GtkWidget *parent, GPtrArray *files);

G_END_DECLS
