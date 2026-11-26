#include "ns-ip.h"

#include <gio/gio.h>

enum { RANK_NONE, RANK_LOCAL, RANK_GLOBAL };

static int
rank (const char *text)
{
  g_autoptr (GInetAddress) a = text ? g_inet_address_new_from_string (text) : NULL;
  if (!a || g_inet_address_get_family (a) != G_SOCKET_FAMILY_IPV6)
    return RANK_NONE;
  if (g_inet_address_get_is_link_local (a) || g_inet_address_get_is_loopback (a) || g_inet_address_get_is_any (a) ||
      g_inet_address_get_is_multicast (a))
    return RANK_NONE;
  const guint8 *b = g_inet_address_to_bytes (a);
  return (b[0] & 0xfe) == 0xfc ? RANK_LOCAL : RANK_GLOBAL;
}

const char *
ns_ip_pick_v6 (const char *const *addrs)
{
  const char *best = NULL;
  int best_rank = RANK_NONE;
  for (guint i = 0; addrs && addrs[i]; i++) {
    int r = rank (addrs[i]);
    if (r > best_rank) {
      best = addrs[i];
      best_rank = r;
    }
  }
  return best;
}
