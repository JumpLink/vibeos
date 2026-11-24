/* The parts of a picture worth drawing: the tiles of it that hold a texel that is not fully transparent, or lie next
 * to one (a bilinear sample there reads it), joined into rectangles. A parallax layer is mostly empty sky: drawing
 * only these leaves the same pixels and blends a fraction of them. */
#pragma once

#include <glib.h>

typedef struct {
  int x0, y0, x1, y1;       /* texels; x1 and y1 one past the last */
} Piece;

/* pixels: rows of 4 channels, alpha the last, as gdk-pixbuf has them; tile: the tiles' size in texels (3 or more).
 * Pieces of a row of tiles span runs of them and never touch each other; a piece below one with the same span grows
 * it. NULL: they would cover most of the picture, draw it whole; an empty array: there is nothing to draw. */
GArray *pieces_find (const guint8 *pixels, int w, int h, int stride, int tile);
