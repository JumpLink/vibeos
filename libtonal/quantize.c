/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * The colours of an image: Wu's quantizer (recursive cuts of the RGB cube by the variance of its pixels, from
 * Graphics Gems II) gives the starting clusters, and weighted k-means in L*a*b* refines them. See tonal.h.
 *
 * The reference does its sums in 32-bit integers that overflow on large images and its k-means compares clusters
 * by position in a reordered row; both are kept, so that an image gives the same colours here as there. */
#include "tonal-private.h"

#include <string.h>

/* ---- the distinct colours of the image, in the order they first appear, with their counts --------------------- */

typedef struct {
  GArray     *colors;     /* guint32, first-appearance order */
  GArray     *counts;     /* gint32 */
  GHashTable *index;      /* colour -> its position + 1 */
} ColorMap;

static void
color_map_init (ColorMap *map, const guint32 *pixels, gsize n_pixels)
{
  map->colors = g_array_new (FALSE, FALSE, sizeof (guint32));
  map->counts = g_array_new (FALSE, FALSE, sizeof (gint32));
  map->index = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (gsize i = 0; i < n_pixels; i++)
    {
      guint position = GPOINTER_TO_UINT (g_hash_table_lookup (map->index, GUINT_TO_POINTER (pixels[i])));

      if (position == 0)
        {
          gint32 one = 1;

          g_array_append_val (map->colors, pixels[i]);
          g_array_append_val (map->counts, one);
          g_hash_table_insert (map->index, GUINT_TO_POINTER (pixels[i]), GUINT_TO_POINTER (map->colors->len));
        }
      else
        g_array_index (map->counts, gint32, position - 1)++;
    }
}

static void
color_map_clear (ColorMap *map)
{
  g_array_unref (map->colors);
  g_array_unref (map->counts);
  g_hash_table_unref (map->index);
}

/* ---- Wu ------------------------------------------------------------------------------------------------------- */

#define INDEX_BITS 5
#define INDEX_COUNT 33
#define TOTAL_SIZE (INDEX_COUNT * INDEX_COUNT * INDEX_COUNT)

typedef struct {
  int r0, r1, g0, g1, b0, b1, vol;
} Box;

typedef enum { RED, GREEN, BLUE } Direction;

typedef struct {
  guint32 *weights, *moments_r, *moments_g, *moments_b;
  double  *moments;
  Box     *cubes;
} Wu;

static int
wu_index (int r, int g, int b)
{
  return (r << (INDEX_BITS * 2)) + (r << (INDEX_BITS + 1)) + r + (g << INDEX_BITS) + g + b;
}

static gint32
volume (const Box *c, const guint32 *m)
{
  return (gint32) (m[wu_index (c->r1, c->g1, c->b1)] - m[wu_index (c->r1, c->g1, c->b0)]
                   - m[wu_index (c->r1, c->g0, c->b1)] + m[wu_index (c->r1, c->g0, c->b0)]
                   - m[wu_index (c->r0, c->g1, c->b1)] + m[wu_index (c->r0, c->g1, c->b0)]
                   + m[wu_index (c->r0, c->g0, c->b1)] - m[wu_index (c->r0, c->g0, c->b0)]);
}

static gint32
bottom (const Box *c, Direction direction, const guint32 *m)
{
  switch (direction)
    {
    case RED:
      return (gint32) (- m[wu_index (c->r0, c->g1, c->b1)] + m[wu_index (c->r0, c->g1, c->b0)]
                       + m[wu_index (c->r0, c->g0, c->b1)] - m[wu_index (c->r0, c->g0, c->b0)]);
    case GREEN:
      return (gint32) (- m[wu_index (c->r1, c->g0, c->b1)] + m[wu_index (c->r1, c->g0, c->b0)]
                       + m[wu_index (c->r0, c->g0, c->b1)] - m[wu_index (c->r0, c->g0, c->b0)]);
    default:
      return (gint32) (- m[wu_index (c->r1, c->g1, c->b0)] + m[wu_index (c->r1, c->g0, c->b0)]
                       + m[wu_index (c->r0, c->g1, c->b0)] - m[wu_index (c->r0, c->g0, c->b0)]);
    }
}

