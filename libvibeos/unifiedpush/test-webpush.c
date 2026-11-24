/* RFC 8291's example (§5 and Appendix A) both ways, what a receiver must refuse, fresh keys, and VAPID */
#include <string.h>

#include <gio/gio.h>

#include "vibeos-webpush.h"

#define BODY "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml" \
             "mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPT" \
             "pK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN"
#define PLAINTEXT "When I grow up, I want to be a watermelon"
#define RECEIVER_PRIVATE "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94"
#define RECEIVER_PUBLIC "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
#define SENDER_PRIVATE "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw"
#define SENDER_PUBLIC "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"
#define AUTH "BTBZMqHH6r4Tts7J_aSIgg"
#define SALT "DGv6ra1nlYgDCS1FRnbzlw"
#define ECDH_SECRET "kyrL1jIIOHEzg3sM2ZWRHDRB62YACZhhSlknJ672kSs"
#define CEK "oIhVW04MRdy2XN9CiKLxTg"
#define NONCE "4h_95klXJ5E_qnoN"

static void
decode_into (const char *text, guint8 *out, gsize size)
{
  gsize length = 0;
  g_autofree guint8 *raw = _vibeos_base64url_decode (text, &length);

  g_assert_nonnull (raw);
  g_assert_cmpuint (length, ==, size);
  memcpy (out, raw, size);
}

static GBytes *
decode_bytes (const char *text)
{
  gsize length = 0;
  guint8 *raw = _vibeos_base64url_decode (text, &length);

  g_assert_nonnull (raw);
  return g_bytes_new_take (raw, length);
}

static void
example_keys (VibeosWebPushKeys *receiver, VibeosWebPushKeys *sender)
{
  decode_into (RECEIVER_PRIVATE, receiver->private_key, 32);
  decode_into (RECEIVER_PUBLIC, receiver->public_key, 65);
  decode_into (AUTH, receiver->auth, 16);
  if (sender)
    {
      decode_into (SENDER_PRIVATE, sender->private_key, 32);
      decode_into (SENDER_PUBLIC, sender->public_key, 65);
    }
}

static void
assert_base64url (const guint8 *data, gsize size, const char *expected)
{
  g_autofree char *text = _vibeos_base64url_encode (data, size);

  g_assert_cmpstr (text, ==, expected);
}

/* Appendix A: the ECDH secret, the content encryption key and the nonce */
static void
test_intermediate (void)
{
  VibeosWebPushKeys receiver, sender;
  guint8 ecdh[32], ecdh_back[32], salt[16], cek[16], nonce[12];
  g_autoptr(GError) error = NULL;

  example_keys (&receiver, &sender);
  g_assert_true (_vibeos_webpush_ecdh (receiver.private_key, receiver.public_key, sender.public_key, ecdh, &error));
  g_assert_no_error (error);
  assert_base64url (ecdh, sizeof ecdh, ECDH_SECRET);
  /* the same from the sender's side */
  g_assert_true (_vibeos_webpush_ecdh (sender.private_key, sender.public_key, receiver.public_key, ecdh_back, &error));
  g_assert_cmpmem (ecdh, sizeof ecdh, ecdh_back, sizeof ecdh_back);

  decode_into (SALT, salt, sizeof salt);
  _vibeos_webpush_derive (receiver.public_key, sender.public_key, ecdh, receiver.auth, salt, cek, nonce);
  assert_base64url (cek, sizeof cek, CEK);
  assert_base64url (nonce, sizeof nonce, NONCE);
}

/* §5: the example message decrypts to its plaintext */
static void
test_decrypt (void)
{
  VibeosWebPushKeys receiver;
  g_autoptr(GBytes) body = decode_bytes (BODY);
  g_autoptr(GBytes) plain = NULL;
  g_autoptr(GError) error = NULL;

  example_keys (&receiver, NULL);
  g_assert_cmpuint (g_bytes_get_size (body), ==, 144);
  plain = _vibeos_webpush_decrypt (&receiver, g_bytes_get_data (body, NULL), g_bytes_get_size (body), &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (plain, NULL), g_bytes_get_size (plain), PLAINTEXT, strlen (PLAINTEXT));
}

/* §5 the other way: with the example's sender keys and salt, the encryption gives the example's body exactly */
static void
test_encrypt (void)
{
  VibeosWebPushKeys receiver, sender;
  guint8 salt[16];
  g_autoptr(GBytes) expected = decode_bytes (BODY);
  g_autoptr(GBytes) body = NULL;
  g_autoptr(GError) error = NULL;

  example_keys (&receiver, &sender);
  decode_into (SALT, salt, sizeof salt);
  body = _vibeos_webpush_encrypt (receiver.public_key, receiver.auth, (const guint8 *) PLAINTEXT, strlen (PLAINTEXT),
                                  &sender, salt, 0, &error);
  g_assert_no_error (error);
  g_assert_true (g_bytes_equal (body, expected));
}

