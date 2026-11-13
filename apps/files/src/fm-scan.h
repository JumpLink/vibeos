/* The Local Network scan: File Manager+ does not wait for announcements, it knocks on every address of
 * the networks the device is on. So does this, for the IPv4 networks of its interfaces (not the loopback, a USB
 * gadget link to a computer, a link-local address or a virtual bridge without hardware under it), at most the /24
 * around the address: TCP 445 on every address at once (a capped number in flight, each given up after a short
 * time), and 139 on the ones that refused 445. A host that answers is named by its NetBIOS node status (UDP 137),
 * else by its reverse DNS name (the host's part: "am6.fritz.box" is "am6"), else by its address written as
 * "192-168-10-1". The device's own address is tried too: a phone serving files is on the network as well.
 *
 * No GTK: the tests link it alone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* a host with a file server: its address ("192.168.10.1") and its name */
typedef void (*FmScanFound) (const char *address, const char *name, gpointer data);
/* every address tried and every host named */
typedef void (*FmScanDone) (gpointer data);

/* starts the scan; @found and @done run on the thread-default main context, never after @cancellable is
 * cancelled. With no network to scan, @done runs at once. */
void       fm_scan_start            (GCancellable *cancellable, FmScanFound found, FmScanDone done, gpointer data);

/* ---- the pieces, for the tests ---- */

/* the addresses of @address/@prefix to try, at most the /24 around it, without the network's and the broadcast
 * address (GInetAddress) */
GPtrArray *fm_scan_targets_for      (GInetAddress *address, guint prefix);
/* the NetBIOS node status request for "*" with transaction @id (50 bytes) */
GBytes    *fm_scan_nbstat_request   (guint16 id);
/* the machine's name from a node status response: its unique file server (<20>) or workstation (<00>) name, the
 * trailing blanks cut; NULL when the response is not one */
char      *fm_scan_nbstat_name      (const guint8 *data, gsize size);
/* "192.168.10.1" as "192-168-10-1" */
char      *fm_scan_address_name     (const char *address);

G_END_DECLS
