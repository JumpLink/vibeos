/* Web Push decryption (RFC 8291) and the receiver's keys, on GnuTLS: see vibeos-webpush.h.
 *
 * With the receiver's key pair (ua_private, ua_public), its auth secret, and from the body the salt and the sender's
 * public key (as_public):
 *   ecdh_secret = ECDH (ua_private, as_public)
 *   IKM   = HKDF (salt = auth, IKM = ecdh_secret, info = "WebPush: info" 0x00 ua_public as_public, 32 bytes)
 *   CEK   = HKDF (salt = salt, IKM, info = "Content-Encoding: aes128gcm" 0x00, 16 bytes)
 *   NONCE = HKDF (salt = salt, IKM, info = "Content-Encoding: nonce" 0x00, 12 bytes)
 * and the record is AES-128-GCM with CEK and NONCE: the plaintext, a delimiter 0x02, then zeros. */
#include <string.h>

#include <gio/gio.h>
#include <gnutls/abstract.h>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include "vibeos-webpush.h"

char *
_vibeos_base64url_encode (const guint8 *data, gsize length)
{
  char *text = g_base64_encode (data, length), *padding;

  g_strdelimit (text, "+", '-');
  g_strdelimit (text, "/", '_');
  if ((padding = strchr (text, '=')))
    *padding = '\0';
  return text;
}

guint8 *
_vibeos_base64url_decode (const char *text, gsize *length)
{
  gsize n = text ? strlen (text) : 0;
  g_autofree char *standard = NULL;

  *length = 0;
  if (!text || n % 4 == 1)
    return NULL;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isalnum (text[i]) && text[i] != '-' && text[i] != '_')
      return NULL;
  standard = g_strnfill (n + (4 - n % 4) % 4, '=');
  memcpy (standard, text, n);
  g_strdelimit (standard, "-", '+');
  g_strdelimit (standard, "_", '/');
  return g_base64_decode (standard, length);
}

static gboolean
failed (GError **error, int code, const char *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what, gnutls_strerror (code));
  return FALSE;
}

/* a GnuTLS number, which may come with fewer bytes (or a leading zero), right-aligned into @size bytes */
static gboolean
copy_number (const gnutls_datum_t *datum, guint8 *out, gsize size)
{
  const guint8 *data = datum->data;
  gsize length = datum->size;

  while (length > size && *data == 0)
    data++, length--;
  if (length > size)
    return FALSE;
  memset (out, 0, size - length);
  memcpy (out + size - length, data, length);
  return TRUE;
}

static gboolean
import_private (gnutls_privkey_t key, const guint8 private_key[VIBEOS_WEBPUSH_PRIVATE_KEY],
                const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY], GError **error)
{
  gnutls_datum_t x = { (guint8 *) public_key + 1, 32 }, y = { (guint8 *) public_key + 33, 32 },
                 k = { (guint8 *) private_key, VIBEOS_WEBPUSH_PRIVATE_KEY };
  int code;

  if (public_key[0] != 0x04)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not an uncompressed P-256 point");
      return FALSE;
    }
  if ((code = gnutls_privkey_import_ecc_raw (key, GNUTLS_ECC_CURVE_SECP256R1, &x, &y, &k)) < 0)
    return failed (error, code, "the private key");
  if ((code = gnutls_privkey_verify_params (key)) < 0)
    return failed (error, code, "the private key");
  return TRUE;
}

static gboolean
import_public (gnutls_pubkey_t key, const guint8 public_key[VIBEOS_WEBPUSH_PUBLIC_KEY], GError **error)
{
  gnutls_datum_t x = { (guint8 *) public_key + 1, 32 }, y = { (guint8 *) public_key + 33, 32 };
  int code;

  if (public_key[0] != 0x04)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not an uncompressed P-256 point");
      return FALSE;
    }
  if ((code = gnutls_pubkey_import_ecc_raw (key, GNUTLS_ECC_CURVE_SECP256R1, &x, &y)) < 0)
    return failed (error, code, "the public key");
  return TRUE;
}

