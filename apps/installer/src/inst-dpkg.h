/* inst-dpkg: what dpkg's database says about installed packages, through dpkg-query (always there on Debian, and
 * readable without any privilege). Blocking but quick: a few milliseconds a call. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char    *version;          /* NULL: not installed (only its configuration left counts as not installed) */
  char    *architecture;
  char    *priority;         /* "required", "important", "optional", ... */
  gboolean essential;
  guint64  installed_size;   /* bytes */
} InstDpkgInfo;

/* The installed package `package`. `architecture` picks one of a multi-arch package's architectures (NULL or
 * "all": the first one installed). FALSE, with `error` set, only when dpkg-query cannot be run; a package that is not
 * installed is an answer (info->version NULL). */
gboolean  inst_dpkg_query        (const char *package, const char *architecture, InstDpkgInfo *info, GError **error);
void      inst_dpkg_info_clear   (InstDpkgInfo *info);

/* the installed package's files, or NULL */
GStrv     inst_dpkg_files        (const char *package);

/* the system's architecture (arm64) and the foreign ones dpkg accepts */
const char *inst_dpkg_native_arch (void);
gboolean  inst_dpkg_arch_supported (const char *architecture);

G_END_DECLS
