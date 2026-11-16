/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * Schemes: the palettes of each style, and the colour roles under both rule sets. See tonal.h.
 *
 * A role is defined by its palette, a starting tone and, for most roles, a background it must stand out from by a
 * contrast ratio that grows with the contrast level. Some come in pairs whose tones must stay a set distance apart
 * (primary and primary_container). The tone a role ends up with is its starting tone moved just far enough to meet
 * those constraints; the colour is its palette at that tone, with the chroma scaled for some surfaces. */
#include "tonal-private.h"

#include <string.h>

/* ---- hue tables ------------------------------------------------------------------------------------------------- */

double
tonal_piecewise_hue (double source_hue, const double *breakpoints, const double *hues, int n_hues)
{
  for (int i = 0; i < n_hues; i++)
    if (source_hue >= breakpoints[i] && source_hue < breakpoints[i + 1])
      return tonal_sanitize_degrees (hues[i]);
  return source_hue;
}

double
tonal_rotated_hue (double source_hue, const double *breakpoints, const double *rotations, int n_rotations)
{
  double rotation = tonal_piecewise_hue (source_hue, breakpoints, rotations, n_rotations);

  if (n_rotations <= 0)
    rotation = 0;
  return tonal_sanitize_degrees (source_hue + rotation);
}

/* the table's breakpoints and values: one more breakpoint than values, or as many (the last value then unused) */
#define ROTATED(hue, bp, rot) \
  tonal_rotated_hue ((hue), (bp), (rot), MIN ((int) G_N_ELEMENTS (bp) - 1, (int) G_N_ELEMENTS (rot)))
#define PIECEWISE(hue, bp, val) \
  tonal_piecewise_hue ((hue), (bp), (val), MIN ((int) G_N_ELEMENTS (bp) - 1, (int) G_N_ELEMENTS (val)))

/* ---- colours people dislike, and colour temperature (for the Content and Fidelity styles) --------------------- */

/* dark yellow-greens: they read as bile or mould */
static gboolean
is_disliked (const TonalHct *hct)
{
  double hue = tonal_round (hct->hue);

  return hue >= 90.0 && hue <= 111.0 && tonal_round (hct->chroma) > 16.0 && tonal_round (hct->tone) < 65.0;
}

static TonalHct
fix_if_disliked (TonalHct hct)
{
  if (is_disliked (&hct))
    return tonal_hct_from_argb (tonal_hct_to_argb (hct.hue, hct.chroma, 70.0));
  return hct;
}

/* The source colour's hue circle at its chroma and tone, each colour with its warmth (from its L*a*b* chroma and
 * hue); the source itself is the last entry. */
typedef struct {
  TonalHct hcts[362];
  double   temps[362];
  int      coldest, warmest;
} Temperatures;

static double
raw_temperature (guint32 argb)
{
  double lab[3], hue, chroma;

  tonal_lab_from_argb (argb, lab);
  hue = tonal_sanitize_degrees (atan2 (lab[2], lab[1]) * (180.0 / G_PI));
  chroma = hypot (lab[1], lab[2]);
  return -0.5 + 0.02 * pow (chroma, 1.07) * cos (tonal_sanitize_degrees (hue - 50.) * (G_PI / 180.0));
}

static void
temperatures_init (Temperatures *t, guint32 source, const TonalHct *input)
{
  int order[362];

  for (int hue = 0; hue <= 360; hue++)
    {
      guint32 argb = tonal_hct_to_argb (hue, input->chroma, input->tone);

      t->hcts[hue] = tonal_hct_from_argb (argb);
      t->temps[hue] = raw_temperature (argb);
    }
  t->hcts[361] = *input;
  t->temps[361] = raw_temperature (source);
  /* a stable sort by warmth, coldest first */
  for (int i = 0; i < 362; i++)
    {
      int j = i;

      while (j > 0 && t->temps[order[j - 1]] > t->temps[i])
        {
          order[j] = order[j - 1];
          j--;
        }
      order[j] = i;
    }
  t->coldest = order[0];
  t->warmest = order[361];
}

static double
relative_temperature (const Temperatures *t, int i)
{
  double range = t->temps[t->warmest] - t->temps[t->coldest];

  if (range == 0.)
    return 0.5;
  return (t->temps[i] - t->temps[t->coldest]) / range;
}

static gboolean
is_between (double angle, double a, double b)
{
  if (a < b)
    return a <= angle && angle <= b;
  return a <= angle || angle <= b;
}

/* the colour across the warmth scale from the source */
static TonalHct
complement (const Temperatures *t)
{
  const TonalHct *input = &t->hcts[361];
  double coldest_hue = t->hcts[t->coldest].hue, coldest_temp = t->temps[t->coldest];
  double warmest_hue = t->hcts[t->warmest].hue, warmest_temp = t->temps[t->warmest];
  double range = warmest_temp - coldest_temp;
  gboolean cold_to_warm = is_between (input->hue, coldest_hue, warmest_hue);
  double start_hue = cold_to_warm ? warmest_hue : coldest_hue, end_hue = cold_to_warm ? coldest_hue : warmest_hue;
  double smallest_error = 1000.;
  int answer = (int) tonal_round (input->hue);
  double complement_temp = 1. - relative_temperature (t, 361);

  for (double addend = 0.; addend <= 360.; addend += 1.)
    {
      double hue = tonal_sanitize_degrees (start_hue + addend);
      int i;
      double error;

      if (!is_between (hue, start_hue, end_hue))
        continue;
      i = (int) tonal_round (hue);
      error = fabs (complement_temp - (t->temps[i] - coldest_temp) / range);
      if (error < smallest_error)
        {
          smallest_error = error;
          answer = i;
        }
    }
  return t->hcts[answer];
}

/* `count` colours around the source, evenly spread in warmth over `divisions` steps of the hue circle; the source
 * in the middle */
static void
analogous (const Temperatures *t, int count, int divisions, TonalHct *out)
{
  int start_hue = (int) tonal_round (t->hcts[361].hue);
  int all[360], n_all = 0, addend = 1, ccw, cw, n_out = 0;
  double last_temp = relative_temperature (t, start_hue), total = 0., step, sum = 0.;

  all[n_all++] = start_hue;
  for (int i = 0; i < 360; i++)
    {
      double temp = relative_temperature (t, tonal_sanitize_degrees_int (start_hue + i));

      total += fabs (temp - last_temp);
      last_temp = temp;
    }
  step = total / (double) divisions;
  last_temp = relative_temperature (t, start_hue);
  while (n_all < divisions)
    {
      int hue = tonal_sanitize_degrees_int (start_hue + addend);
      double temp = relative_temperature (t, hue);
      double desired;
      gboolean satisfied;
      int index_addend = 1;

      sum += fabs (temp - last_temp);
      desired = n_all * step;
      satisfied = sum >= desired;
      while (satisfied && n_all < divisions)
        {
          all[n_all++] = hue;
          desired = (n_all + index_addend) * step;
          satisfied = sum >= desired;
          index_addend++;
        }
      last_temp = temp;
      addend++;
      if (addend > 360)
        {
          while (n_all < divisions)
            all[n_all++] = hue;
          break;
        }
    }
  ccw = (int) floor ((count - 1.0) / 2.0);
  cw = count - ccw - 1;
  for (int i = ccw; i >= 1; i--)
    {
      int index = -i;

      while (index < 0)
        index += n_all;
      out[n_out++] = t->hcts[all[index % n_all]];
    }
  out[n_out++] = t->hcts[361];
  for (int i = 1; i <= cw; i++)
    out[n_out++] = t->hcts[all[i % n_all]];
}

/* ---- the palettes of each style --------------------------------------------------------------------------------- */

