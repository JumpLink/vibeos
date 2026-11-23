/* s10e-logo -- put Debian's pictures into the Samsung bootloader's
   up_param partition on the Galaxy S10e (SM-G970F), from inside LineageOS
   recovery: the screen it shows at power-on (logo.jpg: "SAMSUNG Galaxy
   S10e", "Powered by android") and the two it shows for an unlocked
   bootloader (svb_orange.jpg, booting_warning.jpg: the warning text).

   Usage:
     s10e-logo check PART DIR    say what apply would do, write nothing
     s10e-logo apply PART DIR

   up_param is a tar archive (GNU format) of JPEGs that the bootloader reads
   at every power-on; the last kilobyte of the 8 MiB partition, behind the
   archive, holds a few bytes that are not part of it.  apply replaces
   each of those three members that DIR has a file for, and keeps the other
   members, their order, and every header byte apart from the size and the
   checksum of a replaced one.  The space the shorter archive leaves is
   zeroed up to the old archive's end; nothing behind that is touched.
   Every header is read and checked (its checksum, a plain file, the data
   inside the partition) and the new archive is built in full before a
   byte is written; the written bytes are read back from the device.

   Output the installer reads: "same" (DIR's pictures are there already,
   nothing written), "would N" (check) or "written N": N pictures replaced.

   Exit codes: 0 ok; 1 refused (nothing written); 2 a write failed or did
   not read back the same.

   Build: see the Makefile next to this file.  No libraries beyond libc.  */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 512
#define MAX_PART (64u << 20)      /* up_param is 8 MiB; anything far bigger is the wrong partition */
#define MAX_PICTURE (4u << 20)
#define MAX_MEMBERS 256

static const char *const pictures[] = { "logo.jpg", "svb_orange.jpg", "booting_warning.jpg" };
#define N_PICTURES (sizeof pictures / sizeof pictures[0])

struct member
{
  size_t header;              /* offset of its header in the partition */
  size_t size;                /* data bytes as the partition has them */
  const uint8_t *data;        /* new data, or the partition's own */
  size_t new_size;
};