static void
assert_refused (const VibeosWebPushKeys *keys, const guint8 *body, gsize length)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) plain = _vibeos_webpush_decrypt (keys, body, length, &error);

  g_assert_null (plain);
  g_assert_nonnull (error);
}

/* what a receiver must not accept */
static void
test_refused (void)
{
  VibeosWebPushKeys receiver, other;
  g_autoptr(GBytes) example = decode_bytes (BODY);
  gsize length = g_bytes_get_size (example);
  g_autofree guint8 *body = g_memdup2 (g_bytes_get_data (example, NULL), length);
  g_autofree guint8 *big = NULL;
  g_autoptr(GError) error = NULL;

  example_keys (&receiver, NULL);

  /* any byte of the ciphertext or the tag changed */
  for (gsize i = VIBEOS_WEBPUSH_HEADER; i < length; i += 7)
    {
      body[i] ^= 0x01;
      assert_refused (&receiver, body, length);
      body[i] ^= 0x01;
    }
  /* the salt changed */
  body[3] ^= 0x80;
  assert_refused (&receiver, body, length);
  body[3] ^= 0x80;
  /* the sender's key changed: no longer a point of the curve (RFC 8291 §7) */
  body[21 + 64] ^= 0x01;
  assert_refused (&receiver, body, length);
  body[21 + 64] ^= 0x01;
  /* a compressed point, a key id of another length */
  body[21] = 0x02;
  assert_refused (&receiver, body, length);
  body[21] = 0x04;
  body[20] = 33;
  assert_refused (&receiver, body, length);
  body[20] = 65;
  /* cut short, or only the header */
  assert_refused (&receiver, body, length - 1);
  assert_refused (&receiver, body, VIBEOS_WEBPUSH_HEADER + 16);
  assert_refused (&receiver, body, 10);
  /* a record size smaller than the record: more than one record */
  body[18] = 0x00;
  body[19] = 0x20;
  assert_refused (&receiver, body, length);
  body[18] = 0x10;
  body[19] = 0x00;
  /* another auth secret */
  other = receiver;
  other.auth[0] ^= 0x01;
  assert_refused (&other, body, length);
  /* unchanged, it still decrypts */
  g_autoptr(GBytes) plain = _vibeos_webpush_decrypt (&receiver, body, length, &error);
  g_assert_no_error (error);
  g_assert_nonnull (plain);

  /* more than 4096 bytes */
  big = g_malloc0 (VIBEOS_WEBPUSH_MAX_BODY + 1);
  memcpy (big, body, VIBEOS_WEBPUSH_HEADER);
  assert_refused (&receiver, big, VIBEOS_WEBPUSH_MAX_BODY + 1);
}

/* a delimiter other than 0x02: an encryption of the plaintext with 0x01 (another record would follow) */
static void
test_delimiter (void)
{
  VibeosWebPushKeys receiver, sender;
  g_autoptr(GBytes) body = NULL;
  g_autoptr(GError) error = NULL;
  guint8 plain[] = { 'h', 'i', 0x01 };

  example_keys (&receiver, &sender);
  /* the record "hi" 0x01 0x02 decrypts to "hi" 0x01: the delimiter is the 0x02 */
  body = _vibeos_webpush_encrypt (receiver.public_key, receiver.auth, plain, sizeof plain, &sender, NULL, 0, &error);
  g_assert_no_error (error);
  g_autoptr(GBytes) out = _vibeos_webpush_decrypt (&receiver, g_bytes_get_data (body, NULL),
                                                   g_bytes_get_size (body), &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (out, NULL), g_bytes_get_size (out), plain, sizeof plain);
}

