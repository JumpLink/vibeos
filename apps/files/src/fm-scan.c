/* The Local Network scan (fm-scan.h). */
#include "fm-scan.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>

#define PROBES_IN_FLIGHT 256    /* connections tried at once: a /24 in one go (a file descriptor each) */
#define PROBE_MS         2500   /* a silent address is given up after this: a host answers in milliseconds, but a
                                 * dozing one on Wi-Fi answers the ARP only after its next wake-up (0.5 s and more
                                 * measured), and the kernel asks three times a second apart */
#define NBSTAT_RESEND_MS 250    /* the node status request goes twice (UDP) … */
#define NBSTAT_MS        800    /* … and its answer is waited for this long */
#define RDNS_MS          1500   /* the reverse DNS lookup's limit */
#define PORT_SMB         445
#define PORT_NETBIOS     139
#define PORT_NBNS        137

/* ---- the pieces ---------------------------------------------------------------------------------------------------- */

GPtrArray *
fm_scan_targets_for (GInetAddress *address, guint prefix)
{
  GPtrArray *targets = g_ptr_array_new_with_free_func (g_object_unref);
  const guint8 *bytes;
  guint32 ip, mask, net, broadcast;

  if (g_inet_address_get_family (address) != G_SOCKET_FAMILY_IPV4 || prefix > 30)
    return targets;
  /* a wider network is scanned only around the address: 254 addresses, not 65534 */
  prefix = MAX (prefix, 24);
  bytes = g_inet_address_to_bytes (address);
  ip = (guint32) bytes[0] << 24 | (guint32) bytes[1] << 16 | (guint32) bytes[2] << 8 | bytes[3];
  mask = 0xffffffffu << (32 - prefix);
  net = ip & mask;
  broadcast = net | ~mask;
  for (guint32 a = net + 1; a < broadcast; a++)
    {
      guint8 out[4] = { a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff };

      g_ptr_array_add (targets, g_inet_address_new_from_bytes (out, G_SOCKET_FAMILY_IPV4));
    }
  return targets;
}

GBytes *
fm_scan_nbstat_request (guint16 id)
{
  guint8 packet[50] = { id >> 8, id & 0xff, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 32 };
  const guint8 name[16] = { '*' };

  /* the name "*" padded with NULs to 16 bytes, each byte as two letters (RFC 1002 4.1) */
  for (int i = 0; i < 16; i++)
    {
      packet[13 + 2 * i] = 'A' + (name[i] >> 4);
      packet[14 + 2 * i] = 'A' + (name[i] & 0x0f);
    }
  packet[45] = 0;
  packet[46] = 0x00;            /* NBSTAT */
  packet[47] = 0x21;
  packet[48] = 0x00;            /* IN */
  packet[49] = 0x01;
  return g_bytes_new (packet, sizeof packet);
}

/* past a name in a NetBIOS (DNS-like) message: its labels, or a pointer; 0 when it runs off the end */
static gsize
skip_name (const guint8 *data, gsize size, gsize at)
{
  while (at < size)
    {
      guint8 length = data[at];

      if ((length & 0xc0) == 0xc0)
        return at + 2 <= size ? at + 2 : 0;
      if (length == 0)
        return at + 1;
      at += 1 + length;
    }
  return 0;
}

char *
fm_scan_nbstat_name (const guint8 *data, gsize size)
{
  guint questions, answers, count;
  gsize at = 12, rdlength;
  const guint8 *entries, *found = NULL;

  if (size < 12 || !(data[2] & 0x80))
    return NULL;
  questions = data[4] << 8 | data[5];
  answers = data[6] << 8 | data[7];
  if (answers < 1)
    return NULL;
  for (guint i = 0; i < questions; i++)
    if (!(at = skip_name (data, size, at)) || (at += 4) > size)
      return NULL;
  if (!(at = skip_name (data, size, at)) || at + 10 > size)
    return NULL;
  if ((data[at] << 8 | data[at + 1]) != 0x21)
    return NULL;
  rdlength = data[at + 8] << 8 | data[at + 9];
  at += 10;
  if (rdlength < 1 || at + rdlength > size)
    return NULL;
  count = data[at];
  entries = data + at + 1;
  if (1 + count * 18 > rdlength)
    return NULL;
  /* a unique name (not a group's): the file server's <20>, else the workstation's <00> */
  for (guint suffix = 0; suffix < 2 && !found; suffix++)
    for (guint i = 0; i < count && !found; i++)
      {
        const guint8 *entry = entries + i * 18;

        if (entry[15] == (suffix == 0 ? 0x20 : 0x00) && !(entry[16] & 0x80))
          found = entry;
      }
  if (!found)
    return NULL;
  {
    char name[16];
    int length = 15;

    memcpy (name, found, 15);
    while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == '\0'))
      length--;
    name[length] = '\0';
    if (!length)
      return NULL;
    return g_utf8_validate (name, -1, NULL) ? g_strdup (name)
                                           : g_convert (name, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
  }
}

