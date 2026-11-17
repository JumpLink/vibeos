/* The demo's settings (inst-demo.h), read once from the environment */
#include "inst-demo.h"

#include <stdlib.h>

static GHashTable *
settings (void)
{
  static GHashTable *table;
  static gsize once;

  if (g_once_init_enter (&once))
    {
      const char *env = g_getenv ("VIBEOS_INSTALLER_DEMO");

      table = NULL;
      if (env)
        {
          g_auto (GStrv) items = g_strsplit (env, ",", -1);

          table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
          for (guint i = 0; items[i]; i++)
            {
              char *eq = strchr (items[i], '=');

              if (!*items[i])
                continue;
              if (eq)
                g_hash_table_replace (table, g_strndup (items[i], eq - items[i]), g_strdup (eq + 1));
              else
                g_hash_table_replace (table, g_strdup (items[i]), g_strdup ("yes"));
            }
        }
      g_once_init_leave (&once, 1);
    }
  return table;
}

gboolean
inst_demo_active (void)
{
  return settings () != NULL;
}

const char *
inst_demo_get (const char *key)
{
  GHashTable *table = settings ();

  return table ? g_hash_table_lookup (table, key) : NULL;
}

gboolean
inst_demo_has (const char *key)
{
  return inst_demo_get (key) != NULL;
}

const char *
inst_demo_get_for (const char *key, const char *package)
{
  const char *only = inst_demo_get ("package");

  if (only && package && g_strcmp0 (only, package))
    return NULL;
  return inst_demo_get (key);
}

guint64
inst_demo_number (const char *key, guint64 fallback)
{
  const char *value = inst_demo_get (key);

  return value ? g_ascii_strtoull (value, NULL, 10) : fallback;
}
