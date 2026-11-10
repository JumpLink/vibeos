/* Mounting a server's share (spec §6.4): g_file_mount_enclosing_volume with a GMountOperation whose questions are
 * the app's dialogs. The password goes to GVfs only, with G_PASSWORD_SAVE_PERMANENTLY when it is to be remembered:
 * GVfs keeps it in the Secret Service, and the app writes it nowhere. */
#pragma once

#include <gtk/gtk.h>

#include "fm-remotes.h"

G_BEGIN_DECLS

typedef void (*FmMountDone) (gboolean ok, const GError *error, gpointer data);

/* mounts what @location is on. @remote's user, domain and anonymous login and @password (the server dialog's; NULL
 * when there is none) answer GVfs's first question; after that, or without them, the password dialog asks. */
void   fm_mount               (GtkWidget *parent, GFile *location, const FmRemote *remote, const char *password,
                               FmMountDone done, gpointer data);
/* "Could not connect to %s." or "Could not connect to %1$s with user %2$s." */
char  *fm_mount_error_message (GFile *location, const FmRemote *remote);

/* a removed remote's saved password out of the keyring too (GVfs's schema org.gnome.keyring.NetworkPassword) */
void   fm_mount_forget_password (const FmRemote *remote);

G_END_DECLS
