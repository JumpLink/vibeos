/* sw-dpkg: see sw-dpkg.h */
#include "sw-dpkg.h"

#include <string.h>

const char *
sw_dpkg_status_path (void)
{
  const char *env = g_getenv ("VIBEOS_SOFTWARE_DPKG_STATUS");

  return env && *env ? env : "/var/lib/dpkg/status";
}

/* one stanza's fields of interest, each pointing into the text */
typedef struct {
  const char *package, *status, *version;
  gsize       package_len, status_len, version_len;
} Stanza;

static void
stanza_add (GHashTable *installed, const Stanza *s)
{
  if (!s->package || !s->version || !s->status)
    return;
  /* "Status: install ok installed": the last word is the state */
  if (s->status_len < strlen (" installed") ||
      strncmp (s->status + s->status_len - strlen (" installed"), " installed", strlen (" installed")))
    return;
  g_hash_table_replace (installed, g_strndup (s->package, s->package_len), g_strndup (s->version, s->version_len));
}

GHashTable *
sw_dpkg_parse_status (const char *text, gsize length)
{
  GHashTable *installed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  const char *p = text, *end = text + length;
  Stanza s = { 0 };

  while (p < end)
    {
      const char *eol = memchr (p, '\n', end - p);
      gsize len;

      if (!eol)
        eol = end;
      len = eol - p;
      if (len == 0)
        {
          stanza_add (installed, &s);
          memset (&s, 0, sizeof s);
        }
      else if (*p != ' ' && *p != '\t')
        {
          const char *colon = memchr (p, ':', len);

          if (colon)
            {
              const char *value = colon + 1;
              gsize value_len;

              while (value < eol && (*value == ' ' || *value == '\t'))
                value++;
              value_len = eol - value;
              while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\r'))
                value_len--;
#define FIELD(name, member) \
              if ((gsize) (colon - p) == strlen (name) && !g_ascii_strncasecmp (p, name, strlen (name))) \
                { s.member = value; s.member##_len = value_len; }
              FIELD ("Package", package)
              FIELD ("Status", status)
              FIELD ("Version", version)
#undef FIELD
            }
        }
      p = eol + 1;
    }
  stanza_add (installed, &s);
  return installed;
}

static void
read_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  const char *path = data;
  g_autofree char *contents = NULL;
  gsize length = 0;
  GError *error = NULL;

  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      g_task_return_error (task, error);
      return;
    }
  g_task_return_pointer (task, sw_dpkg_parse_status (contents, length), (GDestroyNotify) g_hash_table_unref);
}

void
sw_dpkg_read_async (const char *path, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);

  g_task_set_task_data (task, g_strdup (path), g_free);
  g_task_run_in_thread (task, read_thread);
}

GHashTable *
sw_dpkg_read_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static char *
desktop_in_list (const char *list_path)
{
  g_autofree char *contents = NULL;
  g_auto (GStrv) lines = NULL;

  if (!g_file_get_contents (list_path, &contents, NULL, NULL))
    return NULL;
  lines = g_strsplit (contents, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    if (g_str_has_prefix (lines[i], "/usr/share/applications/") && g_str_has_suffix (lines[i], ".desktop")
        && !strchr (lines[i] + strlen ("/usr/share/applications/"), '/'))
      return g_strdup (lines[i] + strlen ("/usr/share/applications/"));
  return NULL;
}

char *
sw_dpkg_desktop_id (const char *package)
{
  g_autofree char *plain = g_strdup_printf ("/var/lib/dpkg/info/%s.list", package);
  char *id = desktop_in_list (plain);
  g_autoptr (GDir) dir = NULL;
  const char *name;
  g_autofree char *prefix = g_strconcat (package, ":", NULL);

  if (id)
    return id;
  /* Multi-Arch: same packages keep their list as PACKAGE:ARCH.list */
  dir = g_dir_open ("/var/lib/dpkg/info", 0, NULL);
  while (dir && (name = g_dir_read_name (dir)))
    if (g_str_has_prefix (name, prefix) && g_str_has_suffix (name, ".list"))
      {
        g_autofree char *path = g_build_filename ("/var/lib/dpkg/info", name, NULL);

        if ((id = desktop_in_list (path)))
          return id;
      }
  return NULL;
}
