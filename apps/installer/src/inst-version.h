/* inst-version: Debian package versions, compared the way dpkg compares them.
 *
 * A version is [epoch:]upstream[-revision]: the epoch counts first, then the upstream version, then the revision,
 * each part compared as dpkg --compare-versions does (runs of digits as numbers, everything else character by
 * character, letters before other characters, and a tilde before anything, the end of the string included: 1.0~rc1
 * is older than 1.0). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* < 0 when a is older than b, 0 when they are the same version (1.0 and 1.00 are, as are 0:1 and 1), > 0 when a is
 * newer. NULL is the empty version. */
int   inst_version_compare (const char *a, const char *b);

G_END_DECLS
