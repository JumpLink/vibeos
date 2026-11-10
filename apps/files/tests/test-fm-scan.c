/* The Local Network scan's pieces: which addresses it knocks on and how it reads NetBIOS names. No packet leaves
 * the machine. */
#include "fm-scan.h"

#include <string.h>

static GPtrArray *
targets (const char *address, guint prefix)
{
  g_autoptr(GInetAddress) inet = g_inet_address_new_from_string (address);

  return fm_scan_targets_for (inet, prefix);
}

static char *
nth (GPtrArray *array, guint i)
{
  return g_inet_address_to_string (array->pdata[i]);
}

static void
test_targets (void)
{
  g_autoptr(GPtrArray) lan = targets ("192.168.10.58", 24);
  g_autoptr(GPtrArray) wide = targets ("10.1.2.3", 8);
  g_autoptr(GPtrArray) small = targets ("192.168.10.58", 30);
  g_autoptr(GPtrArray) point = targets ("192.168.10.58", 32);
  g_autofree char *first = nth (lan, 0), *last = nth (lan, lan->len - 1);
  g_autofree char *wide_first = nth (wide, 0), *wide_last = nth (wide, wide->len - 1);
  g_autofree char *small_first = nth (small, 0), *small_last = nth (small, 1);

  /* the /24 without its network and broadcast addresses, the device's own among them */
  g_assert_cmpuint (lan->len, ==, 254);
  g_assert_cmpstr (first, ==, "192.168.10.1");
  g_assert_cmpstr (last, ==, "192.168.10.254");
  /* a /8 only around the address */
  g_assert_cmpuint (wide->len, ==, 254);
  g_assert_cmpstr (wide_first, ==, "10.1.2.1");
  g_assert_cmpstr (wide_last, ==, "10.1.2.254");
  g_assert_cmpuint (small->len, ==, 2);
  g_assert_cmpstr (small_first, ==, "192.168.10.57");
  g_assert_cmpstr (small_last, ==, "192.168.10.58");
  g_assert_cmpuint (point->len, ==, 0);
}

static void
test_request (void)
{
  g_autoptr(GBytes) request = fm_scan_nbstat_request (0x1234);
  gsize size;
  const guint8 *data = g_bytes_get_data (request, &size);

  g_assert_cmpuint (size, ==, 50);
  g_assert_cmpuint (data[0], ==, 0x12);
  g_assert_cmpuint (data[1], ==, 0x34);
  g_assert_cmpuint (data[5], ==, 1);            /* one question */
  g_assert_cmpuint (data[12], ==, 32);
  g_assert_cmpmem (data + 13, 4, "CKAA", 4);   /* "*" and its NUL padding */
  g_assert_cmpuint (data[44], ==, 'A');
  g_assert_cmpuint (data[45], ==, 0);
  g_assert_cmpuint (data[47], ==, 0x21);        /* NBSTAT */
  g_assert_cmpuint (data[49], ==, 0x01);        /* IN */
}

/* a node status response with @n names: (name, suffix, group) */
static GByteArray *
response (guint n, const char *const *names, const guint8 *suffixes, const gboolean *groups)
{
  GByteArray *packet = g_byte_array_new ();
  const guint8 header[12] = { 0x12, 0x34, 0x84, 0x00, 0, 0, 0, 1, 0, 0, 0, 0 };
  guint8 name[34] = { 32 };
  guint rdlength = 1 + n * 18 + 46;
  guint8 fixed[10] = { 0, 0x21, 0, 1, 0, 0, 0, 0, rdlength >> 8, rdlength & 0xff };
  guint8 count = n, stats[46] = { 0 };

  memset (name + 1, 'A', 32);
  name[1] = 'C';
  name[2] = 'K';
  g_byte_array_append (packet, header, sizeof header);
  g_byte_array_append (packet, name, sizeof name);
  g_byte_array_append (packet, fixed, sizeof fixed);
  g_byte_array_append (packet, &count, 1);
  for (guint i = 0; i < n; i++)
    {
      guint8 entry[18];

      memset (entry, ' ', 15);
      memcpy (entry, names[i], strlen (names[i]));
      entry[15] = suffixes[i];
      entry[16] = groups[i] ? 0x84 : 0x04;
      entry[17] = 0;
      g_byte_array_append (packet, entry, sizeof entry);
    }
  g_byte_array_append (packet, stats, sizeof stats);
  return packet;
}

static void
test_names (void)
{
  const char *const server[] = { "WORKGROUP", "PLEX", "PLEX", "WORKGROUP" };
  const guint8 server_suffixes[] = { 0x00, 0x00, 0x20, 0x1e };
  const gboolean server_groups[] = { TRUE, FALSE, FALSE, TRUE };
  const char *const box[] = { "WORKGROUP", "UGOOS" };
  const guint8 box_suffixes[] = { 0x00, 0x00 };
  const gboolean box_groups[] = { TRUE, FALSE };
  g_autoptr(GByteArray) a = response (4, server, server_suffixes, server_groups);
  g_autoptr(GByteArray) b = response (2, box, box_suffixes, box_groups);
  g_autoptr(GByteArray) c = response (1, box, box_suffixes, box_groups);
  g_autofree char *plex = fm_scan_nbstat_name (a->data, a->len);
  g_autofree char *ugoos = fm_scan_nbstat_name (b->data, b->len);
  g_autofree char *group = fm_scan_nbstat_name (c->data, c->len);
  g_autofree char *cut = fm_scan_nbstat_name (a->data, 60);
  g_autofree char *dashed = fm_scan_address_name ("192.168.10.1");

  g_assert_cmpstr (plex, ==, "PLEX");
  g_assert_cmpstr (ugoos, ==, "UGOOS");
  g_assert_null (group);
  g_assert_null (cut);
  /* a request is no answer */
  a->data[2] = 0x00;
  {
    g_autofree char *request = fm_scan_nbstat_name (a->data, a->len);

    g_assert_null (request);
  }
  g_assert_cmpstr (dashed, ==, "192-168-10-1");
}

static void
on_found (const char *address, const char *name, gpointer data)
{
  g_assert_not_reached ();
}

static void
on_done (gpointer data)
{
  g_assert_not_reached ();
}

static gboolean
on_quit (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

/* a scan cancelled before it starts sends nothing and calls nothing */
static void
test_cancelled (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  g_cancellable_cancel (cancellable);
  fm_scan_start (cancellable, on_found, on_done, NULL);
  g_timeout_add (50, on_quit, loop);
  g_main_loop_run (loop);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/scan/targets", test_targets);
  g_test_add_func ("/scan/request", test_request);
  g_test_add_func ("/scan/names", test_names);
  g_test_add_func ("/scan/cancelled", test_cancelled);
  return g_test_run ();
}
