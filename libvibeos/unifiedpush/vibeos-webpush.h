/* Web Push message encryption (RFC 8291, the "aes128gcm" content coding of RFC 8188), on GnuTLS: libvibeos's own
 * use of it, not part of its API.
 *
 *   the receiving side (vibeos-webpush.c, in libvibeos): the app's key pair and auth secret, and decryption;
 *   the sending side (vibeos-webpush-send.c, for the tests and the test sender only): encryption and the VAPID
 *   header (RFC 8292) an app server sends with a message. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define VIBEOS_WEBPUSH_PUBLIC_KEY   65      /* an uncompressed P-256 point: 0x04, x, y */
#define VIBEOS_WEBPUSH_PRIVATE_KEY  32
#define VIBEOS_WEBPUSH_AUTH         16
#define VIBEOS_WEBPUSH_HEADER       86      /* salt (16), record size (4), key id length (1), the sender's key (65) */
#define VIBEOS_WEBPUSH_MAX_BODY     4096    /* what a push service must carry */
/* the largest plaintext that fits: the body less the header, the padding delimiter and the AEAD tag */
#define VIBEOS_WEBPUSH_MAX_PLAINTEXT (VIBEOS_WEBPUSH_MAX_BODY - VIBEOS_WEBPUSH_HEADER - 1 - 16)

/* a receiver's keys: what a browser's PushSubscription holds */
typedef struct {
  guint8 private_key[VIBEOS_WEBPUSH_PRIVATE_KEY];
  guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY];
  guint8 auth[VIBEOS_WEBPUSH_AUTH];
} VibeosWebPushKeys;

G_GNUC_INTERNAL char    *_vibeos_base64url_encode (const guint8 *data, gsize length);
/* strict: only base64url characters, no padding; NULL when it is not that */
G_GNUC_INTERNAL guint8  *_vibeos_base64url_decode (const char *text, gsize *length);

/* a new P-256 key pair and a random auth secret */
G_GNUC_INTERNAL gboolean _vibeos_webpush_generate (VibeosWebPushKeys *keys, GError **error);
/* the public key belongs to the private one, and is a point of the curve */
G_GNUC_INTERNAL gboolean _vibeos_webpush_check    (const VibeosWebPushKeys *keys, GError **error);
/* an aes128gcm body, a single record as RFC 8291 requires; the plaintext without its padding */
G_GNUC_INTERNAL GBytes  *_vibeos_webpush_decrypt  (const VibeosWebPushKeys *keys, const guint8 *body, gsize length,
                                                   GError **error);

/* both sides: ECDH on P-256 (the x coordinate of private · public, 32 bytes; the peer's point is checked, RFC 8291
 * §7), and RFC 8291 §3.4's content encryption key and nonce from both public keys, the ECDH and auth secrets and the
 * salt */
G_GNUC_INTERNAL gboolean _vibeos_webpush_ecdh   (const guint8 *private_key, const guint8 *own_public,
                                                 const guint8 *peer_public, guint8 secret[32], GError **error);
G_GNUC_INTERNAL void     _vibeos_webpush_derive (const guint8 *ua_public, const guint8 *as_public, const guint8 *ecdh,
                                                 const guint8 *auth, const guint8 *salt, guint8 cek[16],
                                                 guint8 nonce[12]);

/* the sending side. @sender (its key pair; the auth secret in it is not used) and @salt are NULL for new random
 * ones (the test vector gives them); @padding is the zero bytes added after the delimiter */
GBytes *_vibeos_webpush_encrypt (const guint8 receiver_public[VIBEOS_WEBPUSH_PUBLIC_KEY],
                                 const guint8 auth[VIBEOS_WEBPUSH_AUTH], const guint8 *plaintext, gsize length,
                                 const VibeosWebPushKeys *sender, const guint8 *salt, gsize padding, GError **error);
/* "vapid t=<JWT>, k=<public key>": an ES256 JWT for @audience (the endpoint's scheme and host), valid until
 * @expires (Unix seconds), with the contact @subject ("mailto:..." or "https://...") */
char   *_vibeos_webpush_vapid (const guint8 private_key[VIBEOS_WEBPUSH_PRIVATE_KEY],
                               const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY], const char *audience,
                               const char *subject, gint64 expires, GError **error);
/* checks a JWT's ES256 signature with the key: what a push service that enforces VAPID does */
gboolean _vibeos_webpush_vapid_verify (const char *jwt, const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY],
                                       GError **error);

G_END_DECLS