static void
palettes_2021 (TonalScheme *s)
{
  static const double expressive_bp[] = { 0, 21, 51, 121, 151, 191, 271, 321, 360 };
  static const double expressive_secondary[] = { 45, 95, 45, 20, 45, 90, 45, 45, 45 };
  static const double expressive_tertiary[] = { 120, 120, 20, 45, 20, 15, 20, 120, 120 };
  static const double vibrant_bp[] = { 0, 41, 61, 101, 131, 181, 251, 301, 360 };
  static const double vibrant_secondary[] = { 18, 15, 10, 12, 15, 18, 15, 12, 12 };
  static const double vibrant_tertiary[] = { 35, 30, 20, 25, 30, 35, 30, 25, 25 };
  TonalPalette *p = s->palettes;
  double hue = s->source_hct.hue, chroma = s->source_hct.chroma;

  switch (s->variant)
    {
    case TONAL_VARIANT_CONTENT:
    case TONAL_VARIANT_FIDELITY:
      {
        Temperatures *t = g_new (Temperatures, 1);
        TonalHct tertiary;

        tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, chroma);
        tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, MAX (chroma - 32.0, chroma * 0.5));
        temperatures_init (t, s->source, &s->source_hct);
        if (s->variant == TONAL_VARIANT_CONTENT)
          {
            TonalHct around[3];

            analogous (t, 3, 6, around);
            tertiary = fix_if_disliked (around[2]);
          }
        else
          tertiary = fix_if_disliked (complement (t));
        g_free (t);
        tonal_palette_init_hct (&p[TONAL_PALETTE_TERTIARY], &tertiary);
        tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, chroma / 8.0);
        tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, chroma / 8.0 + 4.0);
      }
      break;
    case TONAL_VARIANT_FRUIT_SALAD:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], tonal_sanitize_degrees (hue - 50.0), 48.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], tonal_sanitize_degrees (hue - 50.0), 36.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], hue, 36.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 10.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 16.0);
      break;
    case TONAL_VARIANT_MONOCHROME:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 0.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, 0.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], hue, 0.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 0.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 0.0);
      break;
    case TONAL_VARIANT_NEUTRAL:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 12.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, 8.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], hue, 16.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 2.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 2.0);
      break;
    case TONAL_VARIANT_RAINBOW:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 48.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, 16.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], tonal_sanitize_degrees (hue + 60.0), 24.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 0.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 0.0);
      break;
    case TONAL_VARIANT_TONAL_SPOT:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 36.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, 16.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], tonal_sanitize_degrees (hue + 60.0), 24.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 6.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 8.0);
      break;
    case TONAL_VARIANT_EXPRESSIVE:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], tonal_sanitize_degrees (hue + 240), 40);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], ROTATED (hue, expressive_bp, expressive_secondary), 24.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, expressive_bp, expressive_tertiary), 32.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], tonal_sanitize_degrees (hue + 15), 8);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], tonal_sanitize_degrees (hue + 15), 12);
      break;
    case TONAL_VARIANT_VIBRANT:
    default:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 200.0);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], ROTATED (hue, vibrant_bp, vibrant_secondary), 24.0);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, vibrant_bp, vibrant_tertiary), 32.0);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 10);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 12);
      break;
    }
  tonal_palette_init (&p[TONAL_PALETTE_ERROR], 25.0, 84.0);
}

static double
expressive_neutral_hue (double hue)
{
  static const double bp[] = { 0, 71, 124, 253, 278, 300, 360 };
  static const double rot[] = { 10, 0, 10, 0, 10, 0 };

  return ROTATED (hue, bp, rot);
}

static double
expressive_neutral_chroma (double hue, gboolean dark)
{
  return dark ? (tonal_hue_is_yellow (expressive_neutral_hue (hue)) ? 6 : 14) : 18;
}

static double
vibrant_neutral_hue (double hue)
{
  static const double bp[] = { 0, 38, 105, 140, 333, 360 };
  static const double rot[] = { -14, 10, -14, 10, -14 };

  return ROTATED (hue, bp, rot);
}

/* the current rules, for the Neutral, Tonal spot, Expressive and Vibrant styles */
static void
palettes_2025 (TonalScheme *s)
{
  static const double expressive_bp[] = { 0, 105, 140, 204, 253, 278, 300, 333, 360 };
  static const double expressive_secondary[] = { -160, 155, -100, 96, -96, -156, -165, -160 };
  static const double expressive_tertiary[] = { -165, 160, -105, 101, -101, -160, -170, -165 };
  static const double vibrant_secondary_bp[] = { 0, 38, 105, 140, 333, 360 };
  static const double vibrant_secondary[] = { -14, 10, -14, 10, -14 };
  static const double vibrant_tertiary_bp[] = { 0, 38, 71, 105, 140, 161, 253, 333, 360 };
  static const double vibrant_tertiary[] = { -72, 35, 24, -24, 62, 50, 62, -72 };
  static const double neutral_tertiary_bp[] = { 0, 38, 105, 161, 204, 278, 333, 360 };
  static const double neutral_tertiary[] = { -32, 26, 10, -39, 24, -15, -32 };
  static const double tonal_spot_tertiary_bp[] = { 0, 20, 71, 161, 333, 360 };
  static const double tonal_spot_tertiary[] = { -40, 48, -32, 40, -32 };
  static const double error_bp[] = { 0, 3, 13, 23, 33, 43, 153, 273, 360 };
  static const double error_hues[] = { 12, 22, 32, 12, 22, 32, 22, 12 };
  TonalPalette *p = s->palettes;
  double hue = s->source_hct.hue;
  gboolean dark = s->dark;
  double error_hue = PIECEWISE (hue, error_bp, error_hues);

  switch (s->variant)
    {
    case TONAL_VARIANT_NEUTRAL:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, tonal_hue_is_blue (hue) ? 12 : 8);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, tonal_hue_is_blue (hue) ? 6 : 4);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, neutral_tertiary_bp, neutral_tertiary), 20);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 1.4);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 1.4 * 2.2);
      tonal_palette_init (&p[TONAL_PALETTE_ERROR], error_hue, 50);
      break;
    case TONAL_VARIANT_TONAL_SPOT:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, dark ? 26 : 32);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], hue, 16);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, tonal_spot_tertiary_bp, tonal_spot_tertiary),
                          28);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], hue, 5);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], hue, 5 * 1.7);
      tonal_palette_init (&p[TONAL_PALETTE_ERROR], error_hue, 60);
      break;
    case TONAL_VARIANT_EXPRESSIVE:
      {
        double neutral_hue = expressive_neutral_hue (hue), neutral_chroma = expressive_neutral_chroma (hue, dark);

        tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, dark ? 36 : 48);
        tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], ROTATED (hue, expressive_bp, expressive_secondary),
                            dark ? 16 : 24);
        tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, expressive_bp, expressive_tertiary), 48);
        tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], neutral_hue, neutral_chroma);
        tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], neutral_hue,
                            neutral_chroma * (neutral_hue >= 105 && neutral_hue < 125 ? 1.6 : 2.3));
        tonal_palette_init (&p[TONAL_PALETTE_ERROR], error_hue, 64);
      }
      break;
    case TONAL_VARIANT_VIBRANT:
    default:
      tonal_palette_init (&p[TONAL_PALETTE_PRIMARY], hue, 74);
      tonal_palette_init (&p[TONAL_PALETTE_SECONDARY], ROTATED (hue, vibrant_secondary_bp, vibrant_secondary), 56);
      tonal_palette_init (&p[TONAL_PALETTE_TERTIARY], ROTATED (hue, vibrant_tertiary_bp, vibrant_tertiary), 56);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL], vibrant_neutral_hue (hue), 28);
      tonal_palette_init (&p[TONAL_PALETTE_NEUTRAL_VARIANT], vibrant_neutral_hue (hue), 28 * 1.29);
      tonal_palette_init (&p[TONAL_PALETTE_ERROR], error_hue, 80);
      break;
    }
}