/* new keys: valid, different each time, and they carry the largest message that fits in 4096 bytes */
static void
test_fresh_keys (void)
{
  VibeosWebPushKeys a, b;
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *text = g_malloc (VIBEOS_WEBPUSH_MAX_PLAINTEXT);
  g_autoptr(GBytes) body = NULL, plain = NULL, padded = NULL, padded_plain = NULL;

  g_assert_true (_vibeos_webpush_generate (&a, &error));
  g_assert_true (_vibeos_webpush_generate (&b, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (a.public_key[0], ==, 0x04);
  g_assert_true (_vibeos_webpush_check (&a, &error));
  g_assert_no_error (error);
  g_assert_true (memcmp (a.private_key, b.private_key, 32) != 0);
  g_assert_true (memcmp (a.auth, b.auth, 16) != 0);
  /* a public key that is not the private key's */
  memcpy (b.public_key, a.public_key, 65);
  g_assert_false (_vibeos_webpush_check (&b, &error));
  g_clear_error (&error);

  for (gsize i = 0; i < VIBEOS_WEBPUSH_MAX_PLAINTEXT; i++)
    text[i] = g_random_int_range (0, 256);
  body = _vibeos_webpush_encrypt (a.public_key, a.auth, text, VIBEOS_WEBPUSH_MAX_PLAINTEXT, NULL, NULL, 0, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_bytes_get_size (body), ==, VIBEOS_WEBPUSH_MAX_BODY);
  plain = _vibeos_webpush_decrypt (&a, g_bytes_get_data (body, NULL), g_bytes_get_size (body), &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (plain, NULL), g_bytes_get_size (plain), text, VIBEOS_WEBPUSH_MAX_PLAINTEXT);

  /* with padding */
  padded = _vibeos_webpush_encrypt (a.public_key, a.auth, text, 10, NULL, NULL, 100, &error);
  g_assert_cmpuint (g_bytes_get_size (padded), ==, VIBEOS_WEBPUSH_HEADER + 10 + 1 + 100 + 16);
  padded_plain = _vibeos_webpush_decrypt (&a, g_bytes_get_data (padded, NULL), g_bytes_get_size (padded), &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (padded_plain, NULL), g_bytes_get_size (padded_plain), text, 10);
}

static void
test_vapid (void)
{
  VibeosWebPushKeys server, other;
  g_autoptr(GError) error = NULL;
  g_autofree char *header = NULL, *jwt = NULL, *claims = NULL;
  g_auto(GStrv) parts = NULL;
  gsize length = 0;

  g_assert_true (_vibeos_webpush_generate (&server, &error));
  g_assert_true (_vibeos_webpush_generate (&other, &error));
  header = _vibeos_webpush_vapid (server.private_key, server.public_key, "https://push.example.net",
                                  "mailto:push@example.net", 1800000000, &error);
  g_assert_no_error (error);
  g_assert_true (g_str_has_prefix (header, "vapid t="));
  jwt = g_strndup (header + strlen ("vapid t="), strstr (header, ", k=") - header - strlen ("vapid t="));
  parts = g_strsplit (jwt, ".", -1);
  g_assert_cmpuint (g_strv_length (parts), ==, 3);
  claims = (char *) _vibeos_base64url_decode (parts[1], &length);
  claims = g_realloc (claims, length + 1);
  claims[length] = '\0';
  g_assert_cmpstr (claims, ==, "{\"aud\":\"https://push.example.net\",\"exp\":1800000000,\"sub\":\"mailto:push@example.net\"}");
  g_assert_true (_vibeos_webpush_vapid_verify (jwt, server.public_key, &error));
  g_assert_no_error (error);
  g_assert_false (_vibeos_webpush_vapid_verify (jwt, other.public_key, &error));
  g_clear_error (&error);
  jwt[10] = jwt[10] == 'A' ? 'B' : 'A';
  g_assert_false (_vibeos_webpush_vapid_verify (jwt, server.public_key, &error));
}

static void
test_base64url (void)
{
  gsize length = 0;
  g_autofree guint8 *ok = _vibeos_base64url_decode ("AAEC_w", &length);
  const guint8 expected[] = { 0, 1, 2, 255 };

  g_assert_cmpmem (ok, length, expected, sizeof expected);
  g_assert_null (_vibeos_base64url_decode ("AAEC/w", &length));     /* standard base64 */
  g_assert_null (_vibeos_base64url_decode ("AAEC_w==", &length));   /* padding */
  g_assert_null (_vibeos_base64url_decode ("AAECA", &length));      /* a length base64 cannot have */
  g_assert_null (_vibeos_base64url_decode (NULL, &length));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/webpush/rfc8291/intermediate", test_intermediate);
  g_test_add_func ("/webpush/rfc8291/decrypt", test_decrypt);
  g_test_add_func ("/webpush/rfc8291/encrypt", test_encrypt);
  g_test_add_func ("/webpush/refused", test_refused);
  g_test_add_func ("/webpush/delimiter", test_delimiter);
  g_test_add_func ("/webpush/fresh-keys", test_fresh_keys);
  g_test_add_func ("/webpush/vapid", test_vapid);
  g_test_add_func ("/webpush/base64url", test_base64url);
  return g_test_run ();
}