char *
fm_scan_address_name (const char *address)
{
  return g_strdelimit (g_strdup (address), ".:", '-');
}

/* ---- the networks ------------------------------------------------------------------------------------------------ */

static gboolean
sys_net_has (const char *interface, const char *what)
{
  g_autofree char *path = g_build_filename ("/sys/class/net", interface, what, NULL);

  return g_file_test (path, G_FILE_TEST_EXISTS);
}

/* whether a bridge (or a bond) has hardware under it: a desktop's br0 over its Ethernet does, a container's
 * bridge over veths does not */
static gboolean
has_hardware_member (const char *interface)
{
  g_autofree char *path = g_build_filename ("/sys/class/net", interface, "brif", NULL);
  g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
  const char *member;

  while (dir && (member = g_dir_read_name (dir)))
    if (sys_net_has (member, "device"))
      return TRUE;
  return FALSE;
}

/* a network of people's machines: hardware (or a bridge over it), not the USB gadget link to a computer */
static gboolean
interface_is_lan (const char *interface)
{
  g_autofree char *uevent_path = g_build_filename ("/sys/class/net", interface, "uevent", NULL);
  g_autofree char *uevent = NULL;

  if (g_str_has_prefix (interface, "usb") || g_str_has_prefix (interface, "rndis"))
    return FALSE;
  if (g_file_get_contents (uevent_path, &uevent, NULL, NULL) && strstr (uevent, "DEVTYPE=gadget"))
    return FALSE;
  if (sys_net_has (interface, "device"))
    return TRUE;
  return has_hardware_member (interface);
}

/* every address to try, once (two interfaces on one network give it once) */
static GPtrArray *
scan_targets (void)
{
  GPtrArray *targets = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  struct ifaddrs *list = NULL;

  if (getifaddrs (&list) < 0)
    return targets;
  for (struct ifaddrs *i = list; i; i = i->ifa_next)
    {
      g_autoptr(GInetAddress) address = NULL;
      g_autoptr(GPtrArray) more = NULL;
      guint32 mask;
      guint prefix;

      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask
          || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING)
          || (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) || !interface_is_lan (i->ifa_name))
        continue;
      address = g_inet_address_new_from_bytes ((const guint8 *) &((struct sockaddr_in *) i->ifa_addr)->sin_addr,
                                               G_SOCKET_FAMILY_IPV4);
      if (g_inet_address_get_is_link_local (address) || g_inet_address_get_is_loopback (address))
        continue;
      mask = ntohl (((struct sockaddr_in *) i->ifa_netmask)->sin_addr.s_addr);
      prefix = (guint) __builtin_popcount (mask);
      more = fm_scan_targets_for (address, prefix);
      for (guint j = 0; j < more->len; j++)
        {
          char *text = g_inet_address_to_string (more->pdata[j]);

          if (g_hash_table_add (seen, text))
            g_ptr_array_add (targets, g_object_ref (more->pdata[j]));
        }
    }
  freeifaddrs (list);
  return targets;
}

/* ---- the scan ------------------------------------------------------------------------------------------------------ *
 * Everything runs on the caller's main context. Each connection try and each lookup has a cancellable of its own,
 * cancelled by its time limit, and by the caller's (from an idle: a try can end inside g_cancellable_cancel, and the
 * scan's end disconnects from the caller's cancellable, which cannot be done inside its handler). The scan ends, and
 * is freed, when nothing is left in flight. */

typedef struct _Scan Scan;

