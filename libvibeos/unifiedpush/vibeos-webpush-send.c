/* The sending side of Web Push, for the tests and the test sender (not in libvibeos): RFC 8291 encryption and the
 * VAPID header of RFC 8292. See vibeos-webpush.h. */
#include <string.h>

#include <gio/gio.h>
#include <gnutls/abstract.h>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include "vibeos-webpush.h"

GBytes *
_vibeos_webpush_encrypt (const guint8 receiver_public[VIBEOS_WEBPUSH_PUBLIC_KEY],
                         const guint8 auth[VIBEOS_WEBPUSH_AUTH], const guint8 *plaintext, gsize length,
                         const VibeosWebPushKeys *sender, const guint8 *salt, gsize padding, GError **error)
{
  VibeosWebPushKeys fresh;
  guint8 header[VIBEOS_WEBPUSH_HEADER], ecdh[32], cek[16], nonce[12];
  gsize record = length + 1 + padding + 16;
  /* the record size must be larger than the one record (RFC 8291 §4): 4096, as in its example, unless the record is
   * larger still (a test of the limits) */
  guint32 rs = MAX (4096, record + 1);
  gnutls_aead_cipher_hd_t cipher = NULL;
  gnutls_datum_t key = { cek, sizeof cek };
  g_autofree guint8 *plain = NULL, *body = NULL;
  gsize body_length = sizeof header + record, out_length = record;
  int code;

  if (!sender)
    {
      if (!_vibeos_webpush_generate (&fresh, error))
        return NULL;
      sender = &fresh;
    }
  if (salt)
    memcpy (header, salt, 16);
  else
    gnutls_rnd (GNUTLS_RND_NONCE, header, 16);
  header[16] = rs >> 24;
  header[17] = rs >> 16;
  header[18] = rs >> 8;
  header[19] = rs;
  header[20] = VIBEOS_WEBPUSH_PUBLIC_KEY;
  memcpy (header + 21, sender->public_key, VIBEOS_WEBPUSH_PUBLIC_KEY);

  if (!_vibeos_webpush_ecdh (sender->private_key, sender->public_key, receiver_public, ecdh, error))
    return NULL;
  _vibeos_webpush_derive (receiver_public, sender->public_key, ecdh, auth, header, cek, nonce);
  memset (ecdh, 0, sizeof ecdh);
  memset (&fresh, 0, sizeof fresh);

  plain = g_malloc0 (length + 1 + padding);
  memcpy (plain, plaintext, length);
  plain[length] = 0x02;
  body = g_malloc (body_length);
  memcpy (body, header, sizeof header);
  if ((code = gnutls_aead_cipher_init (&cipher, GNUTLS_CIPHER_AES_128_GCM, &key)) < 0
      || (code = gnutls_aead_cipher_encrypt (cipher, nonce, sizeof nonce, NULL, 0, 16, plain, length + 1 + padding,
                                             body + sizeof header, &out_length)) < 0)
    {
      if (cipher)
        gnutls_aead_cipher_deinit (cipher);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "AES-GCM: %s", gnutls_strerror (code));
      return NULL;
    }
  gnutls_aead_cipher_deinit (cipher);
  memset (cek, 0, sizeof cek);
  return g_bytes_new_take (g_steal_pointer (&body), sizeof header + out_length);
}

static char *
base64url_string (const char *text)
{
  return _vibeos_base64url_encode ((const guint8 *) text, strlen (text));
}

