/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * Which of an image's colours to theme with: each colour scores for how much of the image shares its hue and for
 * how close its chroma is to a primary palette's; the best ones that differ enough in hue win. See tonal.h. */
#include "tonal-private.h"

#define TARGET_CHROMA 48.0
#define WEIGHT_PROPORTION 0.7
#define WEIGHT_CHROMA_ABOVE 0.3
#define WEIGHT_CHROMA_BELOW 0.1
#define CUTOFF_CHROMA 5.0
#define CUTOFF_EXCITED_PROPORTION 0.01

typedef struct {
  guint32  argb;
  TonalHct hct;
  double   score;
} Scored;

GArray *
tonal_score (const TonalColorCount *colors, guint n_colors, guint desired, guint32 fallback, gboolean filter)
{
  TonalHct *hcts = g_new (TonalHct, MAX (n_colors, 1));
  Scored *scored = g_new (Scored, MAX (n_colors, 1));
  const Scored **chosen = g_new (const Scored *, MAX (desired, 1));
  double hue_population[360] = { 0 }, excited[360] = { 0 }, population_sum = 0.;
  guint n_scored = 0, n_chosen = 0;
  GArray *result = g_array_new (FALSE, FALSE, sizeof (guint32));

  for (guint i = 0; i < n_colors; i++)
    {
      int hue;

      hcts[i] = tonal_hct_from_argb (colors[i].argb);
      hue = MIN ((int) floor (hcts[i].hue), 359);
      hue_population[hue] += colors[i].count;
      population_sum += colors[i].count;
    }
  /* the share of the image within 15 degrees of each hue */
  for (int hue = 0; hue < 360; hue++)
    {
      double proportion = hue_population[hue] / population_sum;

      for (int i = hue - 14; i < hue + 16; i++)
        excited[tonal_sanitize_degrees_int (i)] += proportion;
    }

  for (guint i = 0; i < n_colors; i++)
    {
      const TonalHct *hct = &hcts[i];
      double proportion = excited[tonal_sanitize_degrees_int ((int) tonal_round (hct->hue))];
      double chroma_weight = hct->chroma < TARGET_CHROMA ? WEIGHT_CHROMA_BELOW : WEIGHT_CHROMA_ABOVE;
      Scored entry = { colors[i].argb, *hct, 0 };
      guint j;

      if (filter && (hct->chroma < CUTOFF_CHROMA || proportion <= CUTOFF_EXCITED_PROPORTION))
        continue;
      entry.score = proportion * 100.0 * WEIGHT_PROPORTION + (hct->chroma - TARGET_CHROMA) * chroma_weight;
      /* a stable insertion, highest score first */
      for (j = n_scored; j > 0 && scored[j - 1].score < entry.score; j--)
        scored[j] = scored[j - 1];
      scored[j] = entry;
      n_scored++;
    }

  /* as far apart in hue as there are colours to fill `desired` with */
  for (int difference = 90; difference >= 15; difference--)
    {
      n_chosen = 0;
      for (guint i = 0; i < n_scored; i++)
        {
          gboolean duplicate = FALSE;

          for (guint j = 0; j < n_chosen && !duplicate; j++)
            duplicate = tonal_difference_degrees (scored[i].hct.hue, chosen[j]->hct.hue) < difference;
          if (!duplicate)
            chosen[n_chosen++] = &scored[i];
          if (n_chosen >= desired)
            break;
        }
      if (n_chosen >= desired)
        break;
    }

  if (n_chosen == 0)
    g_array_append_val (result, fallback);
  for (guint i = 0; i < n_chosen; i++)
    g_array_append_val (result, chosen[i]->argb);

  g_free (hcts);
  g_free (scored);
  g_free (chosen);
  return result;
}

#define MAX_EXTRACTION_AREA (112 * 112)

GArray *
tonal_seeds_from_image (const guint32 *pixels, int width, int height, int stride)
{
  int new_width = width, new_height = height;
  g_autoptr (GArray) sample = g_array_new (FALSE, FALSE, sizeof (guint32));
  g_autoptr (GArray) colors = NULL;

  g_return_val_if_fail (width > 0 && height > 0, NULL);
  /* scaled down without filtering, to the area the colours are taken from */
  if ((gint64) width * height > MAX_EXTRACTION_AREA)
    {
      double scale = sqrt (MAX_EXTRACTION_AREA / ((double) width * height));

      new_width = MAX ((int) (width * scale), 1);
      new_height = MAX ((int) (height * scale), 1);
    }
  for (int y = 0; y < new_height; y++)
    {
      int source_y = MIN ((int) floor ((y + 0.5) * height / new_height), height - 1);

      for (int x = 0; x < new_width; x++)
        {
          int source_x = MIN ((int) floor ((x + 0.5) * width / new_width), width - 1);
          guint32 pixel = pixels[(gsize) source_y * stride + source_x];

          if ((pixel >> 24) == 0)
            continue;
          pixel |= 0xff000000u;
          g_array_append_val (sample, pixel);
        }
    }
  colors = tonal_quantize ((const guint32 *) sample->data, sample->len, 128);
  return tonal_score ((const TonalColorCount *) colors->data, colors->len, 4, 0xff4285f4, TRUE);
}
