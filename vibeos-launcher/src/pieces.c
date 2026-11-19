/* The parts of a picture worth drawing (see pieces.h) */
#include "pieces.h"

#include <stdbool.h>

/* Covering more than this share of the picture, the pieces save too little: it is drawn whole */
#define WHOLE 0.8

GArray *
pieces_find (const guint8 *pixels, int w, int h, int stride, int tile)
{
  if (w <= 0 || h <= 0 || tile < 3)
    return NULL;
  int tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
  g_autofree guint8 *used = g_malloc0 ((gsize) tw * th);
  for (int y = 0; y < h; y++) {
    const guint8 *row = pixels + (gsize) y * stride;
    /* a texel that is not clear, and the ones beside it: a bilinear sample at any of them reads it */
    int ty0 = MAX (y - 1, 0) / tile, ty1 = MIN (y + 1, h - 1) / tile;
    for (int x = 0; x < w; x++) {
      if (!row[x * 4 + 3])
        continue;
      int tx0 = MAX (x - 1, 0) / tile, tx1 = MIN (x + 1, w - 1) / tile;
      used[ty0 * tw + tx0] = used[ty0 * tw + tx1] = used[ty1 * tw + tx0] = used[ty1 * tw + tx1] = 1;
    }
  }
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Piece));
  guint64 covered = 0;
  guint above = 0;          /* the first piece that may reach down to this row */
  for (int ty = 0; ty < th; ty++) {
    int y0 = ty * tile, y1 = MIN (y0 + tile, h);
    guint row_first = out->len;
    for (int tx = 0; tx < tw; tx++) {
      if (!used[ty * tw + tx])
        continue;
      int end = tx + 1;
      while (end < tw && used[ty * tw + end])
        end++;
      int x0 = tx * tile, x1 = MIN (end * tile, w);
      covered += (guint64) (x1 - x0) * (y1 - y0);
      bool grown = false;
      for (guint i = above; i < row_first && !grown; i++) {
        Piece *p = &g_array_index (out, Piece, i);
        if (p->y1 == y0 && p->x0 == x0 && p->x1 == x1) {
          p->y1 = y1;
          grown = true;
        }
      }
      if (!grown)
        g_array_append_val (out, ((Piece) { x0, y0, x1, y1 }));
      tx = end;
    }
    /* the pieces that end above this row are done with */
    while (above < out->len && g_array_index (out, Piece, above).y1 < y1)
      above++;
  }
  if (covered >= WHOLE * w * h) {
    g_array_unref (out);
    return NULL;
  }
  return out;
}