static gint32
top (const Box *c, Direction direction, int position, const guint32 *m)
{
  switch (direction)
    {
    case RED:
      return (gint32) (m[wu_index (position, c->g1, c->b1)] - m[wu_index (position, c->g1, c->b0)]
                       - m[wu_index (position, c->g0, c->b1)] + m[wu_index (position, c->g0, c->b0)]);
    case GREEN:
      return (gint32) (m[wu_index (c->r1, position, c->b1)] - m[wu_index (c->r1, position, c->b0)]
                       - m[wu_index (c->r0, position, c->b1)] + m[wu_index (c->r0, position, c->b0)]);
    default:
      return (gint32) (m[wu_index (c->r1, c->g1, position)] - m[wu_index (c->r1, c->g0, position)]
                       - m[wu_index (c->r0, c->g1, position)] + m[wu_index (c->r0, c->g0, position)]);
    }
}

/* r * r + g * g + b * b in 32-bit integers */
static double
squares (gint32 r, gint32 g, gint32 b)
{
  return (double) tonal_iadd (tonal_iadd (tonal_imul (r, r), tonal_imul (g, g)), tonal_imul (b, b));
}

static void
wu_histogram (Wu *wu, const ColorMap *map)
{
  for (guint i = 0; i < map->colors->len; i++)
    {
      guint32 pixel = g_array_index (map->colors, guint32, i);
      gint32 count = g_array_index (map->counts, gint32, i);
      int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
      int index = wu_index ((red >> (8 - INDEX_BITS)) + 1, (green >> (8 - INDEX_BITS)) + 1,
                            (blue >> (8 - INDEX_BITS)) + 1);

      wu->weights[index] += (guint32) count;
      wu->moments_r[index] += (guint32) tonal_imul (red, count);
      wu->moments_g[index] += (guint32) tonal_imul (green, count);
      wu->moments_b[index] += (guint32) tonal_imul (blue, count);
      wu->moments[index] += tonal_imul (count, red * red + green * green + blue * blue);
    }
}

static void
wu_moments (Wu *wu)
{
  for (int r = 1; r < INDEX_COUNT; ++r)
    {
      guint32 area[INDEX_COUNT] = { 0 }, area_r[INDEX_COUNT] = { 0 }, area_g[INDEX_COUNT] = { 0 };
      guint32 area_b[INDEX_COUNT] = { 0 };
      double area2[INDEX_COUNT] = { 0 };

      for (int g = 1; g < INDEX_COUNT; ++g)
        {
          guint32 line = 0, line_r = 0, line_g = 0, line_b = 0;
          double line2 = 0.0;

          for (int b = 1; b < INDEX_COUNT; ++b)
            {
              int index = wu_index (r, g, b), previous = wu_index (r - 1, g, b);

              line += wu->weights[index];
              line_r += wu->moments_r[index];
              line_g += wu->moments_g[index];
              line_b += wu->moments_b[index];
              line2 += wu->moments[index];
              area[b] += line;
              area_r[b] += line_r;
              area_g[b] += line_g;
              area_b[b] += line_b;
              area2[b] += line2;
              wu->weights[index] = wu->weights[previous] + area[b];
              wu->moments_r[index] = wu->moments_r[previous] + area_r[b];
              wu->moments_g[index] = wu->moments_g[previous] + area_g[b];
              wu->moments_b[index] = wu->moments_b[previous] + area_b[b];
              wu->moments[index] = wu->moments[previous] + area2[b];
            }
        }
    }
}

static double
variance (const Wu *wu, const Box *c)
{
  gint32 dr = volume (c, wu->moments_r), dg = volume (c, wu->moments_g), db = volume (c, wu->moments_b);
  const double *m = wu->moments;
  double xx = m[wu_index (c->r1, c->g1, c->b1)] - m[wu_index (c->r1, c->g1, c->b0)]
              - m[wu_index (c->r1, c->g0, c->b1)] + m[wu_index (c->r1, c->g0, c->b0)]
              - m[wu_index (c->r0, c->g1, c->b1)] + m[wu_index (c->r0, c->g1, c->b0)]
              + m[wu_index (c->r0, c->g0, c->b1)] - m[wu_index (c->r0, c->g0, c->b0)];
  gint32 hypotenuse = (gint32) squares (dr, dg, db);

  return xx - hypotenuse / ((double) volume (c, wu->weights));
}

