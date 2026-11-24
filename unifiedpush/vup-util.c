/* vibeos-unifiedpush: topics, handles, checks and the redaction of secrets in the log (vup.h) */
#include <errno.h>
#include <string.h>
#include <sys/random.h>

#include "vup.h"

/* base64url (RFC 4648 §5) without padding */
static char *
base64url_encode (const guint8 *data, gsize length)
{
  char *text = g_base64_encode (data, length), *padding;

  g_strdelimit (text, "+", '-');
  g_strdelimit (text, "/", '_');
  if ((padding = strchr (text, '=')))
    *padding = '\0';
  return text;
}

/* "up" and 12 base64url characters from 9 bytes of getrandom (): 72 random bits. The push server (ntfy) counts a
 * topic of exactly this shape as a UnifiedPush one and charges what is sent to it to its subscriber */
char *
vup_new_topic (void)
{
  guint8 raw[VUP_TOPIC_RANDOM_BYTES];
  gsize got = 0;
  g_autofree char *text = NULL;

  while (got < sizeof raw)
    {
      ssize_t n = getrandom (raw + got, sizeof raw - got, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        g_error ("getrandom: %s", g_strerror (errno));
      got += n;
    }
  text = base64url_encode (raw, sizeof raw);
  memset (raw, 0, sizeof raw);
  return g_strconcat ("up", text, NULL);
}

/* the handle Settings knows a registration by: it gives nothing of the token away */
char *
vup_registration_id (const char *token)
{
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, token, -1);

  return g_strndup (sum, 16);
}

/* a secret for the log: its first 4 characters and "..." (none of a short one; ASCII, as the log may not be
 * UTF-8), from one of 8 buffers that the next calls reuse, so that a log line can show several */
const char *
vup_redact (const char *secret)
{
  static char buffers[8][16];
  static guint next;
  char *buffer = buffers[next++ % G_N_ELEMENTS (buffers)];

  if (!secret)
    return "(none)";
  if (strlen (secret) < 12)
    return "...";
  g_snprintf (buffer, sizeof buffers[0], "%.4s...", secret);
  return buffer;
}

static gboolean
is_base64url (const char *text)
{
  for (const char *c = text; *c; c++)
    if (!g_ascii_isalnum (*c) && *c != '-' && *c != '_')
      return FALSE;
  return TRUE;
}

/* an uncompressed P-256 point, base64url without padding: 87 characters, 65 bytes, the first one 0x04 */
gboolean
vup_vapid_is_valid (const char *vapid)
{
  g_autofree char *standard = NULL;
  g_autofree guchar *raw = NULL;
  gsize length = 0;

  if (!vapid || strlen (vapid) != VUP_VAPID_LENGTH || !is_base64url (vapid))
    return FALSE;
  standard = g_strconcat (vapid, "=", NULL);
  g_strdelimit (standard, "-", '+');
  g_strdelimit (standard, "_", '/');
  raw = g_base64_decode (standard, &length);
  return length == 65 && raw[0] == 0x04;
}

gboolean
vup_utf8_within (const char *text, gsize max_bytes)
{
  return text && strlen (text) <= max_bytes && g_utf8_validate (text, -1, NULL);
}

/* the push server's URL as the endpoints start: http or https, a host, a path without the last "/", no user,
 * query or fragment. NULL when it is not such a URL */
char *
vup_normalize_server (const char *url)
{
  g_autoptr(GUri) uri = NULL;
  const char *scheme;
  char *server;
  gsize length;

  if (!url || !(uri = g_uri_parse (url, G_URI_FLAGS_ENCODED, NULL)))
    return NULL;
  scheme = g_uri_get_scheme (uri);
  if ((strcmp (scheme, "https") != 0 && strcmp (scheme, "http") != 0) || !g_uri_get_host (uri)
      || !*g_uri_get_host (uri) || g_uri_get_userinfo (uri) || g_uri_get_query (uri) || g_uri_get_fragment (uri)
      || strpbrk (url, " \t\r\n,"))
    return NULL;
  server = g_strdup (url);
  length = strlen (server);
  while (length > 0 && server[length - 1] == '/')
    server[--length] = '\0';
  return server;
}

/* a message's body from the stream: ntfy sends a body that is not UTF-8 text in standard base64, with
 * "encoding": "base64"; NULL when that base64 is broken */
GBytes *
vup_decode_message (const char *text, const char *encoding)
{
  gsize length = strlen (text), decoded = 0;
  guchar *raw;

  if (!encoding || !*encoding)
    return g_bytes_new (text, length);
  if (g_strcmp0 (encoding, "base64") != 0 || length % 4 != 0)
    return NULL;
  for (gsize i = 0; i < length; i++)
    if (!g_ascii_isalnum (text[i]) && text[i] != '+' && text[i] != '/' && !(text[i] == '=' && i >= length - 2))
      return NULL;
  raw = g_base64_decode (text, &decoded);
  return g_bytes_new_take (raw, decoded);
}