typedef struct {
  Scan *scan;
  GInetAddress *address;
  char *text;                   /* the address as text */
  guint16 id;                   /* the node status request's */
  guint timer;
  GCancellable *cancellable;    /* the reverse lookup's */
} Host;

struct _Scan {
  GCancellable *cancellable;    /* the caller's */
  gulong cancelled;
  FmScanFound found;
  FmScanDone done;
  gpointer data;
  GSocketClient *client;
  GPtrArray *targets;           /* GInetAddress */
  GPtrArray *retries;           /* GInetAddress that refused 445: 139 is tried on them */
  guint next, next_retry;
  guint in_flight;              /* connection tries */
  GPtrArray *flight;            /* their cancellables */
  GHashTable *hosts;            /* address text -> Host, the ones answering */
  guint naming;                 /* hosts not named yet */
  GSocket *udp;
  GSource *udp_source;
  guint16 next_id;
  guint cancel_idle;
  int busy;                     /* inside a loop that the scan's end must not free it under */
  gboolean finished;
};

static void scan_pump (Scan *scan);

static gboolean
scan_cancelled (Scan *scan)
{
  return scan->cancellable && g_cancellable_is_cancelled (scan->cancellable);
}

static void
host_free (gpointer data)
{
  Host *host = data;

  g_clear_handle_id (&host->timer, g_source_remove);
  g_clear_object (&host->cancellable);
  g_object_unref (host->address);
  g_free (host->text);
  g_free (host);
}

static void
scan_free (Scan *scan)
{
  g_clear_handle_id (&scan->cancel_idle, g_source_remove);
  if (scan->udp_source)
    {
      g_source_destroy (scan->udp_source);
      g_source_unref (scan->udp_source);
    }
  g_clear_object (&scan->udp);
  g_clear_object (&scan->client);
  g_ptr_array_unref (scan->targets);
  g_ptr_array_unref (scan->retries);
  g_ptr_array_unref (scan->flight);
  g_hash_table_unref (scan->hosts);
  g_clear_object (&scan->cancellable);
  g_free (scan);
}

/* called whenever something ended: the scan ends when nothing is left to do or wait for */
static void
scan_check (Scan *scan)
{
  gboolean more = !scan_cancelled (scan)
                  && (scan->next < scan->targets->len || scan->next_retry < scan->retries->len);

  if (scan->finished || scan->busy || more || scan->in_flight > 0 || scan->naming > 0)
    return;
  scan->finished = TRUE;
  if (scan->cancellable)
    g_cancellable_disconnect (scan->cancellable, scan->cancelled);
  if (!scan_cancelled (scan) && scan->done)
    scan->done (scan->data);
  scan_free (scan);
}

static gboolean
on_cancel_idle (gpointer data)
{
  Scan *scan = data;
  g_autoptr(GPtrArray) cancellables = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  Host *host;

  scan->cancel_idle = 0;
  /* the tries and lookups in flight end with G_IO_ERROR_CANCELLED (some of them in here: the scan stays) */
  for (guint i = 0; i < scan->flight->len; i++)
    g_ptr_array_add (cancellables, g_object_ref (scan->flight->pdata[i]));
  g_hash_table_iter_init (&iter, scan->hosts);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &host))
    if (host->cancellable)
      g_ptr_array_add (cancellables, g_object_ref (host->cancellable));
  scan->busy++;
  for (guint i = 0; i < cancellables->len; i++)
    g_cancellable_cancel (cancellables->pdata[i]);
  scan->busy--;
  scan_check (scan);
  return G_SOURCE_REMOVE;
}

static void
on_scan_cancelled (GCancellable *cancellable, gpointer data)
{
  Scan *scan = data;

  if (!scan->cancel_idle && !scan->finished)
    scan->cancel_idle = g_idle_add (on_cancel_idle, scan);
}

/* ---- naming ---- */

static void
host_named (Host *host, const char *name)
{
  Scan *scan = host->scan;
  g_autofree char *fallback = NULL;

  g_clear_handle_id (&host->timer, g_source_remove);
  if (!scan_cancelled (scan) && scan->found)
    {
      if (!name || !*name)
        name = fallback = fm_scan_address_name (host->text);
      scan->found (host->text, name, scan->data);
    }
  host->id = 0;
  scan->naming--;
  scan_check (scan);
}