static int
maximize (const Wu *wu, const Box *c, Direction direction, int first, int last, gint32 whole_r, gint32 whole_g,
          gint32 whole_b, gint32 whole_w, double *maximum)
{
  gint32 bottom_r = bottom (c, direction, wu->moments_r), bottom_g = bottom (c, direction, wu->moments_g);
  gint32 bottom_b = bottom (c, direction, wu->moments_b), bottom_w = bottom (c, direction, wu->weights);
  double max = 0.0;
  int cut = -1;

  for (int i = first; i < last; i++)
    {
      gint32 half_r = tonal_iadd (bottom_r, top (c, direction, i, wu->moments_r));
      gint32 half_g = tonal_iadd (bottom_g, top (c, direction, i, wu->moments_g));
      gint32 half_b = tonal_iadd (bottom_b, top (c, direction, i, wu->moments_b));
      gint32 half_w = tonal_iadd (bottom_w, top (c, direction, i, wu->weights));
      double temp;

      if (half_w == 0)
        continue;
      temp = squares (half_r, half_g, half_b) / (double) half_w;
      half_r = tonal_isub (whole_r, half_r);
      half_g = tonal_isub (whole_g, half_g);
      half_b = tonal_isub (whole_b, half_b);
      half_w = tonal_isub (whole_w, half_w);
      if (half_w == 0)
        continue;
      temp += squares (half_r, half_g, half_b) / (double) half_w;
      if (temp > max)
        {
          max = temp;
          cut = i;
        }
    }
  *maximum = max;
  return cut;
}

static gboolean
cut (const Wu *wu, Box *one, Box *two)
{
  gint32 whole_r = volume (one, wu->moments_r), whole_g = volume (one, wu->moments_g);
  gint32 whole_b = volume (one, wu->moments_b), whole_w = volume (one, wu->weights);
  double max_r, max_g, max_b;
  int cut_r = maximize (wu, one, RED, one->r0 + 1, one->r1, whole_r, whole_g, whole_b, whole_w, &max_r);
  int cut_g = maximize (wu, one, GREEN, one->g0 + 1, one->g1, whole_r, whole_g, whole_b, whole_w, &max_g);
  int cut_b = maximize (wu, one, BLUE, one->b0 + 1, one->b1, whole_r, whole_g, whole_b, whole_w, &max_b);
  Direction direction;

  if (max_r >= max_g && max_r >= max_b)
    {
      if (cut_r < 0)
        return FALSE;
      direction = RED;
    }
  else if (max_g >= max_r && max_g >= max_b)
    direction = GREEN;
  else
    direction = BLUE;

  two->r1 = one->r1;
  two->g1 = one->g1;
  two->b1 = one->b1;
  switch (direction)
    {
    case RED:
      one->r1 = cut_r;
      two->r0 = one->r1;
      two->g0 = one->g0;
      two->b0 = one->b0;
      break;
    case GREEN:
      one->g1 = cut_g;
      two->r0 = one->r0;
      two->g0 = one->g1;
      two->b0 = one->b0;
      break;
    case BLUE:
      one->b1 = cut_b;
      two->r0 = one->r0;
      two->g0 = one->g0;
      two->b0 = one->b1;
      break;
    }
  one->vol = (one->r1 - one->r0) * (one->g1 - one->g0) * (one->b1 - one->b0);
  two->vol = (two->r1 - two->r0) * (two->g1 - two->g0) * (two->b1 - two->b0);
  return TRUE;
}

