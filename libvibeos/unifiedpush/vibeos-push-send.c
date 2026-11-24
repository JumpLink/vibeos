/* vibeos-push-send: sends a Web Push message as an app server does: encrypted for the app's keys (RFC 8291,
 * aes128gcm) and signed with a VAPID key (RFC 8292), POSTed to the app's endpoint. For trying push by hand
 * (tools/unifiedpush/e2e.sh). Not installed.
 *
 *   vibeos-push-send --vapid-key FILE --print-public-key
 *   vibeos-push-send --endpoint URL --p256dh KEY --auth SECRET --vapid-key FILE [--size N | TEXT]
 *
 * The VAPID key file (0600) holds the private key and the public key, base64url, one to a line; it is made when it
 * does not exist. --size sends N bytes of text instead of TEXT. A message larger than one Web Push message can be
 * (3993 bytes, 4096 encrypted) is refused unless --too-big is given (to try what the server and the receiver make of
 * it). Exits 0 when the server took the message (2xx). */
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "vibeos-webpush.h"

static gboolean
load_vapid (const char *path, VibeosWebPushKeys *keys, GError **error)
{
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;
  g_autofree guint8 *private_key = NULL, *public_key = NULL;
  gsize private_length = 0, public_length = 0;
  g_autofree char *private_text = NULL, *public_text = NULL, *data = NULL;

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      if (!_vibeos_webpush_generate (keys, error))
        return FALSE;
      private_text = _vibeos_base64url_encode (keys->private_key, sizeof keys->private_key);
      public_text = _vibeos_base64url_encode (keys->public_key, sizeof keys->public_key);
      data = g_strdup_printf ("%s\n%s\n", private_text, public_text);
      return g_file_set_contents_full (path, data, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    }
  if (!g_file_get_contents (path, &contents, NULL, error))
    return FALSE;
  lines = g_strsplit (contents, "\n", 3);
  if (g_strv_length (lines) < 2
      || !(private_key = _vibeos_base64url_decode (lines[0], &private_length)) || private_length != 32
      || !(public_key = _vibeos_base64url_decode (lines[1], &public_length)) || public_length != 65)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: not a VAPID key file", path);
      return FALSE;
    }
  memcpy (keys->private_key, private_key, 32);
  memcpy (keys->public_key, public_key, 65);
  memset (keys->auth, 0, sizeof keys->auth);
  return _vibeos_webpush_check (keys, error);
}

static guint8 *
decode (const char *name, const char *text, gsize size)
{
  gsize length = 0;
  guint8 *raw = text ? _vibeos_base64url_decode (text, &length) : NULL;

  if (!raw || length != size)
    {
      g_printerr ("--%s: not %" G_GSIZE_FORMAT " bytes of base64url\n", name, size);
      g_free (raw);
      return NULL;
    }
  return raw;
}

