/* Which of a device's IPv6 addresses Settings shows: a global one first, else a unique local one (fc00::/7), never
 * a link-local one, which every interface has and which says nothing about the network. GLib only, so the tests use
 * the same interface as the page. */
#pragma once

#include <glib.h>

const char *ns_ip_pick_v6 (const char *const *addrs);
