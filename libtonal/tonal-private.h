/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * libtonal's internals, shared between its files. The arithmetic follows the reference implementation step by
 * step, including its rounding (ties towards +infinity) and its 32-bit integer overflow, so that every colour
 * comes out the same to the last bit. */
#pragma once

#include "tonal.h"

#include <math.h>

/* rounding to the nearest integer, ties towards +infinity (Java's Math.round), exactly */
static inline double
tonal_round (double x)
{
  double r = floor (x);

  return x - r >= 0.5 ? r + 1 : r;
}

static inline double
tonal_clamp (double min, double max, double x)
{
  return x < min ? min : x > max ? max : x;
}

static inline double
tonal_sanitize_degrees (double degrees)
{
  degrees = fmod (degrees, 360.0);
  return degrees < 0 ? degrees + 360.0 : degrees;
}

static inline int
tonal_sanitize_degrees_int (int degrees)
{
  degrees = degrees % 360;
  return degrees < 0 ? degrees + 360 : degrees;
}

static inline double
tonal_difference_degrees (double a, double b)
{
  return 180.0 - fabs (fabs (a - b) - 180.0);
}

static inline double
tonal_lerp (double start, double stop, double amount)
{
  return (1.0 - amount) * start + amount * stop;
}

/* 32-bit integer arithmetic that wraps around on overflow, as the reference's does */
static inline gint32
tonal_imul (gint32 a, gint32 b)
{
  return (gint32) ((guint32) a * (guint32) b);
}

static inline gint32
tonal_iadd (gint32 a, gint32 b)
{
  return (gint32) ((guint32) a + (guint32) b);
}

static inline gint32
tonal_isub (gint32 a, gint32 b)
{
  return (gint32) ((guint32) a - (guint32) b);
}

/* hue families the 2025 rules treat apart */
static inline gboolean tonal_hue_is_blue (double hue) { return hue >= 250 && hue < 270; }
static inline gboolean tonal_hue_is_yellow (double hue) { return hue >= 105 && hue < 125; }
static inline gboolean tonal_hue_is_cyan (double hue) { return hue >= 170 && hue < 207; }

/* ---- hct.c: colour spaces */

double   tonal_linearized       (int component);
int      tonal_delinearized     (double linear);
double   tonal_y_from_lstar     (double lstar);
double   tonal_lstar_from_y     (double y);
double   tonal_lstar_from_argb  (guint32 argb);
guint32  tonal_argb_from_lstar  (double lstar);
void     tonal_lab_from_argb    (guint32 argb, double lab[3]);
guint32  tonal_argb_from_lab    (const double lab[3]);

/* ---- hct.c: contrast between tones */

double   tonal_contrast_lighter        (double tone, double ratio);
double   tonal_contrast_darker         (double tone, double ratio);
double   tonal_contrast_lighter_unsafe (double tone, double ratio);
double   tonal_contrast_darker_unsafe  (double tone, double ratio);

/* ---- palette.c */

/* a palette's colour at `tone` without the palette (no key colour needed) */
guint32  tonal_palette_tone_of (double hue, double chroma, double tone);

/* ---- scheme.c */

/* a hue mapped through a table: `hues[i]` for a source hue in [breakpoints[i], breakpoints[i + 1]) */
double   tonal_piecewise_hue (double source_hue, const double *breakpoints, const double *hues, int n_hues);
double   tonal_rotated_hue   (double source_hue, const double *breakpoints, const double *rotations, int n_rotations);
