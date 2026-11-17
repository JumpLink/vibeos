/* The person using the phone, as AccountsService knows them: their name and picture, for the account button on the
 * conversation list and the account menu's header. Without AccountsService, the login name and no picture. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_ACCOUNT (msg_account_get_type ())
G_DECLARE_FINAL_TYPE (MsgAccount, msg_account, MSG, ACCOUNT, GObject)

MsgAccount *msg_account_get_default (void);
/* the full name, else the login name */
const char *msg_account_get_name    (MsgAccount *self);
const char *msg_account_get_login   (MsgAccount *self);
/* the picture file's bytes, or NULL */
GBytes     *msg_account_get_picture (MsgAccount *self);

/* Signals: "changed" (): the name or the picture came in or changed */

G_END_DECLS