static TonalSpec
effective_spec (TonalSpec spec, TonalVariant variant)
{
  switch (variant)
    {
    case TONAL_VARIANT_EXPRESSIVE:
    case TONAL_VARIANT_VIBRANT:
    case TONAL_VARIANT_TONAL_SPOT:
    case TONAL_VARIANT_NEUTRAL:
      return spec;
    default:
      return TONAL_SPEC_2021;
    }
}

void
tonal_scheme_init (TonalScheme *scheme, guint32 source, TonalVariant variant, gboolean dark, double contrast,
                   TonalSpec spec)
{
  scheme->source = source | 0xff000000u;
  scheme->source_hct = tonal_hct_from_argb (scheme->source);
  scheme->variant = variant;
  scheme->dark = dark;
  scheme->contrast = contrast;
  scheme->spec = effective_spec (spec, variant);
  if (scheme->spec == TONAL_SPEC_2025)
    palettes_2025 (scheme);
  else
    palettes_2021 (scheme);
  for (int i = 0; i < TONAL_N_ROLES; i++)
    scheme->tones[i] = NAN;
}

guint32
tonal_scheme_stop (const TonalScheme *scheme, TonalPaletteId palette, int stop)
{
  return tonal_palette_stop (&scheme->palettes[palette], stop);
}

/* ---- the roles' definitions ------------------------------------------------------------------------------------- */

#define NONE (-1)

/* the contrast ratio a role needs at contrast levels -1, 0, 0.5 and 1 */
typedef struct {
  double low, normal, medium, high;
} Curve;

static double
curve_get (const Curve *curve, double level)
{
  if (level <= -1.0)
    return curve->low;
  else if (level < 0.0)
    return tonal_lerp (curve->low, curve->normal, (level - -1) / 1);
  else if (level < 0.5)
    return tonal_lerp (curve->normal, curve->medium, (level - 0) / 0.5);
  else if (level < 1.0)
    return tonal_lerp (curve->medium, curve->high, (level - 0.5) / 0.5);
  return curve->high;
}

typedef enum {
  DARKER,
  LIGHTER,
  NEARER,             /* role a nearer the background than role b */
  RELATIVE_DARKER,    /* darker in light mode, lighter in dark mode */
  RELATIVE_LIGHTER,   /* lighter in light mode, darker in dark mode */
} Polarity;

typedef enum {
  EXACT,
  KEEP_NEARER,        /* at most `delta` away */
  KEEP_FARTHER,       /* at least `delta` away */
} Constraint;

typedef struct {
  const TonalPalette *palette;
  gboolean   is_background;     /* a role other roles stand on */
  double     chroma_multiplier;
  int        background;        /* the role it must contrast with, or NONE */
  int        second_background;
  gboolean   has_curve;
  Curve      curve;
  gboolean   has_pair;          /* a tone distance to keep from another role */
  int        pair_a, pair_b;
  double     delta;
  Polarity   polarity;          /* of a, compared with b */
  gboolean   stay_together;     /* both on the same side of tones 50-59 */
  Constraint constraint;
} Def;