static gboolean
on_rdns_timeout (gpointer data)
{
  Host *host = data;

  host->timer = 0;
  g_cancellable_cancel (host->cancellable);
  return G_SOURCE_REMOVE;
}

static void
on_rdns (GObject *source, GAsyncResult *result, gpointer data)
{
  Host *host = data;
  g_autofree char *name = g_resolver_lookup_by_address_finish (G_RESOLVER (source), result, NULL);

  /* a name, not the address again; the host's own, without the network's domain ("am6.fritz.box" is "am6") */
  if (name && g_hostname_is_ip_address (name))
    g_clear_pointer (&name, g_free);
  if (name && strchr (name, '.'))
    *strchr (name, '.') = '\0';
  if (name && !*name)
    g_clear_pointer (&name, g_free);
  host_named (host, name);
}

static void
host_rdns (Host *host)
{
  Scan *scan = host->scan;

  host->id = 0;
  if (scan_cancelled (scan))
    {
      host_named (host, NULL);
      return;
    }
  host->cancellable = g_cancellable_new ();
  host->timer = g_timeout_add (RDNS_MS, on_rdns_timeout, host);
  g_resolver_lookup_by_address_async (g_resolver_get_default (), host->address, host->cancellable, on_rdns, host);
}

static gboolean
on_nbstat_timeout (gpointer data)
{
  Host *host = data;

  host->timer = 0;
  host_rdns (host);
  return G_SOURCE_REMOVE;
}

static void
nbstat_send (Host *host)
{
  Scan *scan = host->scan;
  g_autoptr(GBytes) request = fm_scan_nbstat_request (host->id);
  g_autoptr(GSocketAddress) to = g_inet_socket_address_new (host->address, PORT_NBNS);
  gsize size;
  const guint8 *bytes = g_bytes_get_data (request, &size);

  if (scan->udp)
    g_socket_send_to (scan->udp, to, (const char *) bytes, size, NULL, NULL);
}

static gboolean
on_nbstat_resend (gpointer data)
{
  Host *host = data;

  nbstat_send (host);
  host->timer = g_timeout_add (NBSTAT_MS - NBSTAT_RESEND_MS, on_nbstat_timeout, host);
  return G_SOURCE_REMOVE;
}

static gboolean
on_udp (GSocket *socket, GIOCondition condition, gpointer data)
{
  Scan *scan = data;
  guint8 buffer[1500];
  gssize size;
  GSocketAddress *from = NULL;

  scan->busy++;
  while ((size = g_socket_receive_from (socket, &from, (char *) buffer, sizeof buffer, NULL, NULL)) >= 0)
    {
      g_autofree char *text = NULL;
      Host *host;

      if (from && G_IS_INET_SOCKET_ADDRESS (from))
        text = g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (from)));
      g_clear_object (&from);
      host = text ? g_hash_table_lookup (scan->hosts, text) : NULL;
      /* the answer to the request still waiting, from the host it went to */
      if (host && host->id && size >= 2 && (buffer[0] << 8 | buffer[1]) == host->id)
        {
          g_autofree char *name = fm_scan_nbstat_name (buffer, (gsize) size);

          if (name)
            host_named (host, name);
        }
    }
  scan->busy--;
  /* the scan may end here, this source with it */
  scan_check (scan);
  return G_SOURCE_CONTINUE;
}

static void
udp_open (Scan *scan)
{
  g_autoptr(GInetAddress) any = g_inet_address_new_any (G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) local = g_inet_socket_address_new (any, 0);

  scan->udp = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  if (!scan->udp || !g_socket_bind (scan->udp, local, FALSE, NULL))
    {
      g_clear_object (&scan->udp);
      return;
    }
  g_socket_set_blocking (scan->udp, FALSE);
  scan->udp_source = g_socket_create_source (scan->udp, G_IO_IN, NULL);
  g_source_set_callback (scan->udp_source, G_SOURCE_FUNC (on_udp), scan, NULL);
  g_source_attach (scan->udp_source, g_main_context_get_thread_default ());
}