/* the starting clusters: distinct colours, in the order the cuts made them */
static GArray *
quantize_wu (const ColorMap *map, int max_colors)
{
  Wu wu = {
    .weights = g_new0 (guint32, TOTAL_SIZE),
    .moments_r = g_new0 (guint32, TOTAL_SIZE),
    .moments_g = g_new0 (guint32, TOTAL_SIZE),
    .moments_b = g_new0 (guint32, TOTAL_SIZE),
    .moments = g_new0 (double, TOTAL_SIZE),
    .cubes = g_new0 (Box, max_colors),
  };
  double *volume_variance = g_new0 (double, max_colors);
  GArray *colors = g_array_new (FALSE, FALSE, sizeof (guint32));
  int generated = max_colors, next = 0;

  wu_histogram (&wu, map);
  wu_moments (&wu);
  wu.cubes[0].r1 = wu.cubes[0].g1 = wu.cubes[0].b1 = INDEX_COUNT - 1;
  for (int i = 1; i < max_colors; i++)
    {
      double temp;

      if (cut (&wu, &wu.cubes[next], &wu.cubes[i]))
        {
          volume_variance[next] = wu.cubes[next].vol > 1 ? variance (&wu, &wu.cubes[next]) : 0.0;
          volume_variance[i] = wu.cubes[i].vol > 1 ? variance (&wu, &wu.cubes[i]) : 0.0;
        }
      else
        {
          volume_variance[next] = 0.0;
          i--;
        }
      next = 0;
      temp = volume_variance[0];
      for (int j = 1; j <= i; j++)
        if (volume_variance[j] > temp)
          {
            temp = volume_variance[j];
            next = j;
          }
      if (temp <= 0.0)
        {
          generated = i + 1;
          break;
        }
    }

  for (int i = 0; i < generated; i++)
    {
      const Box *cube = &wu.cubes[i];
      gint32 weight = volume (cube, wu.weights);
      gboolean seen = FALSE;
      guint32 color;
      gint32 r, g, b;

      if (weight <= 0)
        continue;
      r = volume (cube, wu.moments_r) / weight;
      g = volume (cube, wu.moments_g) / weight;
      b = volume (cube, wu.moments_b) / weight;
      color = 0xff000000u | ((guint32) (r & 0xff) << 16) | ((guint32) (g & 0xff) << 8) | (guint32) (b & 0xff);
      for (guint j = 0; j < colors->len && !seen; j++)
        seen = g_array_index (colors, guint32, j) == color;
      if (!seen)
        g_array_append_val (colors, color);
    }

  g_free (volume_variance);
  g_free (wu.weights);
  g_free (wu.moments_r);
  g_free (wu.moments_g);
  g_free (wu.moments_b);
  g_free (wu.moments);
  g_free (wu.cubes);
  return colors;
}

/* ---- weighted k-means --------------------------------------------------------------------------------------------- */

/* the reference's pseudo-random numbers: a 48-bit linear congruential generator */
typedef struct {
  guint64 seed;
} Random;

static void
random_init (Random *random, guint64 seed)
{
  random->seed = (seed ^ G_GUINT64_CONSTANT (0x5DEECE66D)) & ((G_GUINT64_CONSTANT (1) << 48) - 1);
}

static gint32
random_next (Random *random, int bits)
{
  random->seed = (random->seed * G_GUINT64_CONSTANT (0x5DEECE66D) + 0xB) & ((G_GUINT64_CONSTANT (1) << 48) - 1);
  return (gint32) (guint32) (random->seed >> (48 - bits));
}

static gint32
random_next_int (Random *random, gint32 bound)
{
  gint32 r = random_next (random, 31), m = bound - 1;

  if ((bound & m) == 0)
    return (gint32) (((gint64) bound * (gint64) r) >> 31);
  for (gint32 u = r; tonal_iadd (tonal_isub (u, r = u % bound), m) < 0; u = random_next (random, 31))
    ;
  return r;
}

typedef struct {
  int    index;
  double distance;
} Distance;

typedef double Lab[3];

static double
lab_distance (const double *a, const double *b)
{
  double dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];

  return dl * dl + da * da + db * db;
}

/* a stable sort by distance, nearest first */
static void
sort_distances (Distance *row, int n)
{
  for (int i = 1; i < n; i++)
    {
      Distance d = row[i];
      int j = i;

      while (j > 0 && row[j - 1].distance > d.distance)
        {
          row[j] = row[j - 1];
          j--;
        }
      row[j] = d;
    }
}

#define MAX_ITERATIONS 10
#define MIN_MOVEMENT_DISTANCE 3.0

