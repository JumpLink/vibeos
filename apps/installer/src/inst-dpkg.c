/* dpkg's database through dpkg-query (inst-dpkg.h). With the demo on, its settings for the package come first. */
#include "inst-dpkg.h"
#include "inst-demo.h"

#include <gio/gio.h>
#include <string.h>

static char *
run (GError **error, const char *const *argv)
{
  g_autoptr (GSubprocess) proc = NULL;
  char *out = NULL;

  proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
  if (!proc || !g_subprocess_communicate_utf8 (proc, NULL, NULL, &out, NULL, error))
    return NULL;
  /* a non-zero exit (no such package) still answers */
  return out ? out : g_strdup ("");
}

/* dpkg's status of a package with files on the system: installed, unpacked, half-configured, half-installed,
 * awaiting or pending triggers (the second letter of db:Status-Abbrev); not 'n' (not installed) or 'c' (only its
 * configuration files left) */
static gboolean
has_files (const char *abbrev)
{
  return strlen (abbrev) >= 2 && strchr ("iUFhWt", abbrev[1]) != NULL;
}

gboolean
inst_dpkg_query (const char *package, const char *architecture, InstDpkgInfo *info, GError **error)
{
  const char *argv[] = { "dpkg-query", "--show",
                         "--showformat=${db:Status-Abbrev}\t${Version}\t${Architecture}\t${Essential}\t"
                         "${Priority}\t${Installed-Size}\n",
                         "--", package, NULL };
  g_autofree char *out = NULL;
  g_auto (GStrv) lines = NULL;
  int chosen = -1;

  memset (info, 0, sizeof *info);
  if (!package || !*package)
    return TRUE;
  if (!g_strcmp0 (architecture, "all"))
    architecture = NULL;
  if (inst_demo_active () && inst_demo_get_for ("installed", package))
    {
      const char *version = inst_demo_get_for ("installed", package);

      if (g_strcmp0 (version, "no"))
        {
          info->version = g_strdup (version);
          info->architecture = g_strdup (architecture ? architecture : inst_dpkg_native_arch ());
          info->priority = g_strdup ("optional");
          info->essential = inst_demo_get_for ("essential", package) != NULL;
          info->installed_size = inst_demo_number ("installed-size", 9 * 1024 * 1024);
        }
      return TRUE;
    }
  out = run (error, argv);
  if (!out)
    return FALSE;
  lines = g_strsplit (out, "\n", -1);
  /* every architecture of the name, the one asked for first: an Architecture: all package that used to be arm64 (or
   * the other way round) is the same package to dpkg */
  for (int i = 0; lines[i]; i++)
    {
      g_auto (GStrv) fields = g_strsplit (lines[i], "\t", 6);

      if (g_strv_length (fields) < 6 || !has_files (fields[0]) || !*fields[1])
        continue;
      if (chosen < 0 || !g_strcmp0 (fields[2], architecture))
        chosen = i;
    }
  if (chosen >= 0)
    {
      g_auto (GStrv) fields = g_strsplit (lines[chosen], "\t", 6);

      info->version = g_strdup (fields[1]);
      info->architecture = g_strdup (fields[2]);
      info->essential = !g_ascii_strcasecmp (fields[3], "yes");
      info->priority = g_strdup (fields[4]);
      info->installed_size = g_ascii_strtoull (fields[5], NULL, 10) * 1024;
      if (inst_demo_active () && inst_demo_get_for ("essential", package))
        info->essential = TRUE;
    }
  return TRUE;
}

void
inst_dpkg_info_clear (InstDpkgInfo *info)
{
  g_clear_pointer (&info->version, g_free);
  g_clear_pointer (&info->architecture, g_free);
  g_clear_pointer (&info->priority, g_free);
}

GStrv
inst_dpkg_files (const char *package)
{
  const char *argv[] = { "dpkg-query", "--listfiles", "--", package, NULL };
  g_autofree char *out = run (NULL, argv);

  if (!out || !*out)
    return NULL;
  return g_strsplit (g_strchomp (out), "\n", -1);
}

const char *
inst_dpkg_native_arch (void)
{
  static char *arch;
  static gsize once;

  if (g_once_init_enter (&once))
    {
      const char *argv[] = { "dpkg", "--print-architecture", NULL };
      char *out = run (NULL, argv);

      arch = out && *g_strstrip (out) ? out : (g_free (out), g_strdup ("arm64"));
      g_once_init_leave (&once, 1);
    }
  return arch;
}

gboolean
inst_dpkg_arch_supported (const char *architecture)
{
  const char *argv[] = { "dpkg", "--print-foreign-architectures", NULL };
  g_autofree char *foreign = NULL;
  g_auto (GStrv) archs = NULL;

  if (!architecture || !*architecture || !g_strcmp0 (architecture, "all")
      || !g_strcmp0 (architecture, inst_dpkg_native_arch ()))
    return TRUE;
  foreign = run (NULL, argv);
  if (!foreign)
    return FALSE;
  archs = g_strsplit_set (g_strstrip (foreign), " \n", -1);
  return g_strv_contains ((const char *const *) archs, architecture);
}