static gboolean
is_background (TonalRole role)
{
  switch (role)
    {
    case TONAL_BACKGROUND:
    case TONAL_SURFACE:
    case TONAL_SURFACE_DIM:
    case TONAL_SURFACE_BRIGHT:
    case TONAL_SURFACE_CONTAINER_LOWEST:
    case TONAL_SURFACE_CONTAINER_LOW:
    case TONAL_SURFACE_CONTAINER:
    case TONAL_SURFACE_CONTAINER_HIGH:
    case TONAL_SURFACE_CONTAINER_HIGHEST:
    case TONAL_SURFACE_VARIANT:
    case TONAL_INVERSE_SURFACE:
    case TONAL_SURFACE_TINT:
    case TONAL_PRIMARY:
    case TONAL_PRIMARY_CONTAINER:
    case TONAL_PRIMARY_FIXED:
    case TONAL_PRIMARY_FIXED_DIM:
    case TONAL_SECONDARY:
    case TONAL_SECONDARY_CONTAINER:
    case TONAL_SECONDARY_FIXED:
    case TONAL_SECONDARY_FIXED_DIM:
    case TONAL_TERTIARY:
    case TONAL_TERTIARY_CONTAINER:
    case TONAL_TERTIARY_FIXED:
    case TONAL_TERTIARY_FIXED_DIM:
    case TONAL_ERROR:
    case TONAL_ERROR_CONTAINER:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
is_fixed_dim (TonalRole role)
{
  return role == TONAL_PRIMARY_FIXED_DIM || role == TONAL_SECONDARY_FIXED_DIM || role == TONAL_TERTIARY_FIXED_DIM;
}

static void
set_background (Def *d, int background, double low, double normal, double medium, double high)
{
  d->background = background;
  d->has_curve = TRUE;
  d->curve = (Curve) { low, normal, medium, high };
}

static void
set_pair (Def *d, TonalRole a, TonalRole b, double delta, Polarity polarity, gboolean stay_together,
          Constraint constraint)
{
  d->has_pair = TRUE;
  d->pair_a = a;
  d->pair_b = b;
  d->delta = delta;
  d->polarity = polarity;
  d->stay_together = stay_together;
  d->constraint = constraint;
}

static int
highest_surface (const TonalScheme *s)
{
  return s->dark ? TONAL_SURFACE_BRIGHT : TONAL_SURFACE_DIM;
}

static void
def_init (Def *d, const TonalScheme *s, TonalPaletteId palette, TonalRole role)
{
  *d = (Def) {
    .palette = &s->palettes[palette],
    .is_background = is_background (role),
    .chroma_multiplier = 1.0,
    .background = NONE,
    .second_background = NONE,
  };
}

/* the original rules */
static void
def_2021 (const TonalScheme *s, TonalRole role, Def *d)
{
  int highest = highest_surface (s);

  switch (role)
    {
    case TONAL_PRIMARY_PALETTE_KEY_COLOR:
    case TONAL_SURFACE_TINT:
    case TONAL_PRIMARY:
    case TONAL_ON_PRIMARY:
    case TONAL_PRIMARY_CONTAINER:
    case TONAL_ON_PRIMARY_CONTAINER:
    case TONAL_PRIMARY_FIXED:
    case TONAL_PRIMARY_FIXED_DIM:
    case TONAL_ON_PRIMARY_FIXED:
    case TONAL_ON_PRIMARY_FIXED_VARIANT:
    case TONAL_INVERSE_PRIMARY:
      def_init (d, s, TONAL_PALETTE_PRIMARY, role);
      break;
    case TONAL_SECONDARY_PALETTE_KEY_COLOR:
    case TONAL_SECONDARY:
    case TONAL_ON_SECONDARY:
    case TONAL_SECONDARY_CONTAINER:
    case TONAL_ON_SECONDARY_CONTAINER:
    case TONAL_SECONDARY_FIXED:
    case TONAL_SECONDARY_FIXED_DIM:
    case TONAL_ON_SECONDARY_FIXED:
    case TONAL_ON_SECONDARY_FIXED_VARIANT:
      def_init (d, s, TONAL_PALETTE_SECONDARY, role);
      break;
    case TONAL_TERTIARY_PALETTE_KEY_COLOR:
    case TONAL_TERTIARY:
    case TONAL_ON_TERTIARY:
    case TONAL_TERTIARY_CONTAINER:
    case TONAL_ON_TERTIARY_CONTAINER:
    case TONAL_TERTIARY_FIXED:
    case TONAL_TERTIARY_FIXED_DIM:
    case TONAL_ON_TERTIARY_FIXED:
    case TONAL_ON_TERTIARY_FIXED_VARIANT:
      def_init (d, s, TONAL_PALETTE_TERTIARY, role);
      break;
    case TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR:
    case TONAL_SURFACE_VARIANT:
    case TONAL_ON_SURFACE_VARIANT:
    case TONAL_OUTLINE:
    case TONAL_OUTLINE_VARIANT:
      def_init (d, s, TONAL_PALETTE_NEUTRAL_VARIANT, role);
      break;
    case TONAL_ERROR_PALETTE_KEY_COLOR:
    case TONAL_ERROR:
    case TONAL_ON_ERROR:
    case TONAL_ERROR_CONTAINER:
    case TONAL_ON_ERROR_CONTAINER:
      def_init (d, s, TONAL_PALETTE_ERROR, role);
      break;
    default:
      def_init (d, s, TONAL_PALETTE_NEUTRAL, role);
      break;
    }

  switch (role)
    {
    case TONAL_ON_BACKGROUND:
      set_background (d, TONAL_BACKGROUND, 3, 3, 4.5, 7);
      break;
    case TONAL_ON_SURFACE:
      set_background (d, highest, 4.5, 7, 11, 21);
      break;
    case TONAL_ON_SURFACE_VARIANT:
      set_background (d, highest, 3, 4.5, 7, 11);
      break;
    case TONAL_INVERSE_ON_SURFACE:
      set_background (d, TONAL_INVERSE_SURFACE, 4.5, 7, 11, 21);
      break;
    case TONAL_OUTLINE:
      set_background (d, highest, 1.5, 3, 4.5, 7);
      break;
    case TONAL_OUTLINE_VARIANT:
      set_background (d, highest, 1, 1, 3, 4.5);
      break;

    case TONAL_PRIMARY:
    case TONAL_SECONDARY:
    case TONAL_TERTIARY:
    case TONAL_ERROR:
      set_background (d, highest, 3, 4.5, 7, 7);
      set_pair (d, role + 2, role, 10, NEARER, FALSE, EXACT);
      break;
    case TONAL_PRIMARY_CONTAINER:
    case TONAL_SECONDARY_CONTAINER:
    case TONAL_TERTIARY_CONTAINER:
    case TONAL_ERROR_CONTAINER:
      set_background (d, highest, 1, 1, 3, 4.5);
      set_pair (d, role, role - 2, 10, NEARER, FALSE, EXACT);
      break;
    case TONAL_ON_PRIMARY:
    case TONAL_ON_SECONDARY:
    case TONAL_ON_TERTIARY:
    case TONAL_ON_ERROR:
      set_background (d, role - 1, 4.5, 7, 11, 21);
      break;
    case TONAL_ON_PRIMARY_CONTAINER:
    case TONAL_ON_SECONDARY_CONTAINER:
    case TONAL_ON_TERTIARY_CONTAINER:
    case TONAL_ON_ERROR_CONTAINER:
      set_background (d, role - 1, 3, 4.5, 7, 11);
      break;
    case TONAL_INVERSE_PRIMARY:
      set_background (d, TONAL_INVERSE_SURFACE, 3, 4.5, 7, 7);
      break;

    case TONAL_PRIMARY_FIXED:
    case TONAL_SECONDARY_FIXED:
    case TONAL_TERTIARY_FIXED:
      set_background (d, highest, 1, 1, 3, 4.5);
      set_pair (d, role, role + 1, 10, LIGHTER, TRUE, EXACT);
      break;
    case TONAL_PRIMARY_FIXED_DIM:
    case TONAL_SECONDARY_FIXED_DIM:
    case TONAL_TERTIARY_FIXED_DIM:
      set_background (d, highest, 1, 1, 3, 4.5);
      set_pair (d, role - 1, role, 10, LIGHTER, TRUE, EXACT);
      break;
    case TONAL_ON_PRIMARY_FIXED:
    case TONAL_ON_SECONDARY_FIXED:
    case TONAL_ON_TERTIARY_FIXED:
      set_background (d, role - 1, 4.5, 7, 11, 21);
      d->second_background = role - 2;
      break;
    case TONAL_ON_PRIMARY_FIXED_VARIANT:
    case TONAL_ON_SECONDARY_FIXED_VARIANT:
    case TONAL_ON_TERTIARY_FIXED_VARIANT:
      set_background (d, role - 2, 3, 4.5, 7, 11);
      d->second_background = role - 3;
      break;
    default:
      break;
    }
}

/* the contrast curves of the current rules, by their standard ratio */
static void
set_background_2025 (Def *d, int background, double normal)
{
  if (normal == 1.5)
    set_background (d, background, 1.5, 1.5, 3, 5.5);
  else if (normal == 3)
    set_background (d, background, 3, 3, 4.5, 7);
  else if (normal == 4.5)
    set_background (d, background, 4.5, 4.5, 7, 11);
  else if (normal == 6)
    set_background (d, background, 6, 6, 7, 11);
  else if (normal == 7)
    set_background (d, background, 7, 7, 11, 21);
  else if (normal == 9)
    set_background (d, background, 9, 9, 11, 21);
  else if (normal == 11)
    set_background (d, background, 11, 11, 21, 21);
  else
    set_background (d, background, 21, 21, 21, 21);
}

/* the chroma multipliers of the neutral roles, by style */
static double
neutral_multiplier (const TonalScheme *s, double neutral, double tonal_spot, double expressive_yellow,
                    double expressive, double vibrant)
{
  switch (s->variant)
    {
    case TONAL_VARIANT_NEUTRAL:
      return neutral;
    case TONAL_VARIANT_TONAL_SPOT:
      return tonal_spot;
    case TONAL_VARIANT_EXPRESSIVE:
      return tonal_hue_is_yellow (s->palettes[TONAL_PALETTE_NEUTRAL].hue) ? expressive_yellow : expressive;
    case TONAL_VARIANT_VIBRANT:
      return vibrant;
    default:
      return 1;
    }
}

/* the current rules (for a phone-sized screen and up, which is every Android device but a watch) */
static void
def_2025 (const TonalScheme *s, TonalRole role, Def *d)
{
  int highest = highest_surface (s);
  gboolean more_contrast = s->contrast > 0;

  switch (role)
    {
    case TONAL_PRIMARY_PALETTE_KEY_COLOR:
    case TONAL_SECONDARY_PALETTE_KEY_COLOR:
    case TONAL_TERTIARY_PALETTE_KEY_COLOR:
    case TONAL_NEUTRAL_PALETTE_KEY_COLOR:
    case TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR:
    case TONAL_ERROR_PALETTE_KEY_COLOR:
    case TONAL_SHADOW:
    case TONAL_SCRIM:
      def_2021 (s, role, d);
      return;
    /* these follow other roles now */
    case TONAL_BACKGROUND:
      def_2025 (s, TONAL_SURFACE, d);
      return;
    case TONAL_ON_BACKGROUND:
      def_2025 (s, TONAL_ON_SURFACE, d);
      return;
    case TONAL_SURFACE_VARIANT:
      def_2025 (s, TONAL_SURFACE_CONTAINER_HIGHEST, d);
      return;
    case TONAL_SURFACE_TINT:
      def_2025 (s, TONAL_PRIMARY, d);
      return;
    default:
      break;
    }

  def_2021 (s, role, d);
  d->chroma_multiplier = 1.0;
  d->background = d->second_background = NONE;
  d->has_curve = d->has_pair = FALSE;
  if (d->palette == &s->palettes[TONAL_PALETTE_NEUTRAL_VARIANT])
    d->palette = &s->palettes[TONAL_PALETTE_NEUTRAL];

  switch (role)
    {
    case TONAL_SURFACE_DIM:
      if (!s->dark)
        d->chroma_multiplier = neutral_multiplier (s, 2.5, 1.7, 2.7, 1.75, 1.36);
      break;
    case TONAL_SURFACE_BRIGHT:
      if (s->dark)
        d->chroma_multiplier = neutral_multiplier (s, 2.5, 1.7, 2.7, 1.75, 1.36);
      break;
    case TONAL_SURFACE_CONTAINER_LOW:
      d->chroma_multiplier = neutral_multiplier (s, 1.3, 1.25, 1.3, 1.15, 1.08);
      break;
    case TONAL_SURFACE_CONTAINER:
      d->chroma_multiplier = neutral_multiplier (s, 1.6, 1.4, 1.6, 1.3, 1.15);
      break;
    case TONAL_SURFACE_CONTAINER_HIGH:
      d->chroma_multiplier = neutral_multiplier (s, 1.9, 1.5, 1.95, 1.45, 1.22);
      break;
    case TONAL_SURFACE_CONTAINER_HIGHEST:
      d->chroma_multiplier = neutral_multiplier (s, 2.2, 1.7, 2.3, 1.6, 1.29);
      break;
    case TONAL_ON_SURFACE:
    case TONAL_ON_SURFACE_VARIANT:
    case TONAL_OUTLINE:
    case TONAL_OUTLINE_VARIANT:
      d->chroma_multiplier = neutral_multiplier (s, 2.2, 1.7, s->dark ? 3.0 : 2.3, 1.6, 1);
      set_background_2025 (d, highest, role == TONAL_ON_SURFACE ? (s->dark ? 11 : 9)
                                       : role == TONAL_ON_SURFACE_VARIANT ? (s->dark ? 6 : 4.5)
                                       : role == TONAL_OUTLINE ? 3 : 1.5);
      break;
    case TONAL_INVERSE_ON_SURFACE:
      set_background_2025 (d, TONAL_INVERSE_SURFACE, 7);
      break;

    case TONAL_PRIMARY:
    case TONAL_SECONDARY:
    case TONAL_TERTIARY:
    case TONAL_ERROR:
      set_background_2025 (d, highest, 4.5);
      set_pair (d, role + 2, role, 5, RELATIVE_LIGHTER, TRUE, KEEP_FARTHER);
      break;
    case TONAL_PRIMARY_CONTAINER:
    case TONAL_SECONDARY_CONTAINER:
    case TONAL_TERTIARY_CONTAINER:
    case TONAL_ERROR_CONTAINER:
    case TONAL_PRIMARY_FIXED:
    case TONAL_SECONDARY_FIXED:
    case TONAL_TERTIARY_FIXED:
      d->background = highest;
      if (more_contrast)
        set_background_2025 (d, highest, 1.5);
      break;
    case TONAL_ON_PRIMARY:
    case TONAL_ON_SECONDARY:
    case TONAL_ON_TERTIARY:
    case TONAL_ON_ERROR:
      set_background_2025 (d, role - 1, 6);
      break;
    case TONAL_ON_PRIMARY_CONTAINER:
    case TONAL_ON_SECONDARY_CONTAINER:
    case TONAL_ON_TERTIARY_CONTAINER:
      set_background_2025 (d, role - 1, 6);
      break;
    case TONAL_ON_ERROR_CONTAINER:
      set_background_2025 (d, role - 1, 4.5);
      break;
    case TONAL_PRIMARY_FIXED_DIM:
    case TONAL_SECONDARY_FIXED_DIM:
    case TONAL_TERTIARY_FIXED_DIM:
      set_pair (d, role, role - 1, 5, DARKER, TRUE, EXACT);
      break;
    case TONAL_ON_PRIMARY_FIXED:
    case TONAL_ON_SECONDARY_FIXED:
    case TONAL_ON_TERTIARY_FIXED:
      set_background_2025 (d, role - 1, 7);
      break;
    case TONAL_ON_PRIMARY_FIXED_VARIANT:
    case TONAL_ON_SECONDARY_FIXED_VARIANT:
    case TONAL_ON_TERTIARY_FIXED_VARIANT:
      set_background_2025 (d, role - 2, 4.5);
      break;
    case TONAL_INVERSE_PRIMARY:
      set_background_2025 (d, TONAL_INVERSE_SURFACE, 6);
      break;
    default:
      break;
    }
}

static void
def (const TonalScheme *s, TonalRole role, Def *d)
{
  if (s->spec == TONAL_SPEC_2025)
    def_2025 (s, role, d);
  else
    def_2021 (s, role, d);
}

/* ---- starting tones ----------------------------------------------------------------------------------------------- */

static double get_tone (TonalScheme *s, TonalRole role);

static gboolean
prefers_light_foreground (double tone)
{
  return tonal_round (tone) < 60;
}

/* the tone for text on `bg_tone` closest to `ratio`, lighter or darker as reads best */
static double
foreground_tone (double bg_tone, double ratio)
{
  double lighter = tonal_contrast_lighter_unsafe (bg_tone, ratio);
  double darker = tonal_contrast_darker_unsafe (bg_tone, ratio);
  double lighter_ratio = tonal_contrast_ratio (lighter, bg_tone);
  double darker_ratio = tonal_contrast_ratio (darker, bg_tone);

  if (prefers_light_foreground (bg_tone))
    {
      gboolean negligible = fabs (lighter_ratio - darker_ratio) < 0.1 && lighter_ratio < ratio && darker_ratio < ratio;

      return lighter_ratio >= ratio || lighter_ratio >= darker_ratio || negligible ? lighter : darker;
    }
  return darker_ratio >= ratio || darker_ratio >= lighter_ratio ? darker : lighter;
}

/* a role without a tone of its own starts at its background's */
static double
tone_from_background (TonalScheme *s, TonalRole role)
{
  Def d;

  def (s, role, &d);
  return d.background != NONE ? get_tone (s, d.background) : 50;
}

/* the original rules' search for the tone of a chroma: from `tone` towards black or white, until the chroma is
 * reached or stops growing */
static double
find_desired_chroma_by_tone_2021 (double hue, double chroma, double tone, gboolean by_decreasing_tone)
{
  double answer = tone;
  TonalHct closest = tonal_hct_from_argb (tonal_hct_to_argb (hue, chroma, tone));

  if (closest.chroma < chroma)
    {
      double peak = closest.chroma;

      while (closest.chroma < chroma)
        {
          TonalHct candidate;

          answer += by_decreasing_tone ? -1.0 : 1.0;
          candidate = tonal_hct_from_argb (tonal_hct_to_argb (hue, chroma, answer));
          if (peak > candidate.chroma)
            break;
          if (fabs (candidate.chroma - chroma) < 0.4)
            break;
          if (fabs (candidate.chroma - chroma) < fabs (closest.chroma - chroma))
            closest = candidate;
          peak = MAX (peak, candidate.chroma);
        }
    }
  return answer;
}

static double
tone_2021 (TonalScheme *s, TonalRole role)
{
  gboolean dark = s->dark, mono = s->variant == TONAL_VARIANT_MONOCHROME;
  gboolean fidelity = s->variant == TONAL_VARIANT_FIDELITY || s->variant == TONAL_VARIANT_CONTENT;
  double level = s->contrast;

  switch (role)
    {
    case TONAL_PRIMARY_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_PRIMARY].key.tone;
    case TONAL_SECONDARY_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_SECONDARY].key.tone;
    case TONAL_TERTIARY_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_TERTIARY].key.tone;
    case TONAL_NEUTRAL_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_NEUTRAL].key.tone;
    case TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_NEUTRAL_VARIANT].key.tone;
    case TONAL_ERROR_PALETTE_KEY_COLOR:
      return s->palettes[TONAL_PALETTE_ERROR].key.tone;
    case TONAL_BACKGROUND:
    case TONAL_SURFACE:
      return dark ? 6 : 98;
    case TONAL_ON_BACKGROUND:
    case TONAL_ON_SURFACE:
      return dark ? 90 : 10;
    case TONAL_SURFACE_DIM:
      return dark ? 6 : curve_get (&(Curve) { 87, 87, 80, 75 }, level);
    case TONAL_SURFACE_BRIGHT:
      return dark ? curve_get (&(Curve) { 24, 24, 29, 34 }, level) : 98;
    case TONAL_SURFACE_CONTAINER_LOWEST:
      return dark ? curve_get (&(Curve) { 4, 4, 2, 0 }, level) : 100;
    case TONAL_SURFACE_CONTAINER_LOW:
      return dark ? curve_get (&(Curve) { 10, 10, 11, 12 }, level) : curve_get (&(Curve) { 96, 96, 96, 95 }, level);
    case TONAL_SURFACE_CONTAINER:
      return dark ? curve_get (&(Curve) { 12, 12, 16, 20 }, level) : curve_get (&(Curve) { 94, 94, 92, 90 }, level);
    case TONAL_SURFACE_CONTAINER_HIGH:
      return dark ? curve_get (&(Curve) { 17, 17, 21, 25 }, level) : curve_get (&(Curve) { 92, 92, 88, 85 }, level);
    case TONAL_SURFACE_CONTAINER_HIGHEST:
      return dark ? curve_get (&(Curve) { 22, 22, 26, 30 }, level) : curve_get (&(Curve) { 90, 90, 84, 80 }, level);
    case TONAL_SURFACE_VARIANT:
      return dark ? 30 : 90;
    case TONAL_ON_SURFACE_VARIANT:
      return dark ? 80 : 30;
    case TONAL_INVERSE_SURFACE:
      return dark ? 90 : 20;
    case TONAL_INVERSE_ON_SURFACE:
      return dark ? 20 : 95;
    case TONAL_OUTLINE:
      return dark ? 60 : 50;
    case TONAL_OUTLINE_VARIANT:
      return dark ? 30 : 80;
    case TONAL_SHADOW:
    case TONAL_SCRIM:
      return 0;
    case TONAL_SURFACE_TINT:
      return dark ? 80 : 40;

    case TONAL_PRIMARY:
      return mono ? (dark ? 100 : 0) : (dark ? 80 : 40);
    case TONAL_ON_PRIMARY:
      return mono ? (dark ? 10 : 90) : (dark ? 20 : 100);
    case TONAL_PRIMARY_CONTAINER:
      if (fidelity)
        return s->source_hct.tone;
      return mono ? (dark ? 85 : 25) : (dark ? 30 : 90);
    case TONAL_ON_PRIMARY_CONTAINER:
      if (fidelity)
        return foreground_tone (tone_2021 (s, TONAL_PRIMARY_CONTAINER), 4.5);
      return mono ? (dark ? 0 : 100) : (dark ? 90 : 30);
    case TONAL_INVERSE_PRIMARY:
      return dark ? 40 : 80;

    case TONAL_SECONDARY:
      return dark ? 80 : 40;
    case TONAL_ON_SECONDARY:
      return mono ? (dark ? 10 : 100) : (dark ? 20 : 100);
    case TONAL_SECONDARY_CONTAINER:
      if (mono)
        return dark ? 30 : 85;
      if (!fidelity)
        return dark ? 30 : 90;
      return find_desired_chroma_by_tone_2021 (s->palettes[TONAL_PALETTE_SECONDARY].hue,
                                               s->palettes[TONAL_PALETTE_SECONDARY].chroma, dark ? 30 : 90, !dark);
    case TONAL_ON_SECONDARY_CONTAINER:
      if (mono)
        return dark ? 90 : 10;
      if (!fidelity)
        return dark ? 90 : 30;
      return foreground_tone (tone_2021 (s, TONAL_SECONDARY_CONTAINER), 4.5);

    case TONAL_TERTIARY:
      return mono ? (dark ? 90 : 25) : (dark ? 80 : 40);
    case TONAL_ON_TERTIARY:
      return mono ? (dark ? 10 : 90) : (dark ? 20 : 100);
    case TONAL_TERTIARY_CONTAINER:
      if (mono)
        return dark ? 60 : 49;
      if (!fidelity)
        return dark ? 30 : 90;
      return fix_if_disliked (tonal_hct_from_argb (tonal_palette_tone (&s->palettes[TONAL_PALETTE_TERTIARY],
                                                                       s->source_hct.tone))).tone;
    case TONAL_ON_TERTIARY_CONTAINER:
      if (mono)
        return dark ? 0 : 100;
      if (!fidelity)
        return dark ? 90 : 30;
      return foreground_tone (tone_2021 (s, TONAL_TERTIARY_CONTAINER), 4.5);

    case TONAL_ERROR:
      return dark ? 80 : 40;
    case TONAL_ON_ERROR:
      return dark ? 20 : 100;
    case TONAL_ERROR_CONTAINER:
      return dark ? 30 : 90;
    case TONAL_ON_ERROR_CONTAINER:
      return mono ? (dark ? 90 : 10) : (dark ? 90 : 30);

    case TONAL_PRIMARY_FIXED:
    case TONAL_TERTIARY_FIXED:
      return mono ? 40.0 : 90.0;
    case TONAL_PRIMARY_FIXED_DIM:
    case TONAL_TERTIARY_FIXED_DIM:
      return mono ? 30.0 : 80.0;
    case TONAL_ON_PRIMARY_FIXED:
    case TONAL_ON_TERTIARY_FIXED:
      return mono ? 100.0 : 10.0;
    case TONAL_ON_PRIMARY_FIXED_VARIANT:
    case TONAL_ON_TERTIARY_FIXED_VARIANT:
      return mono ? 90.0 : 30.0;
    case TONAL_SECONDARY_FIXED:
      return mono ? 80.0 : 90.0;
    case TONAL_SECONDARY_FIXED_DIM:
      return mono ? 70.0 : 80.0;
    case TONAL_ON_SECONDARY_FIXED:
      return 10.0;
    case TONAL_ON_SECONDARY_FIXED_VARIANT:
      return mono ? 25.0 : 30.0;
    default:
      return 50;
    }
}