static void __attribute__ ((noreturn, format (printf, 2, 3)))
die (int code, const char *fmt, ...)
{
  va_list ap;

  fputs ("error: ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  exit (code);
}

static size_t
round_block (size_t n)
{
  return (n + BLOCK - 1) / BLOCK * BLOCK;
}

/* An octal header field: digits after optional spaces, ending at the field's
   end or at a NUL or space.  -1 when it is not one.  */
static long long
octal (const uint8_t *p, size_t len)
{
  size_t i = 0;
  long long v = 0;
  int digits = 0;

  while (i < len && p[i] == ' ')
    i++;
  for (; i < len && p[i] >= '0' && p[i] <= '7'; i++, digits++)
    {
      if (v > (1LL << 40))
        return -1;
      v = v * 8 + (p[i] - '0');
    }
  if (!digits || (i < len && p[i] != '\0' && p[i] != ' '))
    return -1;
  return v;
}

/* The header checksum: the sum of its bytes, the checksum field counted as
   eight spaces.  */
static unsigned
header_sum (const uint8_t *h)
{
  unsigned s = 0;

  for (int i = 0; i < BLOCK; i++)
    s += (i >= 148 && i < 156) ? ' ' : h[i];
  return s;
}

static int
all_zero (const uint8_t *p, size_t n)
{
  while (n--)
    if (*p++)
      return 0;
  return 1;
}

/* Write V into the field as the archive spells it: as many octal digits as
   the field had before its terminator, zero-padded, the terminator kept.  */
static int
put_octal (uint8_t *p, size_t len, unsigned long long v)
{
  size_t digits = 0;

  while (digits < len && p[digits] >= '0' && p[digits] <= '7')
    digits++;
  if (!digits)
    return -1;
  for (size_t i = digits; i-- > 0; v >>= 3)
    p[i] = '0' + (v & 7);
  return v ? -1 : 0;
}

static uint8_t *
read_file (const char *path, size_t *size)
{
  int fd = open (path, O_RDONLY);
  struct stat st;
  uint8_t *buf;
  size_t got = 0;

  if (fd < 0)
    return NULL;
  if (fstat (fd, &st) || !S_ISREG (st.st_mode) || st.st_size <= 0 || (uint64_t) st.st_size > MAX_PICTURE)
    die (1, "%s: not a picture this tool takes (a file of 1 byte to %u MiB)", path, MAX_PICTURE >> 20);
  buf = malloc (st.st_size);
  if (!buf)
    die (1, "out of memory");
  while (got < (size_t) st.st_size)
    {
      ssize_t n = read (fd, buf + got, st.st_size - got);

      if (n <= 0)
        die (1, "%s: %s", path, n ? strerror (errno) : "short read");
      got += n;
    }
  close (fd);
  *size = got;
  return buf;
}

static void
read_all (int fd, uint8_t *buf, size_t n, const char *what)
{
  size_t got = 0;

  while (got < n)
    {
      ssize_t r = pread (fd, buf + got, n - got, got);

      if (r <= 0)
        die (what ? 1 : 2, "%s: %s", what ? what : "reading back", r ? strerror (errno) : "short read");
      got += r;
    }
}

int
main (int argc, char **argv)
{
  int apply;
  const char *part, *dir;
  int fd;
  off_t end;
  size_t total, pos, old_end, new_end, span;
  uint8_t *buf, *out, *back;
  struct member m[MAX_MEMBERS];
  size_t n = 0;
  int replaced = 0, found = 0;

  if (argc != 4 || (strcmp (argv[1], "check") && strcmp (argv[1], "apply")))
    {
      fputs ("usage: s10e-logo check|apply PART DIR\n", stderr);
      return 1;
    }
  apply = !strcmp (argv[1], "apply");
  part = argv[2];
  dir = argv[3];
  /* a DIR without the pictures would read as "same": it is an error */
  for (size_t i = 0; i < N_PICTURES; i++)
    {
      char path[4096];
      struct stat st;

      snprintf (path, sizeof path, "%s/%s", dir, pictures[i]);
      if (!stat (path, &st))
        break;
      if (i == N_PICTURES - 1)
        die (1, "%s has none of %s, %s, %s", dir, pictures[0], pictures[1], pictures[2]);
    }

  fd = open (part, apply ? O_RDWR : O_RDONLY);
  if (fd < 0)
    die (1, "%s: %s", part, strerror (errno));
  end = lseek (fd, 0, SEEK_END);
  if (end < 4 * BLOCK || (uint64_t) end > MAX_PART)
    die (1, "%s: %lld bytes, not an up_param partition", part, (long long) end);
  total = end;
  buf = malloc (total);
  out = malloc (total);
  back = malloc (total);
  if (!buf || !out || !back)
    die (1, "out of memory");
  read_all (fd, buf, total, part);

  /* the archive as it is: every header checked, up to the end block */
  for (pos = 0;; )
    {
      const uint8_t *h = buf + pos;
      long long size, sum;

      if (pos + BLOCK > total)
        die (1, "%s: no end to the archive", part);
      if (all_zero (h, BLOCK))
        break;
      sum = octal (h + 148, 8);
      if (sum < 0 || (unsigned) sum != header_sum (h))
        die (1, "%s: the header at %zu does not check: not a tar archive (up_param)", part, pos);
      if (h[156] != '0' && h[156] != '\0')
        die (1, "%s: the member at %zu is not a plain file (type '%c')", part, pos, h[156]);
      if (!memchr (h, '\0', 100))
        die (1, "%s: the member at %zu has a name this tool does not read", part, pos);
      size = octal (h + 124, 12);
      if (size < 0 || (uint64_t) size > total || pos + BLOCK + round_block (size) > total)
        die (1, "%s: the member %s at %zu does not fit in the partition", part, (const char *) h, pos);
      if (n == MAX_MEMBERS)
        die (1, "%s: more than %d members", part, MAX_MEMBERS);
      m[n].header = pos;
      m[n].size = size;
      m[n].data = h + BLOCK;
      m[n].new_size = size;
      for (size_t i = 0; i < N_PICTURES; i++)
        if (!strcmp ((const char *) h, pictures[i]))
          {
            char path[4096];
            size_t len;
            uint8_t *data;

            found++;
            snprintf (path, sizeof path, "%s/%s", dir, pictures[i]);
            data = read_file (path, &len);
            if (!data)
              {
                if (errno != ENOENT)
                  die (1, "%s: %s", path, strerror (errno));
                break;
              }
            if (len == m[n].size && !memcmp (data, m[n].data, len))
              break;
            m[n].data = data;
            m[n].new_size = len;
            replaced++;
          }
      n++;
      pos += BLOCK + round_block (size);
    }
  old_end = pos;
  if (!found)
    die (1, "%s: no %s in it: not the bootloader's up_param", part, pictures[0]);
  if (!replaced)
    {
      puts ("same");
      return 0;
    }

  /* the new archive, over a copy of the partition */
  memcpy (out, buf, total);
  new_end = 0;
  for (size_t i = 0; i < n; i++)
    {
      uint8_t *h = out + new_end;
      size_t padded = round_block (m[i].new_size);
      char sum[16];

      if (new_end + BLOCK + padded + 2 * BLOCK > total)
        die (1, "%s: the new archive does not fit in the partition", part);
      memmove (h, buf + m[i].header, BLOCK);
      if (m[i].new_size != m[i].size || m[i].data != buf + m[i].header + BLOCK)
        {
          if (put_octal (h + 124, 12, m[i].new_size))
            die (1, "%s: %s: its size does not fit the header", part, (const char *) h);
          snprintf (sum, sizeof sum, "%06o", header_sum (h));
          if (strlen (sum) != 6)
            die (1, "%s: %s: its checksum does not fit the header", part, (const char *) h);
          memcpy (h + 148, sum, 6);
        }
      memmove (h + BLOCK, m[i].data, m[i].new_size);
      memset (h + BLOCK + m[i].new_size, 0, padded - m[i].new_size);
      new_end += BLOCK + padded;
    }
  /* the end blocks; and zeros where the old archive was, up to its own end blocks */
  span = old_end + 2 * BLOCK > new_end + 2 * BLOCK ? old_end + 2 * BLOCK : new_end + 2 * BLOCK;
  if (span > total)
    span = total;
  memset (out + new_end, 0, span - new_end);

  if (!apply)
    {
      printf ("would %d\n", replaced);
      return 0;
    }

  for (pos = 0; pos < span; )
    {
      ssize_t w = pwrite (fd, out + pos, span - pos, pos);

      if (w <= 0)
        die (2, "%s: writing at %zu: %s", part, pos, w ? strerror (errno) : "short write");
      pos += w;
    }
  if (fsync (fd))
    die (2, "%s: fsync: %s", part, strerror (errno));
  /* read back from the device, not from the page cache */
  ioctl (fd, BLKFLSBUF, 0);
  read_all (fd, back, total, NULL);
  if (memcmp (back, out, total))
    die (2, "%s: it does not read back as written", part);
  close (fd);
  printf ("written %d\n", replaced);
  return 0;
}
