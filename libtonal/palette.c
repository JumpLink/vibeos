/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * Tonal palettes: one hue and chroma at every tone. See tonal.h. */
#include "tonal-private.h"

#include <stdlib.h>

const int tonal_stops[TONAL_N_STOPS] = { 0, 10, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000 };

static guint32
average_argb (guint32 a, guint32 b)
{
  guint32 red = (((a >> 16) & 255) + ((b >> 16) & 255) + 1) / 2;
  guint32 green = (((a >> 8) & 255) + ((b >> 8) & 255) + 1) / 2;
  guint32 blue = ((a & 255) + (b & 255) + 1) / 2;

  return 0xff000000u | red << 16 | green << 8 | blue;
}

/* Yellow at tone 99 is the mean of tones 98 and 100: at its own tone it comes out too green. */
guint32
tonal_palette_tone_of (double hue, double chroma, double tone)
{
  if (tone == 99.0 && tonal_hue_is_yellow (hue))
    return average_argb (tonal_hct_to_argb (hue, chroma, 98), tonal_hct_to_argb (hue, chroma, 100));
  return tonal_hct_to_argb (hue, chroma, tone);
}

guint32
tonal_palette_tone (const TonalPalette *palette, double tone)
{
  return tonal_palette_tone_of (palette->hue, palette->chroma, tone);
}

/* the most chroma the hue has at an integer tone */
static double
max_chroma (double hue, int tone, double *cache)
{
  if (cache[tone] < 0)
    cache[tone] = tonal_hct_from_argb (tonal_hct_to_argb (hue, 200.0, tone)).chroma;
  return cache[tone];
}

/* The key colour: the requested chroma at the tone closest to 50 that has that much chroma (or, for a chroma no tone
 * reaches, at the tone with the most). A binary search over integer tones. */
static TonalHct
key_color (double hue, double chroma)
{
  const int pivot = 50;
  const double epsilon = 0.01;
  double cache[102];
  int lower = 0, upper = 100;

  for (int i = 0; i < 102; i++)
    cache[i] = -1;
  while (lower < upper)
    {
      int mid = (lower + upper) / 2;
      gboolean ascending = max_chroma (hue, mid, cache) < max_chroma (hue, mid + 1, cache);
      gboolean sufficient = max_chroma (hue, mid, cache) >= chroma - epsilon;

      if (sufficient)
        {
          if (abs (lower - pivot) < abs (upper - pivot))
            upper = mid;
          else
            {
              if (lower == mid)
                break;
              lower = mid;
            }
        }
      else if (ascending)
        lower = mid + 1;
      else
        upper = mid;
    }
  return tonal_hct_from_argb (tonal_hct_to_argb (hue, chroma, lower));
}

void
tonal_palette_init (TonalPalette *palette, double hue, double chroma)
{
  palette->hue = hue;
  palette->chroma = chroma;
  palette->key = key_color (hue, chroma);
}

void
tonal_palette_init_hct (TonalPalette *palette, const TonalHct *hct)
{
  palette->hue = hct->hue;
  palette->chroma = hct->chroma;
  palette->key = *hct;
}

guint32
tonal_palette_stop (const TonalPalette *palette, int stop)
{
  double tone = stop == 0 ? 100 : stop == 10 ? 99 : stop == 50 ? 95 : 100 - stop / 10;

  return tonal_palette_tone (palette, tone);
}