/* the current rules' search for a palette's most colourful tone: from `tone` towards black or white while the
 * chroma grows */
static double
find_best_tone_for_chroma (double hue, double chroma, double tone, gboolean by_decreasing_tone)
{
  double answer = tone;
  double best = tonal_hct_from_argb (tonal_hct_to_argb (hue, chroma, answer)).chroma;

  while (best < chroma)
    {
      double candidate;

      if (tone < 0 || tone > 100)
        break;
      tone += by_decreasing_tone ? -1.0 : 1.0;
      candidate = tonal_hct_from_argb (tonal_hct_to_argb (hue, chroma, tone)).chroma;
      if (best < candidate)
        {
          best = candidate;
          answer = tone;
        }
    }
  return answer;
}

/* the lightest tone at which the palette is most colourful, within [lower, upper] */
static double
t_max_c (const TonalPalette *palette, double lower, double upper, double chroma_multiplier)
{
  return tonal_clamp (lower, upper,
                      find_best_tone_for_chroma (palette->hue, palette->chroma * chroma_multiplier, 100, TRUE));
}

/* the darkest tone at which it is most colourful, within [lower, upper] */
static double
t_min_c (const TonalPalette *palette, double lower, double upper)
{
  return tonal_clamp (lower, upper, find_best_tone_for_chroma (palette->hue, palette->chroma, 0, FALSE));
}