gboolean
_vibeos_webpush_generate (VibeosWebPushKeys *keys, GError **error)
{
  gnutls_privkey_t key = NULL;
  gnutls_datum_t x = { 0 }, y = { 0 }, k = { 0 };
  gnutls_ecc_curve_t curve;
  gboolean ok = FALSE;
  int code;

  if ((code = gnutls_privkey_init (&key)) < 0
      || (code = gnutls_privkey_generate2 (key, GNUTLS_PK_ECDSA, GNUTLS_CURVE_TO_BITS (GNUTLS_ECC_CURVE_SECP256R1),
                                           0, NULL, 0)) < 0
      || (code = gnutls_privkey_export_ecc_raw2 (key, &curve, &x, &y, &k, GNUTLS_EXPORT_FLAG_NO_LZ)) < 0)
    failed (error, code, "a new key pair");
  else if (!copy_number (&k, keys->private_key, 32) || !copy_number (&x, keys->public_key + 1, 32)
           || !copy_number (&y, keys->public_key + 33, 32))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "a new key pair: a number too long for P-256");
  else if ((code = gnutls_rnd (GNUTLS_RND_KEY, keys->auth, sizeof keys->auth)) < 0)
    failed (error, code, "a new auth secret");
  else
    {
      keys->public_key[0] = 0x04;
      ok = TRUE;
    }
  if (k.data)
    gnutls_memset (k.data, 0, k.size);
  gnutls_free (x.data);
  gnutls_free (y.data);
  gnutls_free (k.data);
  gnutls_privkey_deinit (key);
  return ok;
}

gboolean
_vibeos_webpush_check (const VibeosWebPushKeys *keys, GError **error)
{
  gnutls_privkey_t key = NULL;
  gboolean ok;

  gnutls_privkey_init (&key);
  ok = import_private (key, keys->private_key, keys->public_key, error);
  gnutls_privkey_deinit (key);
  return ok;
}

/* HKDF-SHA-256 (RFC 5869), up to 32 bytes: the extract and one round of the expand */
static void
hkdf (const guint8 *salt, gsize salt_length, const guint8 *ikm, gsize ikm_length, const guint8 *info,
      gsize info_length, guint8 *out, gsize out_length)
{
  guint8 prk[32], okm[32];
  gsize size = sizeof prk;
  g_autoptr(GHmac) extract = g_hmac_new (G_CHECKSUM_SHA256, salt, salt_length);
  g_autoptr(GHmac) expand = NULL;
  const guint8 one = 0x01;

  g_assert (out_length <= sizeof okm);
  g_hmac_update (extract, ikm, ikm_length);
  g_hmac_get_digest (extract, prk, &size);
  expand = g_hmac_new (G_CHECKSUM_SHA256, prk, sizeof prk);
  g_hmac_update (expand, info, info_length);
  g_hmac_update (expand, &one, 1);
  size = sizeof okm;
  g_hmac_get_digest (expand, okm, &size);
  memcpy (out, okm, out_length);
  memset (prk, 0, sizeof prk);
  memset (okm, 0, sizeof okm);
}

void
_vibeos_webpush_derive (const guint8 *ua_public, const guint8 *as_public, const guint8 *ecdh, const guint8 *auth,
                        const guint8 *salt, guint8 cek[16], guint8 nonce[12])
{
  static const char key_label[] = "WebPush: info";            /* and its 0x00 */
  static const char cek_info[] = "Content-Encoding: aes128gcm";
  static const char nonce_info[] = "Content-Encoding: nonce";
  guint8 key_info[sizeof key_label + 2 * VIBEOS_WEBPUSH_PUBLIC_KEY], ikm[32];

  memcpy (key_info, key_label, sizeof key_label);
  memcpy (key_info + sizeof key_label, ua_public, VIBEOS_WEBPUSH_PUBLIC_KEY);
  memcpy (key_info + sizeof key_label + VIBEOS_WEBPUSH_PUBLIC_KEY, as_public, VIBEOS_WEBPUSH_PUBLIC_KEY);
  hkdf (auth, VIBEOS_WEBPUSH_AUTH, ecdh, 32, key_info, sizeof key_info, ikm, sizeof ikm);
  hkdf (salt, 16, ikm, sizeof ikm, (const guint8 *) cek_info, sizeof cek_info, cek, 16);
  hkdf (salt, 16, ikm, sizeof ikm, (const guint8 *) nonce_info, sizeof nonce_info, nonce, 12);
  memset (ikm, 0, sizeof ikm);
}

