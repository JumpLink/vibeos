/* Debian versions (inst-version.h): dpkg's comparison, after lib/dpkg/version.c (verrevcmp). */
#include "inst-version.h"

#include <stdlib.h>
#include <string.h>

/* a character's weight among the non-digits: the end and the tilde first, then letters, then everything else */
static int
order (int c)
{
  if (g_ascii_isdigit (c))
    return 0;
  if (g_ascii_isalpha (c))
    return c;
  if (c == '~')
    return -1;
  if (c)
    return c + 256;
  return 0;
}

/* one part (upstream version or revision): alternating runs of non-digits, compared by weight, and of digits,
 * compared as numbers */
static int
compare_part (const char *a, const char *b)
{
  while (*a || *b)
    {
      int first_diff = 0;

      while ((*a && !g_ascii_isdigit (*a)) || (*b && !g_ascii_isdigit (*b)))
        {
          int ac = order (*a), bc = order (*b);

          if (ac != bc)
            return ac - bc;
          a++;
          b++;
        }
      while (*a == '0')
        a++;
      while (*b == '0')
        b++;
      while (g_ascii_isdigit (*a) && g_ascii_isdigit (*b))
        {
          if (!first_diff)
            first_diff = *a - *b;
          a++;
          b++;
        }
      if (g_ascii_isdigit (*a))
        return 1;
      if (g_ascii_isdigit (*b))
        return -1;
      if (first_diff)
        return first_diff;
    }
  return 0;
}

typedef struct {
  unsigned long epoch;
  char *upstream, *revision;   /* in one buffer: free upstream only */
} Version;

/* [epoch:]upstream[-revision], as dpkg splits it: the epoch before the first colon, the revision after the last
 * hyphen */
static void
version_parse (const char *text, Version *v)
{
  const char *colon;
  char *hyphen;

  while (text && g_ascii_isspace (*text))
    text++;
  v->epoch = 0;
  colon = text ? strchr (text, ':') : NULL;
  if (colon)
    {
      v->epoch = strtoul (text, NULL, 10);
      text = colon + 1;
    }
  v->upstream = g_strstrip (g_strdup (text ? text : ""));
  hyphen = strrchr (v->upstream, '-');
  if (hyphen)
    {
      *hyphen = '\0';
      v->revision = hyphen + 1;
    }
  else
    v->revision = v->upstream + strlen (v->upstream);
}

int
inst_version_compare (const char *a, const char *b)
{
  Version va, vb;
  int result;

  version_parse (a, &va);
  version_parse (b, &vb);
  if (va.epoch != vb.epoch)
    result = va.epoch > vb.epoch ? 1 : -1;
  else
    {
      result = compare_part (va.upstream, vb.upstream);
      if (!result)
        result = compare_part (va.revision, vb.revision);
    }
  g_free (va.upstream);
  g_free (vb.upstream);
  return result < 0 ? -1 : result > 0 ? 1 : 0;
}