/* a fixed role's tone: its container's in light mode at standard contrast */
static double
fixed_tone (const TonalScheme *s, TonalRole container)
{
  TonalScheme light = *s;

  light.dark = FALSE;
  light.contrast = 0.0;
  for (int i = 0; i < TONAL_N_ROLES; i++)
    light.tones[i] = NAN;
  return get_tone (&light, container);
}

static double
tone_2025 (TonalScheme *s, TonalRole role)
{
  const TonalPalette *p = &s->palettes[TONAL_PALETTE_PRIMARY], *q = &s->palettes[TONAL_PALETTE_SECONDARY];
  const TonalPalette *t = &s->palettes[TONAL_PALETTE_TERTIARY], *e = &s->palettes[TONAL_PALETTE_ERROR];
  gboolean dark = s->dark, yellow = tonal_hue_is_yellow (s->palettes[TONAL_PALETTE_NEUTRAL].hue);
  TonalVariant v = s->variant;
  gboolean vibrant = v == TONAL_VARIANT_VIBRANT;

  switch (role)
    {
    case TONAL_BACKGROUND:
    case TONAL_SURFACE:
      return dark ? 4 : yellow ? 99 : vibrant ? 97 : 98;
    case TONAL_SURFACE_DIM:
      return dark ? 4 : yellow ? 90 : vibrant ? 85 : 87;
    case TONAL_SURFACE_BRIGHT:
      return dark ? 18 : yellow ? 99 : vibrant ? 97 : 98;
    case TONAL_SURFACE_CONTAINER_LOWEST:
      return dark ? 0 : 100;
    case TONAL_SURFACE_CONTAINER_LOW:
      return dark ? 6 : yellow ? 98 : vibrant ? 95 : 96;
    case TONAL_SURFACE_CONTAINER:
      return dark ? 9 : yellow ? 96 : vibrant ? 92 : 94;
    case TONAL_SURFACE_CONTAINER_HIGH:
      return dark ? 12 : yellow ? 94 : vibrant ? 90 : 92;
    case TONAL_SURFACE_VARIANT:
    case TONAL_SURFACE_CONTAINER_HIGHEST:
      return dark ? 15 : yellow ? 92 : vibrant ? 88 : 90;
    case TONAL_ON_SURFACE:
      if (vibrant)
        return t_max_c (&s->palettes[TONAL_PALETTE_NEUTRAL], 0, 100, 1.1);
      return get_tone (s, highest_surface (s));
    case TONAL_ON_BACKGROUND:
      return get_tone (s, TONAL_ON_SURFACE);
    case TONAL_INVERSE_SURFACE:
      return dark ? 98 : 4;

    case TONAL_SURFACE_TINT:
    case TONAL_PRIMARY:
      if (v == TONAL_VARIANT_NEUTRAL)
        return dark ? 80 : 40;
      if (v == TONAL_VARIANT_TONAL_SPOT)
        return dark ? 80 : t_max_c (p, 0, 100, 1);
      if (v == TONAL_VARIANT_EXPRESSIVE)
        return t_max_c (p, 0, dark ? (tonal_hue_is_cyan (p->hue) ? 88 : 98) : (tonal_hue_is_yellow (p->hue) ? 25 : 98),
                        1);
      return t_max_c (p, 0, tonal_hue_is_cyan (p->hue) ? 88 : 98, 1);
    case TONAL_PRIMARY_CONTAINER:
      if (v == TONAL_VARIANT_NEUTRAL)
        return dark ? 30 : 90;
      if (v == TONAL_VARIANT_TONAL_SPOT)
        return dark ? t_min_c (p, 35, 93) : t_max_c (p, 0, 90, 1);
      if (v == TONAL_VARIANT_EXPRESSIVE)
        return dark ? t_min_c (p, 30, 93) : t_max_c (p, 78, tonal_hue_is_cyan (p->hue) ? 88 : 90, 1);
      return dark ? t_min_c (p, 66, 93) : t_max_c (p, 66, tonal_hue_is_cyan (p->hue) ? 88 : 93, 1);
    case TONAL_PRIMARY_FIXED:
      return fixed_tone (s, TONAL_PRIMARY_CONTAINER);
    case TONAL_PRIMARY_FIXED_DIM:
      return get_tone (s, TONAL_PRIMARY_FIXED);
    case TONAL_INVERSE_PRIMARY:
      return t_max_c (p, 0, 100, 1);

    case TONAL_SECONDARY:
      if (v == TONAL_VARIANT_NEUTRAL)
        return dark ? t_min_c (q, 0, 98) : t_max_c (q, 0, 100, 1);
      if (vibrant)
        return t_max_c (q, 0, dark ? 90 : 98, 1);
      return dark ? 80 : t_max_c (q, 0, 100, 1);
    case TONAL_SECONDARY_CONTAINER:
      if (vibrant)
        return dark ? t_min_c (q, 30, 40) : t_max_c (q, 84, 90, 1);
      if (v == TONAL_VARIANT_EXPRESSIVE)
        return dark ? 15 : t_max_c (q, 90, 95, 1);
      return dark ? 25 : 90;
    case TONAL_SECONDARY_FIXED:
      return fixed_tone (s, TONAL_SECONDARY_CONTAINER);
    case TONAL_SECONDARY_FIXED_DIM:
      return get_tone (s, TONAL_SECONDARY_FIXED);

    case TONAL_TERTIARY:
      if (v == TONAL_VARIANT_EXPRESSIVE || vibrant)
        return t_max_c (t, 0, tonal_hue_is_cyan (t->hue) ? 88 : (dark ? 98 : 100), 1);
      return dark ? t_max_c (t, 0, 98, 1) : t_max_c (t, 0, 100, 1);
    case TONAL_TERTIARY_CONTAINER:
      if (v == TONAL_VARIANT_NEUTRAL)
        return dark ? t_max_c (t, 0, 93, 1) : t_max_c (t, 0, 96, 1);
      if (v == TONAL_VARIANT_TONAL_SPOT)
        return t_max_c (t, 0, dark ? 93 : 100, 1);
      if (v == TONAL_VARIANT_EXPRESSIVE)
        return t_max_c (t, 75, tonal_hue_is_cyan (t->hue) ? 88 : (dark ? 93 : 100), 1);
      return dark ? t_max_c (t, 0, 93, 1) : t_max_c (t, 72, 100, 1);
    case TONAL_TERTIARY_FIXED:
      return fixed_tone (s, TONAL_TERTIARY_CONTAINER);
    case TONAL_TERTIARY_FIXED_DIM:
      return get_tone (s, TONAL_TERTIARY_FIXED);

    case TONAL_ERROR:
      return dark ? t_min_c (e, 0, 98) : t_max_c (e, 0, 100, 1);
    case TONAL_ERROR_CONTAINER:
      return dark ? t_min_c (e, 30, 93) : t_max_c (e, 0, 90, 1);

    case TONAL_PRIMARY_PALETTE_KEY_COLOR:
    case TONAL_SECONDARY_PALETTE_KEY_COLOR:
    case TONAL_TERTIARY_PALETTE_KEY_COLOR:
    case TONAL_NEUTRAL_PALETTE_KEY_COLOR:
    case TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR:
    case TONAL_ERROR_PALETTE_KEY_COLOR:
    case TONAL_SHADOW:
    case TONAL_SCRIM:
      return tone_2021 (s, role);
    default:
      /* the on_ roles, outline and outline_variant */
      return tone_from_background (s, role);
    }
}

