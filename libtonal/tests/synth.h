/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * Synthetic images for the tests: blobs of colour with uniform noise, from a fixed pseudo-random sequence. */
#pragma once

#include <glib.h>

typedef struct { double share; int r, g, b; } Blob;

static guint32
synth_next (guint32 *state)
{
  *state = *state * 1103515245u + 12345u;
  return *state >> 8;
}

static guint32 *
synth_image (guint32 seed, int n, const Blob *blobs, int n_blobs, int noise)
{
  guint32 *pixels = g_new (guint32, n), state = seed;

  for (int i = 0; i < n; i++)
    {
      double x = (synth_next (&state) % 10000) / 10000.0, acc = 0;
      int k = 0, c[3];

      for (k = 0; k < n_blobs - 1; k++)
        if (x < (acc += blobs[k].share))
          break;
      c[0] = blobs[k].r, c[1] = blobs[k].g, c[2] = blobs[k].b;
      for (int j = 0; j < 3; j++)
        {
          int v = c[j] + (noise ? (int) (synth_next (&state) % (2 * noise + 1)) - noise : 0);

          c[j] = CLAMP (v, 0, 255);
        }
      pixels[i] = 0xff000000u | (guint32) c[0] << 16 | (guint32) c[1] << 8 | (guint32) c[2];
    }
  return pixels;
}

static const Blob blue_orange[] = { { 0.7, 40, 90, 200 }, { 0.3, 230, 120, 30 } };
static const Blob grey_only[] = { { 1.0, 128, 128, 128 } };
static const Blob three[] = { { 0.5, 20, 140, 60 }, { 0.3, 250, 220, 40 }, { 0.2, 90, 20, 160 } };
static const Blob mostly_grey[] = { { 0.95, 120, 120, 125 }, { 0.05, 200, 30, 60 } };