static GArray *
quantize_wsmeans (const ColorMap *map, const GArray *starting, int max_colors)
{
  int point_count = map->colors->len, cluster_count = MIN (max_colors, point_count);
  Lab *points, *clusters;
  int *cluster_indices, *pixel_count_sums;
  Distance *matrix;
  Random random;
  GArray *result = g_array_new (FALSE, FALSE, sizeof (TonalColorCount));

  if (starting->len != 0)
    cluster_count = MIN (cluster_count, (int) starting->len);
  if (cluster_count <= 0 || starting->len == 0)
    return result;

  random_init (&random, 0x42688);
  points = g_new (Lab, point_count);
  for (int i = 0; i < point_count; i++)
    tonal_lab_from_argb (g_array_index (map->colors, guint32, i), points[i]);
  clusters = g_new0 (Lab, cluster_count);
  for (int i = 0; i < cluster_count; i++)
    tonal_lab_from_argb (g_array_index (starting, guint32, i), clusters[i]);
  cluster_indices = g_new (int, point_count);
  for (int i = 0; i < point_count; i++)
    cluster_indices[i] = random_next_int (&random, cluster_count);
  matrix = g_new (Distance, (gsize) cluster_count * cluster_count);
  for (int i = 0; i < cluster_count * cluster_count; i++)
    matrix[i] = (Distance) { -1, -1 };
  pixel_count_sums = g_new0 (int, cluster_count);

#define M(i, j) matrix[(gsize) (i) * cluster_count + (j)]
  for (int iteration = 0; iteration < MAX_ITERATIONS; iteration++)
    {
      int points_moved = 0;
      double *sum_a, *sum_b, *sum_c;

      for (int i = 0; i < cluster_count; i++)
        {
          for (int j = i + 1; j < cluster_count; j++)
            {
              double distance = lab_distance (clusters[i], clusters[j]);

              M (j, i).distance = distance;
              M (j, i).index = i;
              M (i, j).distance = distance;
              M (i, j).index = j;
            }
          sort_distances (&M (i, 0), cluster_count);
        }

      for (int i = 0; i < point_count; i++)
        {
          int previous_index = cluster_indices[i], new_index = -1;
          double previous_distance = lab_distance (points[i], clusters[previous_index]);
          double minimum_distance = previous_distance;

          for (int j = 0; j < cluster_count; j++)
            {
              double distance;

              if (M (previous_index, j).distance >= 4 * previous_distance)
                continue;
              distance = lab_distance (points[i], clusters[j]);
              if (distance < minimum_distance)
                {
                  minimum_distance = distance;
                  new_index = j;
                }
            }
          if (new_index != -1 && fabs (sqrt (minimum_distance) - sqrt (previous_distance)) > MIN_MOVEMENT_DISTANCE)
            {
              points_moved++;
              cluster_indices[i] = new_index;
            }
        }
      if (points_moved == 0 && iteration != 0)
        break;

      sum_a = g_new0 (double, cluster_count);
      sum_b = g_new0 (double, cluster_count);
      sum_c = g_new0 (double, cluster_count);
      memset (pixel_count_sums, 0, sizeof (int) * cluster_count);
      for (int i = 0; i < point_count; i++)
        {
          int cluster = cluster_indices[i];
          gint32 count = g_array_index (map->counts, gint32, i);

          pixel_count_sums[cluster] += count;
          sum_a[cluster] += points[i][0] * count;
          sum_b[cluster] += points[i][1] * count;
          sum_c[cluster] += points[i][2] * count;
        }
      for (int i = 0; i < cluster_count; i++)
        {
          int count = pixel_count_sums[i];

          if (count == 0)
            {
              clusters[i][0] = clusters[i][1] = clusters[i][2] = 0.;
              continue;
            }
          clusters[i][0] = sum_a[i] / count;
          clusters[i][1] = sum_b[i] / count;
          clusters[i][2] = sum_c[i] / count;
        }
      g_free (sum_a);
      g_free (sum_b);
      g_free (sum_c);
    }
#undef M

  for (int i = 0; i < cluster_count; i++)
    {
      TonalColorCount entry = { tonal_argb_from_lab (clusters[i]), pixel_count_sums[i] };
      gboolean seen = FALSE;

      if (pixel_count_sums[i] == 0)
        continue;
      for (guint j = 0; j < result->len && !seen; j++)
        seen = g_array_index (result, TonalColorCount, j).argb == entry.argb;
      if (!seen)
        g_array_append_val (result, entry);
    }

  g_free (points);
  g_free (clusters);
  g_free (cluster_indices);
  g_free (matrix);
  g_free (pixel_count_sums);
  return result;
}

GArray *
tonal_quantize (const guint32 *pixels, gsize n_pixels, guint max_colors)
{
  ColorMap map;
  GArray *starting, *result;

  g_return_val_if_fail (max_colors > 0, NULL);
  color_map_init (&map, pixels, n_pixels);
  starting = quantize_wu (&map, max_colors);
  result = quantize_wsmeans (&map, starting, max_colors);
  g_array_unref (starting);
  color_map_clear (&map);
  return result;
}