static double
tone_of (TonalScheme *s, TonalRole role)
{
  return s->spec == TONAL_SPEC_2025 ? tone_2025 (s, role) : tone_2021 (s, role);
}

/* ---- the final tones ---------------------------------------------------------------------------------------------- */

/* a tone that contrasts with two backgrounds at once */
static double
dual_background (TonalScheme *s, const Def *d, double answer, double desired)
{
  double tone1 = get_tone (s, d->background), tone2 = get_tone (s, d->second_background);
  double upper = MAX (tone1, tone2), lower = MIN (tone1, tone2);
  double light, dark;

  if (tonal_contrast_ratio (upper, answer) >= desired && tonal_contrast_ratio (lower, answer) >= desired)
    return answer;
  light = tonal_contrast_lighter (upper, desired);
  dark = tonal_contrast_darker (lower, desired);
  if (prefers_light_foreground (tone1) || prefers_light_foreground (tone2))
    return light < 0 ? 100 : light;
  if ((light != -1) + (dark != -1) == 1)
    return light != -1 ? light : dark;
  return dark < 0 ? 0 : dark;
}

static double
get_tone_2021 (TonalScheme *s, TonalRole role)
{
  gboolean decreasing = s->contrast < 0;
  double answer, bg_tone, desired;
  Def d;

  def (s, role, &d);
  if (d.has_pair)
    {
      gboolean a_is_nearer = d.polarity == NEARER || (d.polarity == LIGHTER && !s->dark)
                             || (d.polarity == DARKER && !s->dark);
      int nearer = a_is_nearer ? d.pair_a : d.pair_b, farther = a_is_nearer ? d.pair_b : d.pair_a;
      double dir = s->dark ? 1 : -1, delta = d.delta;
      double n_tone = tone_of (s, nearer), f_tone = tone_of (s, farther);
      Def dn, df;

      def (s, nearer, &dn);
      def (s, farther, &df);
      if (d.background != NONE && dn.has_curve && df.has_curve)
        {
          double n_contrast = curve_get (&dn.curve, s->contrast), f_contrast = curve_get (&df.curve, s->contrast);

          bg_tone = get_tone (s, d.background);
          if (tonal_contrast_ratio (bg_tone, n_tone) < n_contrast)
            n_tone = foreground_tone (bg_tone, n_contrast);
          if (tonal_contrast_ratio (bg_tone, f_tone) < f_contrast)
            f_tone = foreground_tone (bg_tone, f_contrast);
          if (decreasing)
            {
              n_tone = foreground_tone (bg_tone, n_contrast);
              f_tone = foreground_tone (bg_tone, f_contrast);
            }
        }
      if ((f_tone - n_tone) * dir < delta)
        {
          f_tone = tonal_clamp (0, 100, n_tone + delta * dir);
          if ((f_tone - n_tone) * dir < delta)
            n_tone = tonal_clamp (0, 100, f_tone - delta * dir);
        }
      /* tones 50-59 are an awkward middle: neither light nor dark text reads on them */
      if (50 <= n_tone && n_tone < 60)
        {
          if (dir > 0)
            {
              n_tone = 60;
              f_tone = MAX (f_tone, n_tone + delta * dir);
            }
          else
            {
              n_tone = 49;
              f_tone = MIN (f_tone, n_tone + delta * dir);
            }
        }
      else if (50 <= f_tone && f_tone < 60)
        {
          if (d.stay_together)
            {
              if (dir > 0)
                {
                  n_tone = 60;
                  f_tone = MAX (f_tone, n_tone + delta * dir);
                }
              else
                {
                  n_tone = 49;
                  f_tone = MIN (f_tone, n_tone + delta * dir);
                }
            }
          else
            f_tone = dir > 0 ? 60 : 49;
        }
      return (int) role == nearer ? n_tone : f_tone;
    }

  answer = tone_of (s, role);
  if (d.background == NONE || !d.has_curve)
    return answer;
  bg_tone = get_tone (s, d.background);
  desired = curve_get (&d.curve, s->contrast);
  if (tonal_contrast_ratio (bg_tone, answer) < desired)
    answer = foreground_tone (bg_tone, desired);
  if (decreasing)
    answer = foreground_tone (bg_tone, desired);
  if (d.is_background && 50 <= answer && answer < 60)
    answer = tonal_contrast_ratio (49, bg_tone) >= desired ? 49 : 60;
  if (d.second_background == NONE)
    return answer;
  return dual_background (s, &d, answer, desired);
}

