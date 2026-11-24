/* VibeosUnifiedPush: push messages for an app, through the system's UnifiedPush distributor (on VibeOS
 * vibeos-unifiedpush), by the UnifiedPush D-Bus spec 0.3.0.
 *
 * The app keeps no connection of its own and polls nothing. It registers on every start and gets an endpoint, a URL,
 * with its keys (p256dh, auth): it sends all three to its server. The server sends Web Push messages to the endpoint,
 * encrypted for those keys (RFC 8291, aes128gcm; signed with VAPID, RFC 8292, if it gave its key). The distributor
 * hands each one to the app, starting it over D-Bus if it does not run, and this object decrypts it and emits
 * "message".
 *
 *   static void
 *   startup (GApplication *app)
 *   {
 *     VibeosUnifiedPush *push = vibeos_unified_push_new (app, NULL);
 *     g_signal_connect (push, "new-endpoint", G_CALLBACK (send_to_server), NULL);
 *     g_signal_connect (push, "message", G_CALLBACK (on_message), NULL);
 *     vibeos_unified_push_register (push, NULL, SERVER_VAPID_KEY);
 *   }
 *
 * One object per registration: most apps make one, with instance NULL ("default"); a messenger with several
 * accounts makes one per account ("account-2", ...). The instance names the registration's state: its token, its key
 * pair and auth secret and its endpoint, in the app's data directory ($XDG_DATA_HOME/<app id>/unifiedpush/<instance>,
 * mode 0600). An instance name is letters, digits, "-", "_" and ".", not first.
 *
 * Call vibeos_unified_push_register on every start. "new-endpoint" fires whenever the endpoint or the keys are new or
 * changed: send all three values to the server each time. p256dh and auth are base64url, as a browser's
 * PushSubscription gives them.
 *
 * Messages come to /org/unifiedpush/Connector on the app's D-Bus name. Create the object in startup (or before
 * g_application_run): the distributor tries again for a few seconds if a message comes before it exists. The object
 * holds the GApplication (g_application_hold) while it delivers a message, and releases it after the "message"
 * handlers return: an app started by D-Bus for a push (--gapplication-service) then exits by itself. The app's D-Bus
 * service file is /usr/share/dbus-1/services/<app id>.service, with Exec=/usr/bin/<app> --gapplication-service.
 *
 * Signals:
 *   "new-endpoint"        (VibeosUnifiedPush *, const char *endpoint, const char *p256dh, const char *auth, gpointer)
 *   "message"             (VibeosUnifiedPush *, GBytes *message, gpointer): already decrypted
 *   "unregistered"        (VibeosUnifiedPush *, gpointer): the registration is gone (vibeos_unified_push_unregister,
 *                         or the user removed it in Settings): tell the server to stop sending
 *   "registration-failed" (VibeosUnifiedPush *, const char *reason, gpointer): the spec's reasons, as the
 *                         distributor gave them: "NETWORK", "VAPID_REQUIRED", "ACTION_REQUIRED" (also when no
 *                         distributor is installed), "UNAUTHORIZED", and "INTERNAL_ERROR" (also when the distributor
 *                         did not answer, or vapid_public_key is not a key)
 * Properties: "application" (GApplication, construct-only), "instance" (string, construct-only), "endpoint" (string,
 * read-only). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_UNIFIED_PUSH (vibeos_unified_push_get_type ())
G_DECLARE_FINAL_TYPE (VibeosUnifiedPush, vibeos_unified_push, VIBEOS, UNIFIED_PUSH, GObject)

/* one object per push registration. Most apps use instance NULL ("default"); a messenger with several accounts makes
 * one per account ("account-2", ...) */
VibeosUnifiedPush *vibeos_unified_push_new          (GApplication *app, const char *instance);
/* description: shown in Settings, NULL for the app's name; vapid_public_key: the app server's, base64url P-256
 * (87 characters), or NULL */
void               vibeos_unified_push_register     (VibeosUnifiedPush *push, const char *description,
                                                     const char *vapid_public_key);
void               vibeos_unified_push_unregister   (VibeosUnifiedPush *push);
/* NULL until registered */
const char        *vibeos_unified_push_get_endpoint (VibeosUnifiedPush *push);

G_END_DECLS
