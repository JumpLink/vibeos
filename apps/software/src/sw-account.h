/* SwAccount: the store account signed in on this device, if any.
 *
 * The token. Sign-in and sign-up return a token that belongs to this device (the server's docs/api.md, signing in).
 * It is kept in the Secret Service (libsecret: the login keyring, which VibeOS unlocks with the lock-screen PIN),
 * under the schema vibeos.Software.Token with the store's address as its attribute, so a token for the local test
 * server never meets vibeos.top's. A keyring is the place for a password-equivalent: encrypted at rest, unlocked
 * with the session, and not readable by a program that only reads files. Where there is no Secret Service (a bare
 * session, the pictures' test runs) the token stays in memory for the session only and Software says so once.
 * VIBEOS_SOFTWARE_TOKEN_FILE names a file (written 0600) to use instead, for tests.
 *
 * The account's name and address are kept beside it in ~/.local/share/vibeos-software/account.json (0600), so Mine
 * shows them offline; GET /me refreshes them on every start.
 *
 * Installed apps. While signed in, the store learns what this device has installed: the whole list right after
 * signing in and on every start (PUT /me/device/apps), and each change as it happens (POST /me/device/apps/events).
 * That is what lets the account review an app and see its apps on its other devices. Signed out, Software only sends
 * the anonymous ping (POST /installs/anonymous: a device id, a package) for apps installed through it, so their
 * install counts include people without accounts.
 *
 * A 401 on any request with the token means the token is gone (signed out elsewhere, the password changed): the
 * account is forgotten and "changed" emitted. */
#pragma once

#include "sw-api.h"

G_BEGIN_DECLS

#define SW_TYPE_ACCOUNT (sw_account_get_type ())
G_DECLARE_FINAL_TYPE (SwAccount, sw_account, SW, ACCOUNT, GObject)

SwAccount  *sw_account_get_default   (void);
/* reads the token (asynchronously); "changed" follows */
void        sw_account_load          (SwAccount *self);
gboolean    sw_account_is_loaded     (SwAccount *self);

gboolean    sw_account_is_signed_in  (SwAccount *self);
/* the User object (name, email, email_verified, pending_email), or NULL */
JsonObject *sw_account_get_user      (SwAccount *self);
const char *sw_account_get_name      (SwAccount *self);
const char *sw_account_get_email     (SwAccount *self);
gboolean    sw_account_is_verified   (SwAccount *self);

/* every call's callback gets the answer (or the error) as SwApi's does */
void        sw_account_sign_in       (SwAccount *self, const char *email, const char *password,
                                      GCancellable *cancellable, SwApiCallback callback, gpointer data);
void        sw_account_register      (SwAccount *self, const char *name, const char *email, const char *password,
                                      GCancellable *cancellable, SwApiCallback callback, gpointer data);
void        sw_account_verify        (SwAccount *self, const char *code, GCancellable *cancellable,
                                      SwApiCallback callback, gpointer data);
void        sw_account_resend        (SwAccount *self, GCancellable *cancellable, SwApiCallback callback,
                                      gpointer data);
void        sw_account_forgot        (SwAccount *self, const char *email, GCancellable *cancellable,
                                      SwApiCallback callback, gpointer data);
void        sw_account_check_code    (SwAccount *self, const char *email, const char *code,
                                      GCancellable *cancellable, SwApiCallback callback, gpointer data);
void        sw_account_reset         (SwAccount *self, const char *email, const char *code, const char *password,
                                      GCancellable *cancellable, SwApiCallback callback, gpointer data);
/* signs out on the server, and forgets the token here whatever the server said */
void        sw_account_sign_out      (SwAccount *self, GCancellable *cancellable, SwApiCallback callback,
                                      gpointer data);
void        sw_account_delete        (SwAccount *self, const char *password, gboolean delete_reviews,
                                      GCancellable *cancellable, SwApiCallback callback, gpointer data);
/* GET /me again */
void        sw_account_refresh       (SwAccount *self);
/* the user object from an answer that changed it (a verified email) */
void        sw_account_set_user      (SwAccount *self, JsonObject *user);

/* Software installed `package` (a finished job): for the anonymous ping when signed out */
void        sw_account_note_installed (SwAccount *self, const char *package);

/* Signals: "changed" () signed in or out, or the user changed; "notice" (const char *text) a sentence for the user
 * (the token could not be kept) */

G_END_DECLS