static double
get_tone_2025 (TonalScheme *s, TonalRole role)
{
  double answer, bg_tone, desired;
  Def d;

  def (s, role, &d);
  if (d.has_pair)
    {
      double absolute = d.polarity == DARKER || (d.polarity == RELATIVE_LIGHTER && s->dark)
                        || (d.polarity == RELATIVE_DARKER && !s->dark) ? -d.delta : d.delta;
      gboolean am_a = (int) role == d.pair_a;
      int self = am_a ? d.pair_a : d.pair_b, reference = am_a ? d.pair_b : d.pair_a;
      double self_tone = tone_of (s, self), reference_tone = get_tone (s, reference);
      double relative = absolute * (am_a ? 1 : -1);

      switch (d.constraint)
        {
        case EXACT:
          self_tone = tonal_clamp (0, 100, reference_tone + relative);
          break;
        case KEEP_NEARER:
          if (relative > 0)
            self_tone = tonal_clamp (0, 100, tonal_clamp (reference_tone, reference_tone + relative, self_tone));
          else
            self_tone = tonal_clamp (0, 100, tonal_clamp (reference_tone + relative, reference_tone, self_tone));
          break;
        case KEEP_FARTHER:
          if (relative > 0)
            self_tone = tonal_clamp (reference_tone + relative, 100, self_tone);
          else
            self_tone = tonal_clamp (0, reference_tone + relative, self_tone);
          break;
        }
      if (d.background != NONE && d.has_curve)
        {
          bg_tone = get_tone (s, d.background);
          desired = curve_get (&d.curve, s->contrast);
          if (!(tonal_contrast_ratio (bg_tone, self_tone) >= desired && s->contrast >= 0))
            self_tone = foreground_tone (bg_tone, desired);
        }
      /* backgrounds keep out of tones 50-56 (and are lifted to 65 from 57 up); the fixed_dim roles keep their
       * distance from their fixed role instead */
      if (d.is_background && !is_fixed_dim (role))
        self_tone = self_tone >= 57 ? tonal_clamp (65, 100, self_tone) : tonal_clamp (0, 49, self_tone);
      return self_tone;
    }

  answer = tone_of (s, role);
  if (d.background == NONE || !d.has_curve)
    return answer;
  bg_tone = get_tone (s, d.background);
  desired = curve_get (&d.curve, s->contrast);
  if (!(tonal_contrast_ratio (bg_tone, answer) >= desired && s->contrast >= 0))
    answer = foreground_tone (bg_tone, desired);
  if (d.is_background && !is_fixed_dim (role))
    answer = answer >= 57 ? tonal_clamp (65, 100, answer) : tonal_clamp (0, 49, answer);
  if (d.second_background == NONE)
    return answer;
  return dual_background (s, &d, answer, desired);
}

static double
get_tone (TonalScheme *s, TonalRole role)
{
  if (isnan (s->tones[role]))
    s->tones[role] = s->spec == TONAL_SPEC_2025 ? get_tone_2025 (s, role) : get_tone_2021 (s, role);
  return s->tones[role];
}

double
tonal_scheme_tone (TonalScheme *scheme, TonalRole role)
{
  g_return_val_if_fail (role < TONAL_N_ROLES, 0);
  return get_tone (scheme, role);
}

guint32
tonal_scheme_color (TonalScheme *scheme, TonalRole role)
{
  double tone;
  Def d;

  g_return_val_if_fail (role < TONAL_N_ROLES, 0);
  def (scheme, role, &d);
  tone = get_tone (scheme, role);
  if (scheme->spec == TONAL_SPEC_2021 || d.chroma_multiplier == 1.0)
    return tonal_palette_tone (d.palette, tone);
  /* the chroma scaled: a tone of a more (or less) colourful palette of the same hue */
  return tonal_palette_tone_of (d.palette->hue, d.palette->chroma * d.chroma_multiplier, tone);
}
