/* dictate-device: see dictate-device.h */
#include "dictate-device.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/random.h>

G_LOCK_DEFINE_STATIC (device);
static char *id, *secret, *model, *release;
static gboolean model_done;

/* `name` in `dir`: its first line if `valid` says so, else a new value from `make`, kept 0600 */
static char *
kept (const char *dir, const char *name, gboolean (*valid) (const char *), char *(*make) (void))
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *contents = NULL;
  char *value;

  if (g_file_get_contents (path, &contents, NULL, NULL) && valid (g_strstrip (contents)))
    return g_steal_pointer (&contents);
  value = make ();
  if (g_mkdir_with_parents (dir, 0700) != 0 ||
      !g_file_set_contents_full (path, value, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    g_warning ("could not keep %s in %s", name, dir);
  return value;
}

static gboolean
valid_uuid (const char *text)
{
  return g_uuid_string_is_valid (text);
}

static char *
make_uuid (void)
{
  return g_uuid_string_random ();
}

static gboolean
valid_secret (const char *text)
{
  if (strlen (text) != 64)
    return FALSE;
  for (const char *p = text; *p; p++)
    if (!g_ascii_isxdigit (*p) || g_ascii_isupper (*p))
      return FALSE;
  return TRUE;
}

static char *
make_secret (void)
{
  guint8 bytes[32];
  gsize got = 0;
  GString *hex = g_string_sized_new (64);

  /* the kernel's random numbers: GLib's g_random is not meant for secrets */
  while (got < sizeof bytes)
    {
      ssize_t n = getrandom (bytes + got, sizeof bytes - got, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        g_error ("no random numbers: %s", g_strerror (errno));
      got += n;
    }
  for (guint i = 0; i < sizeof bytes; i++)
    g_string_append_printf (hex, "%02x", bytes[i]);
  return g_string_free (hex, FALSE);
}

const char *
dictate_device_id (void)
{
  G_LOCK (device);
  if (!id)
    {
      g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "vibeos-software", NULL);
      id = kept (dir, "device-id", valid_uuid, make_uuid);
    }
  G_UNLOCK (device);
  return id;
}

const char *
dictate_device_secret (void)
{
  G_LOCK (device);
  if (!secret)
    {
      g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", NULL);
      secret = kept (dir, "agent-secret", valid_secret, make_secret);
    }
  G_UNLOCK (device);
  return secret;
}

const char *
dictate_device_model (void)
{
  static const char *const files[] = { "/proc/device-tree/model", "/sys/class/dmi/id/product_name", NULL };

  G_LOCK (device);
  if (!model_done)
    {
      for (guint i = 0; !model && files[i]; i++)
        {
          g_autofree char *contents = NULL;
          gsize length;

          /* the device tree's string ends in a NUL */
          if (g_file_get_contents (files[i], &contents, &length, NULL) && length)
            {
              g_autofree char *text = g_strndup (contents, length);
              g_strstrip (text);
              if (*text && g_utf8_validate (text, -1, NULL))
                model = g_utf8_substring (text, 0, MIN (100, g_utf8_strlen (text, -1)));
            }
        }
      model_done = TRUE;
    }
  G_UNLOCK (device);
  return model;
}

void
dictate_device_set_release (const char *value)
{
  G_LOCK (device);
  g_free (release);
  release = g_strdup (value);
  G_UNLOCK (device);
}

const char *
dictate_device_release (void)
{
  return release;
}