int
main (int argc, char **argv)
{
  char *endpoint = NULL, *p256dh_text = NULL, *auth_text = NULL, *vapid_path = NULL, *subject = NULL;
  int size = -1, ttl = 86400;
  gboolean print_public = FALSE, too_big = FALSE;
  const GOptionEntry entries[] = {
    { "endpoint", 0, 0, G_OPTION_ARG_STRING, &endpoint, "The app's endpoint", "URL" },
    { "p256dh", 0, 0, G_OPTION_ARG_STRING, &p256dh_text, "The app's public key", "KEY" },
    { "auth", 0, 0, G_OPTION_ARG_STRING, &auth_text, "The app's auth secret", "SECRET" },
    { "vapid-key", 0, 0, G_OPTION_ARG_FILENAME, &vapid_path, "The app server's VAPID key file", "FILE" },
    { "subject", 0, 0, G_OPTION_ARG_STRING, &subject, "The VAPID subject (mailto:push@example.net)", "URI" },
    { "print-public-key", 0, 0, G_OPTION_ARG_NONE, &print_public, "Print the VAPID public key", NULL },
    { "size", 0, 0, G_OPTION_ARG_INT, &size, "Send this many bytes of text", "BYTES" },
    { "ttl", 0, 0, G_OPTION_ARG_INT, &ttl, "Seconds the server keeps it (86400)", "SECONDS" },
    { "too-big", 0, 0, G_OPTION_ARG_NONE, &too_big, "Send it even if it is too big for Web Push", NULL },
    { NULL }
  };
  g_autoptr(GOptionContext) context = g_option_context_new ("[TEXT]");
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *p256dh = NULL, *auth = NULL;
  g_autofree char *text = NULL, *header = NULL, *audience = NULL;
  g_autoptr(GUri) uri = NULL;
  g_autoptr(GBytes) body = NULL, reply = NULL;
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) message = NULL;
  g_autofree char *ttl_text = NULL;
  VibeosWebPushKeys vapid;
  guint status;

  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }
  if (!vapid_path)
    {
      g_printerr ("--vapid-key is needed\n");
      return 2;
    }
  if (!load_vapid (vapid_path, &vapid, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  if (print_public)
    {
      g_autofree char *k = _vibeos_base64url_encode (vapid.public_key, sizeof vapid.public_key);
      printf ("%s\n", k);
      return 0;
    }
  if (!endpoint || !(p256dh = decode ("p256dh", p256dh_text, 65)) || !(auth = decode ("auth", auth_text, 16)))
    {
      if (!endpoint)
        g_printerr ("--endpoint, --p256dh and --auth are needed\n");
      return 2;
    }
  if (!(uri = g_uri_parse (endpoint, G_URI_FLAGS_ENCODED, &error)) || !g_uri_get_host (uri))
    {
      g_printerr ("--endpoint: %s\n", error ? error->message : "no host");
      return 2;
    }
  if (size >= 0)
    {
      GString *made = g_string_sized_new (size);
      for (int i = 0; i < size; i++)
        g_string_append_c (made, "0123456789abcdefghijklmnopqrstuvwxyz"[i % 36]);
      text = g_string_free (made, FALSE);
    }
  else
    text = g_strdup (argc > 1 ? argv[1] : "Hello from vibeos-push-send");
  if (strlen (text) > VIBEOS_WEBPUSH_MAX_PLAINTEXT && !too_big)
    {
      g_printerr ("%" G_GSIZE_FORMAT " bytes: a Web Push message carries %d at most (--too-big sends it anyway)\n",
                  strlen (text), VIBEOS_WEBPUSH_MAX_PLAINTEXT);
      return 2;
    }

  if (!(body = _vibeos_webpush_encrypt (p256dh, auth, (const guint8 *) text, strlen (text), NULL, NULL, 0, &error)))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  /* VAPID's audience is the endpoint's origin; the token lasts 12 hours (at most 24 are allowed) */
  audience = g_uri_get_port (uri) > 0
               ? g_strdup_printf ("%s://%s:%d", g_uri_get_scheme (uri), g_uri_get_host (uri), g_uri_get_port (uri))
               : g_strdup_printf ("%s://%s", g_uri_get_scheme (uri), g_uri_get_host (uri));
  if (!(header = _vibeos_webpush_vapid (vapid.private_key, vapid.public_key, audience,
                                        subject ? subject : "mailto:push@example.net",
                                        g_get_real_time () / G_USEC_PER_SEC + 12 * 3600, &error)))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  memset (&vapid, 0, sizeof vapid);

  session = soup_session_new_with_options ("user-agent", "vibeos-push-send", "timeout", 30, NULL);
  message = soup_message_new_from_uri ("POST", uri);
  ttl_text = g_strdup_printf ("%d", ttl);
  soup_message_headers_replace (soup_message_get_request_headers (message), "Authorization", header);
  soup_message_headers_replace (soup_message_get_request_headers (message), "Content-Encoding", "aes128gcm");
  soup_message_headers_replace (soup_message_get_request_headers (message), "TTL", ttl_text);
  soup_message_set_request_body_from_bytes (message, "application/octet-stream", body);
  reply = soup_session_send_and_read (session, message, NULL, &error);
  if (!reply)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  status = soup_message_get_status (message);
  printf ("%u %" G_GSIZE_FORMAT " bytes (%zu of text)\n", status, g_bytes_get_size (body), strlen (text));
  if (status < 200 || status >= 300)
    {
      g_printerr ("%.*s\n", (int) MIN (g_bytes_get_size (reply), 400), (const char *) g_bytes_get_data (reply, NULL));
      return 1;
    }
  return 0;
}