gboolean
_vibeos_webpush_ecdh (const guint8 *private_key, const guint8 *own_public, const guint8 *peer_public,
                      guint8 secret[32], GError **error)
{
  gnutls_privkey_t own = NULL;
  gnutls_pubkey_t peer = NULL;
  gnutls_datum_t shared = { 0 };
  gboolean ok = FALSE;
  int code;

  gnutls_privkey_init (&own);
  gnutls_pubkey_init (&peer);
  /* the import checks the point: on the curve, in range, not at infinity (RFC 8291 §7) */
  if (import_private (own, private_key, own_public, error) && import_public (peer, peer_public, error))
    {
      if ((code = gnutls_privkey_derive_secret (own, peer, NULL, &shared, 0)) < 0)
        failed (error, code, "ECDH");
      else if (!copy_number (&shared, secret, 32))
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "ECDH: not a P-256 secret");
      else
        ok = TRUE;
    }
  if (shared.data)
    gnutls_memset (shared.data, 0, shared.size);
  gnutls_free (shared.data);
  gnutls_pubkey_deinit (peer);
  gnutls_privkey_deinit (own);
  return ok;
}

GBytes *
_vibeos_webpush_decrypt (const VibeosWebPushKeys *keys, const guint8 *body, gsize length, GError **error)
{
  guint8 ecdh[32], cek[16], nonce[12];
  const guint8 *salt = body, *as_public = body + 21, *record = body + VIBEOS_WEBPUSH_HEADER;
  gsize record_length = length - VIBEOS_WEBPUSH_HEADER, plain_length;
  guint32 rs;
  gnutls_aead_cipher_hd_t cipher = NULL;
  gnutls_datum_t key = { cek, sizeof cek };
  g_autofree guint8 *plain = NULL;
  int code;

  /* the header: the salt, the record size, the key id (the sender's public key, 65 bytes), then one record of at
   * least the delimiter and the tag */
  if (length < VIBEOS_WEBPUSH_HEADER + 1 + 16 || length > VIBEOS_WEBPUSH_MAX_BODY || body[20] != 65)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not an aes128gcm push message (%" G_GSIZE_FORMAT
                   " bytes)", length);
      return NULL;
    }
  rs = (guint32) body[16] << 24 | (guint32) body[17] << 16 | (guint32) body[18] << 8 | body[19];
  if (rs < 18 || record_length > rs)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "more than one record (record size %u)", rs);
      return NULL;
    }

  if (!_vibeos_webpush_ecdh (keys->private_key, keys->public_key, as_public, ecdh, error))
    return NULL;
  _vibeos_webpush_derive (keys->public_key, as_public, ecdh, keys->auth, salt, cek, nonce);
  memset (ecdh, 0, sizeof ecdh);

  plain_length = record_length;
  plain = g_malloc (plain_length);
  if ((code = gnutls_aead_cipher_init (&cipher, GNUTLS_CIPHER_AES_128_GCM, &key)) < 0
      || (code = gnutls_aead_cipher_decrypt (cipher, nonce, sizeof nonce, NULL, 0, 16, record, record_length, plain,
                                             &plain_length)) < 0)
    {
      memset (cek, 0, sizeof cek);
      if (cipher)
        gnutls_aead_cipher_deinit (cipher);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "cannot decrypt: %s", gnutls_strerror (code));
      return NULL;
    }
  memset (cek, 0, sizeof cek);
  gnutls_aead_cipher_deinit (cipher);

  /* the padding: zeros from the end, then the delimiter, which for the last (the only) record is 0x02 */
  while (plain_length > 0 && plain[plain_length - 1] == 0)
    plain_length--;
  if (plain_length == 0 || plain[plain_length - 1] != 0x02)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the padding delimiter is not 0x02");
      return NULL;
    }
  return g_bytes_new (plain, plain_length - 1);
}
