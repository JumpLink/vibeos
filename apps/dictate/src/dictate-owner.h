/* dictate-owner: the phone's owner confirmed by the lock screen's own PIN or pattern, never by a voice.
 *
 * The shell's ConfirmOwner (vibeos.Shell) locks the phone if it is not locked yet, brings up its bouncer and answers
 * once the right secret is in (TRUE), or once the bouncer goes without it: back, a swipe, the screen off (FALSE). The
 * app never sees the secret; the shell keeps its own throttling of wrong tries. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* the shell can be asked (its ConfirmOwner, found once, as the app starts: dictate_owner_probe) */
void     dictate_owner_probe          (void);
/* a PIN or pattern locks the phone, and the shell can be asked for it */
gboolean dictate_owner_has_secret     (void);
void     dictate_owner_confirm_async  (const char *app_id, GCancellable *cancellable, GAsyncReadyCallback callback,
                                       gpointer data);
gboolean dictate_owner_confirm_finish (GAsyncResult *result, GError **error);

G_END_DECLS
