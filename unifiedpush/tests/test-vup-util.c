/* vibeos-unifiedpush's helpers: topics, the redaction of secrets, and the checks of what apps and the server send */
#include <string.h>

#include "vup.h"

static void
test_topic (void)
{
  g_autoptr(GHashTable) topics = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint counts[64] = { 0 };
  const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  for (guint i = 0; i < 4000; i++)
    {
      char *topic = vup_new_topic ();

      g_assert_cmpuint (strlen (topic), ==, VUP_TOPIC_LENGTH);
      g_assert_true (g_str_has_prefix (topic, "up"));
      for (const char *c = topic + 2; *c; c++)
        {
          const char *at = strchr (alphabet, *c);
          g_assert_nonnull (at);
          counts[at - alphabet]++;
        }
      g_assert_false (g_hash_table_contains (topics, topic));
      g_hash_table_add (topics, topic);
    }
  /* 48000 characters over 64: 750 each on average; every one turns up, none dominates (9 bytes make exactly 12
   * characters, all 6 bits of each random) */
  for (guint i = 0; i < 64; i++)
    {
      g_assert_cmpuint (counts[i], >, 500);
      g_assert_cmpuint (counts[i], <, 1000);
    }
}

static void
test_redact (void)
{
  g_assert_cmpstr (vup_redact ("upAbCdEfGhIjKl"), ==, "upAb...");
  g_assert_cmpstr (vup_redact ("0123456789ab"), ==, "0123...");
  g_assert_cmpstr (vup_redact ("short"), ==, "...");
  g_assert_cmpstr (vup_redact (NULL), ==, "(none)");
  /* several in one line */
  g_assert_cmpstr (vup_redact ("aaaaaaaaaaaaaa"), !=, vup_redact ("bbbbbbbbbbbbbb"));
}

static void
test_vapid (void)
{
  /* RFC 8291's example keys: uncompressed points */
  g_assert_true (vup_vapid_is_valid ("BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"));
  g_assert_true (vup_vapid_is_valid ("BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"));
  /* one character short, one too many, standard base64, padding, not 0x04 first */
  g_assert_false (vup_vapid_is_valid ("BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A"));
  g_assert_false (vup_vapid_is_valid ("BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8A"));
  g_assert_false (vup_vapid_is_valid ("BP4z9KsN6nGRTbVYI/c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"));
  g_assert_false (vup_vapid_is_valid ("BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A="));
  g_assert_false (vup_vapid_is_valid ("AP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"));
  g_assert_false (vup_vapid_is_valid (""));
  g_assert_false (vup_vapid_is_valid (NULL));
}

static void
test_utf8_within (void)
{
  g_autofree char *hundred = g_strnfill (100, 'a'), *more = g_strnfill (101, 'a');

  g_assert_true (vup_utf8_within (hundred, 100));
  g_assert_false (vup_utf8_within (more, 100));
  g_assert_true (vup_utf8_within ("ünïcödé", 100));
  g_assert_false (vup_utf8_within ("\xff\xfe", 100));
  g_assert_false (vup_utf8_within (NULL, 100));
}

static void
test_server (void)
{
  static const struct { const char *in, *out; } cases[] = {
    { "https://push.vibeos.top", "https://push.vibeos.top" },
    { "https://push.vibeos.top/", "https://push.vibeos.top" },
    { "https://example.org:8443/ntfy//", "https://example.org:8443/ntfy" },
    { "http://192.168.1.2:8080", "http://192.168.1.2:8080" },
    { "ftp://push.vibeos.top", NULL },
    { "https://", NULL },
    { "push.vibeos.top", NULL },
    { "https://user@push.vibeos.top", NULL },
    { "https://push.vibeos.top/?x=1", NULL },
    { "https://push.vibeos.top/#a", NULL },
    { "https://push.vibeos.top/a,b", NULL },
    { "", NULL },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *out = vup_normalize_server (cases[i].in);
      g_assert_cmpstr (out, ==, cases[i].out);
    }
}

static void
test_decode (void)
{
  g_autoptr(GBytes) text = vup_decode_message ("hello text", NULL);
  g_autoptr(GBytes) binary = vup_decode_message ("AAEC/w==", "base64");
  g_autoptr(GBytes) broken = vup_decode_message ("AAE*/w==", "base64");
  g_autoptr(GBytes) unknown = vup_decode_message ("AAEC", "gzip");
  const guint8 expected[] = { 0, 1, 2, 255 };
  gsize size;
  const guint8 *data;

  g_assert_cmpmem (g_bytes_get_data (text, NULL), g_bytes_get_size (text), "hello text", 10);
  data = g_bytes_get_data (binary, &size);
  g_assert_cmpmem (data, size, expected, sizeof expected);
  g_assert_null (broken);
  g_assert_null (unknown);
}

static void
test_registration_id (void)
{
  g_autofree char *a = vup_registration_id ("token-a"), *b = vup_registration_id ("token-b"),
                  *again = vup_registration_id ("token-a");

  g_assert_cmpuint (strlen (a), ==, 16);
  g_assert_cmpstr (a, ==, again);
  g_assert_cmpstr (a, !=, b);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unifiedpush/topic", test_topic);
  g_test_add_func ("/unifiedpush/redact", test_redact);
  g_test_add_func ("/unifiedpush/vapid", test_vapid);
  g_test_add_func ("/unifiedpush/utf8-within", test_utf8_within);
  g_test_add_func ("/unifiedpush/server", test_server);
  g_test_add_func ("/unifiedpush/decode", test_decode);
  g_test_add_func ("/unifiedpush/registration-id", test_registration_id);
  return g_test_run ();
}