/* a host answered: its name, by NetBIOS, by DNS or by its address */
static void
host_add (Scan *scan, GInetAddress *address)
{
  g_autofree char *text = g_inet_address_to_string (address);
  Host *host;

  if (g_hash_table_contains (scan->hosts, text))
    return;
  host = g_new0 (Host, 1);
  host->scan = scan;
  host->address = g_object_ref (address);
  host->text = g_strdup (text);
  g_hash_table_insert (scan->hosts, g_steal_pointer (&text), host);
  scan->naming++;
  if (!scan->udp)
    udp_open (scan);
  if (!scan->udp)
    {
      host_rdns (host);
      return;
    }
  host->id = ++scan->next_id ? scan->next_id : ++scan->next_id;
  nbstat_send (host);
  host->timer = g_timeout_add (NBSTAT_RESEND_MS, on_nbstat_resend, host);
}

/* ---- the connection tries ---- */

typedef struct {
  Scan *scan;
  GInetAddress *address;
  guint16 port;
  GCancellable *cancellable;
  guint timer;
} Probe;

static gboolean
on_probe_timeout (gpointer data)
{
  Probe *probe = data;

  probe->timer = 0;
  g_cancellable_cancel (probe->cancellable);
  return G_SOURCE_REMOVE;
}

static void
on_probe_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Probe *probe = data;
  Scan *scan = probe->scan;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSocketConnection) connection = g_socket_client_connect_finish (G_SOCKET_CLIENT (source), result,
                                                                            &error);

  g_clear_handle_id (&probe->timer, g_source_remove);
  g_ptr_array_remove_fast (scan->flight, probe->cancellable);
  scan->in_flight--;
  if (!scan_cancelled (scan))
    {
      if (connection)
        host_add (scan, probe->address);
      /* a refusal is a host without SMB on 445: an old one may have it on 139 */
      else if (probe->port == PORT_SMB && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED))
        g_ptr_array_add (scan->retries, g_object_ref (probe->address));
    }
  if (connection)
    g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
  g_object_unref (probe->cancellable);
  g_object_unref (probe->address);
  g_free (probe);
  scan_pump (scan);
  scan_check (scan);
}

static void
probe_start (Scan *scan, GInetAddress *address, guint16 port)
{
  Probe *probe = g_new0 (Probe, 1);
  g_autoptr(GSocketAddress) to = g_inet_socket_address_new (address, port);

  probe->scan = scan;
  probe->address = g_object_ref (address);
  probe->port = port;
  probe->cancellable = g_cancellable_new ();
  g_ptr_array_add (scan->flight, probe->cancellable);
  scan->in_flight++;
  probe->timer = g_timeout_add (PROBE_MS, on_probe_timeout, probe);
  g_socket_client_connect_async (scan->client, G_SOCKET_CONNECTABLE (to), probe->cancellable, on_probe_done, probe);
}

static void
scan_pump (Scan *scan)
{
  while (!scan_cancelled (scan) && scan->in_flight < PROBES_IN_FLIGHT)
    {
      if (scan->next < scan->targets->len)
        probe_start (scan, scan->targets->pdata[scan->next++], PORT_SMB);
      else if (scan->next_retry < scan->retries->len)
        {
          GInetAddress *address = scan->retries->pdata[scan->next_retry++];
          g_autofree char *text = g_inet_address_to_string (address);

          if (!g_hash_table_contains (scan->hosts, text))
            probe_start (scan, address, PORT_NETBIOS);
        }
      else
        break;
    }
}

void
fm_scan_start (GCancellable *cancellable, FmScanFound found, FmScanDone done, gpointer data)
{
  Scan *scan = g_new0 (Scan, 1);

  scan->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  scan->found = found;
  scan->done = done;
  scan->data = data;
  scan->targets = scan_targets ();
  scan->retries = g_ptr_array_new_with_free_func (g_object_unref);
  scan->flight = g_ptr_array_new ();
  scan->hosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, host_free);
  scan->client = g_socket_client_new ();
  /* straight to the address: no proxy, and a timeout the tries' own timers come before */
  g_socket_client_set_enable_proxy (scan->client, FALSE);
  g_socket_client_set_timeout (scan->client, PROBE_MS / 1000 + 2);
  if (cancellable)
    scan->cancelled = g_cancellable_connect (cancellable, G_CALLBACK (on_scan_cancelled), scan, NULL);
  scan_pump (scan);
  scan_check (scan);
}