char *
_vibeos_webpush_vapid (const guint8 private_key[VIBEOS_WEBPUSH_PRIVATE_KEY],
                       const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY], const char *audience, const char *subject,
                       gint64 expires, GError **error)
{
  g_autofree char *header = base64url_string ("{\"typ\":\"JWT\",\"alg\":\"ES256\"}");
  g_autofree char *claims_json = NULL, *claims = NULL, *input = NULL, *signature = NULL, *k = NULL;
  g_autofree char *audience_json = NULL, *subject_json = NULL;
  gnutls_privkey_t key = NULL;
  gnutls_datum_t x = { (guint8 *) public_key + 1, 32 }, y = { (guint8 *) public_key + 33, 32 },
                 d = { (guint8 *) private_key, 32 }, data, sig = { 0 }, r = { 0 }, s = { 0 };
  guint8 raw[64];
  int code;

  /* the claims, as JSON strings: quotes and backslashes escaped (neither belongs in a URL or a mailto: anyway) */
  audience_json = g_strescape (audience, NULL);
  subject_json = g_strescape (subject, NULL);
  claims_json = g_strdup_printf ("{\"aud\":\"%s\",\"exp\":%" G_GINT64_FORMAT ",\"sub\":\"%s\"}", audience_json,
                                 expires, subject_json);
  claims = base64url_string (claims_json);
  input = g_strconcat (header, ".", claims, NULL);
  data.data = (guint8 *) input;
  data.size = strlen (input);

  gnutls_privkey_init (&key);
  if ((code = gnutls_privkey_import_ecc_raw (key, GNUTLS_ECC_CURVE_SECP256R1, &x, &y, &d)) < 0
      || (code = gnutls_privkey_sign_data2 (key, GNUTLS_SIGN_ECDSA_SHA256, 0, &data, &sig)) < 0
      || (code = gnutls_decode_rs_value (&sig, &r, &s)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "VAPID: %s", gnutls_strerror (code));
      gnutls_free (sig.data);
      gnutls_privkey_deinit (key);
      return NULL;
    }
  /* JWS wants r and s as two 32-byte numbers, not DER */
  memset (raw, 0, sizeof raw);
  memcpy (raw + 32 - MIN (r.size, 32), r.data + (r.size > 32 ? r.size - 32 : 0), MIN (r.size, 32));
  memcpy (raw + 64 - MIN (s.size, 32), s.data + (s.size > 32 ? s.size - 32 : 0), MIN (s.size, 32));
  gnutls_free (sig.data);
  gnutls_free (r.data);
  gnutls_free (s.data);
  gnutls_privkey_deinit (key);

  signature = _vibeos_base64url_encode (raw, sizeof raw);
  k = _vibeos_base64url_encode (public_key, VIBEOS_WEBPUSH_PUBLIC_KEY);
  return g_strdup_printf ("vapid t=%s.%s, k=%s", input, signature, k);
}

gboolean
_vibeos_webpush_vapid_verify (const char *jwt, const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY], GError **error)
{
  const char *dot = strrchr (jwt, '.');
  g_autofree guint8 *raw = NULL;
  gsize raw_length = 0;
  gnutls_pubkey_t key = NULL;
  gnutls_datum_t x = { (guint8 *) public_key + 1, 32 }, y = { (guint8 *) public_key + 33, 32 }, data, sig = { 0 },
                 r, s;
  int code;

  if (!dot || !(raw = _vibeos_base64url_decode (dot + 1, &raw_length)) || raw_length != 64)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not an ES256 JWT");
      return FALSE;
    }
  data.data = (guint8 *) jwt;
  data.size = dot - jwt;
  r.data = raw;
  r.size = 32;
  s.data = raw + 32;
  s.size = 32;
  gnutls_pubkey_init (&key);
  if ((code = gnutls_pubkey_import_ecc_raw (key, GNUTLS_ECC_CURVE_SECP256R1, &x, &y)) < 0
      || (code = gnutls_encode_rs_value (&sig, &r, &s)) < 0
      || (code = gnutls_pubkey_verify_data2 (key, GNUTLS_SIGN_ECDSA_SHA256, 0, &data, &sig)) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "VAPID: %s", gnutls_strerror (code));
      gnutls_free (sig.data);
      gnutls_pubkey_deinit (key);
      return FALSE;
    }
  gnutls_free (sig.data);
  gnutls_pubkey_deinit (key);
  return TRUE;
}
