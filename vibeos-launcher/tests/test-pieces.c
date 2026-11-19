/* The pieces of a parallax layer worth drawing (pieces.c): every texel a bilinear sample can read a colour from is
 * inside one, they never overlap, they sit on the tile grid, each tile of them is needed, a mostly covered picture is
 * drawn whole and a clear one not at all. */
#include "pieces.h"

#include <math.h>
#include <stdbool.h>

#define TILE 16

typedef struct {
  int     w, h, stride;
  guint8 *px;
} Pic;

static Pic
pic_new (int w, int h)
{
  Pic p = { w, h, w * 4 + 8, NULL };      /* a row longer than its pixels, as gdk-pixbuf's may be */
  p.px = g_malloc0 ((gsize) p.stride * h);
  return p;
}

static void
dot (Pic *p, int x, int y, guint8 a)
{
  p->px[(gsize) y * p->stride + x * 4 + 3] = a;
}

static bool
alpha (const Pic *p, int x, int y)
{
  return x >= 0 && y >= 0 && x < p->w && y < p->h && p->px[(gsize) y * p->stride + x * 4 + 3];
}

static int
piece_at (GArray *pieces, int x, int y)
{
  int n = 0;
  for (guint i = 0; i < pieces->len; i++) {
    Piece *q = &g_array_index (pieces, Piece, i);
    n += x >= q->x0 && x < q->x1 && y >= q->y0 && y < q->y1;
  }
  return n;
}

/* a tile some texel of which, or of the ring of texels around it, is not clear */
static bool
tile_needed (const Pic *p, int tx, int ty)
{
  for (int y = ty * TILE - 1; y <= (ty + 1) * TILE; y++)
    for (int x = tx * TILE - 1; x <= (tx + 1) * TILE; x++)
      if (alpha (p, x, y))
        return true;
  return false;
}

static void
check (const Pic *p, GArray *pieces)
{
  g_assert_nonnull (pieces);
  for (int y = 0; y < p->h; y++)
    for (int x = 0; x < p->w; x++) {
      int n = piece_at (pieces, x, y);
      g_assert_cmpint (n, <=, 1);                                 /* no texel drawn twice */
      bool near = false;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
          near |= alpha (p, x + dx, y + dy);
      if (near)
        g_assert_cmpint (n, ==, 1);                               /* what a sample here reads is drawn */
    }
  for (guint i = 0; i < pieces->len; i++) {
    Piece *q = &g_array_index (pieces, Piece, i);
    g_assert_cmpint (q->x0 % TILE, ==, 0);
    g_assert_cmpint (q->y0 % TILE, ==, 0);
    g_assert_true (q->x1 == p->w || q->x1 % TILE == 0);
    g_assert_true (q->y1 == p->h || q->y1 % TILE == 0);
    g_assert_cmpint (q->x0, <, q->x1);
    g_assert_cmpint (q->y0, <, q->y1);
    for (int ty = q->y0 / TILE; ty * TILE < q->y1; ty++)
      for (int tx = q->x0 / TILE; tx * TILE < q->x1; tx++)
        g_assert_true (tile_needed (p, tx, ty));
  }
}

static void
test_clear (void)
{
  Pic p = pic_new (100, 70);
  g_autoptr (GArray) pieces = pieces_find (p.px, p.w, p.h, p.stride, TILE);
  g_assert_nonnull (pieces);
  g_assert_cmpuint (pieces->len, ==, 0);
  g_free (p.px);
}

static void
test_opaque (void)
{
  Pic p = pic_new (100, 70);
  for (int y = 0; y < p.h; y++)
    for (int x = 0; x < p.w; x++)
      dot (&p, x, y, 255);
  g_assert_null (pieces_find (p.px, p.w, p.h, p.stride, TILE));
  g_free (p.px);
}

/* texels on the tiles' edges and corners: the tile beside them is needed too, the one past that is not */
static void
test_edges (void)
{
  Pic p = pic_new (130, 90);       /* neither a whole number of tiles */
  dot (&p, 15, 15, 1);             /* the last texel of tile (0, 0): (1, 0), (0, 1), (1, 1) are read with it */
  dot (&p, 64, 40, 200);           /* the first of tile (4, 2) */
  dot (&p, 129, 89, 255);          /* the picture's last texel, in a partial tile */
  dot (&p, 70, 0, 9);
  g_autoptr (GArray) pieces = pieces_find (p.px, p.w, p.h, p.stride, TILE);
  check (&p, pieces);
  g_assert_cmpint (piece_at (pieces, 16, 16), ==, 1);
  g_assert_cmpint (piece_at (pieces, 32, 16), ==, 0);
  g_assert_cmpint (piece_at (pieces, 63, 40), ==, 1);
  g_free (p.px);
}

/* a band across the picture: one piece per span of rows, grown down rather than one per row of tiles */
static void
test_band (void)
{
  Pic p = pic_new (200, 200);
  for (int y = 40; y < 120; y++)
    for (int x = 33; x < 90; x++)
      dot (&p, x, y, 128);
  g_autoptr (GArray) pieces = pieces_find (p.px, p.w, p.h, p.stride, TILE);
  check (&p, pieces);
  g_assert_cmpuint (pieces->len, ==, 1);
  Piece *q = &g_array_index (pieces, Piece, 0);
  g_assert_cmpint (q->x0, ==, 32);
  g_assert_cmpint (q->x1, ==, 96);
  g_assert_cmpint (q->y0, ==, 32);
  g_assert_cmpint (q->y1, ==, 128);
  g_free (p.px);
}

/* stars and a ring: scattered texels, all of them covered, none twice */
static void
test_scattered (void)
{
  Pic p = pic_new (517, 311);
  g_autoptr (GRand) r = g_rand_new_with_seed (7);
  for (int i = 0; i < 120; i++)
    dot (&p, g_rand_int_range (r, 0, p.w), g_rand_int_range (r, 0, p.h), g_rand_int_range (r, 1, 256));
  for (int i = 0; i < 2000; i++) {
    double t = i * G_PI * 2 / 2000;
    dot (&p, 250 + (int) (200 * cos (t)), 150 + (int) (120 * sin (t)), 255);
  }
  g_autoptr (GArray) pieces = pieces_find (p.px, p.w, p.h, p.stride, TILE);
  check (&p, pieces);
  g_free (p.px);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/pieces/clear", test_clear);
  g_test_add_func ("/pieces/opaque", test_opaque);
  g_test_add_func ("/pieces/edges", test_edges);
  g_test_add_func ("/pieces/band", test_band);
  g_test_add_func ("/pieces/scattered", test_scattered);
  return g_test_run ();
}
