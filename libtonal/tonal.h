/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * libtonal: Material 3's colours from one seed colour, computed the way Android computes its system colours.
 *
 *   HCT          a colour as hue, chroma and tone (CAM16 hue and chroma, L* tone), and back to the nearest sRGB
 *   palettes     a hue and a chroma at every tone; Android names thirteen of the tones as "stops"
 *   schemes      a seed, a style (variant), light or dark and a contrast level give every colour role: primary,
 *                on_surface, surface_container_high, ... Two rule sets exist: the original one (2021) and the
 *                current one (2025), which Android uses for the Tonal spot, Neutral, Vibrant and Expressive
 *                styles; the other styles keep the original rules there too, and so they do here
 *   seeds        the colours of an image (a quantizer: Wu's cuts, then weighted k-means) and the ones worth
 *                theming with, best first (Score)
 *
 * Plain C on GLib, no GTK: the launcher, the tools and libvibeos all use it. Colours are 0xAARRGGBB. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ---- HCT -------------------------------------------------------------------------------------------------------- */

typedef struct {
  double hue;           /* 0 <= hue < 360 */
  double chroma;
  double tone;          /* 0 (black) .. 100 (white) */
} TonalHct;

TonalHct tonal_hct_from_argb (guint32 argb);
/* the sRGB colour with this hue and tone whose chroma is the closest to `chroma` (opaque) */
guint32  tonal_hct_to_argb   (double hue, double chroma, double tone);

/* the WCAG contrast ratio of two tones, 1 .. 21 */
double   tonal_contrast_ratio (double tone_a, double tone_b);

/* ---- tonal palettes --------------------------------------------------------------------------------------------- */

typedef struct {
  double   hue;
  double   chroma;
  TonalHct key;         /* the palette's key colour: its chroma at the tone nearest 50 that reaches it */
} TonalPalette;

void     tonal_palette_init     (TonalPalette *palette, double hue, double chroma);
void     tonal_palette_init_hct (TonalPalette *palette, const TonalHct *hct);
/* the palette's colour at `tone` */
guint32  tonal_palette_tone     (const TonalPalette *palette, double tone);

/* Android's stops, the suffixes of system_accent1_0 ... system_neutral2_1000: stop 0 is tone 100, 10 is 99, 50 is
 * 95, 100 is 90, 200 is 80, and so on down to 900 (tone 10) and 1000 (tone 0) */
#define TONAL_N_STOPS 13
extern const int tonal_stops[TONAL_N_STOPS];
/* the colour at stop `stop` (one of tonal_stops) */
guint32  tonal_palette_stop     (const TonalPalette *palette, int stop);

/* ---- schemes ---------------------------------------------------------------------------------------------------- */

/* the styles; their names (tonal_variant_name) are the ones vibeos.Theme's "style" key takes */
typedef enum {
  TONAL_VARIANT_MONOCHROME,
  TONAL_VARIANT_NEUTRAL,
  TONAL_VARIANT_TONAL_SPOT,
  TONAL_VARIANT_VIBRANT,
  TONAL_VARIANT_EXPRESSIVE,
  TONAL_VARIANT_FIDELITY,
  TONAL_VARIANT_CONTENT,
  TONAL_VARIANT_RAINBOW,
  TONAL_VARIANT_FRUIT_SALAD,
  TONAL_N_VARIANTS
} TonalVariant;

typedef enum {
  TONAL_SPEC_2021,
  TONAL_SPEC_2025,
} TonalSpec;

/* The colour roles. tonal_role_name gives each one's name as Android's resources spell it after "system_" (and before
 * "_light" or "_dark"): "primary", "on_surface", "surface_container_high". */
typedef enum {
  TONAL_PRIMARY_PALETTE_KEY_COLOR,
  TONAL_SECONDARY_PALETTE_KEY_COLOR,
  TONAL_TERTIARY_PALETTE_KEY_COLOR,
  TONAL_NEUTRAL_PALETTE_KEY_COLOR,
  TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR,
  TONAL_ERROR_PALETTE_KEY_COLOR,
  TONAL_BACKGROUND,
  TONAL_ON_BACKGROUND,
  TONAL_SURFACE,
  TONAL_SURFACE_DIM,
  TONAL_SURFACE_BRIGHT,
  TONAL_SURFACE_CONTAINER_LOWEST,
  TONAL_SURFACE_CONTAINER_LOW,
  TONAL_SURFACE_CONTAINER,
  TONAL_SURFACE_CONTAINER_HIGH,
  TONAL_SURFACE_CONTAINER_HIGHEST,
  TONAL_ON_SURFACE,
  TONAL_SURFACE_VARIANT,
  TONAL_ON_SURFACE_VARIANT,
  TONAL_OUTLINE,
  TONAL_OUTLINE_VARIANT,
  TONAL_INVERSE_SURFACE,
  TONAL_INVERSE_ON_SURFACE,
  TONAL_SHADOW,
  TONAL_SCRIM,
  TONAL_SURFACE_TINT,
  TONAL_PRIMARY,
  TONAL_ON_PRIMARY,
  TONAL_PRIMARY_CONTAINER,
  TONAL_ON_PRIMARY_CONTAINER,
  TONAL_PRIMARY_FIXED,
  TONAL_PRIMARY_FIXED_DIM,
  TONAL_ON_PRIMARY_FIXED,
  TONAL_ON_PRIMARY_FIXED_VARIANT,
  TONAL_INVERSE_PRIMARY,
  TONAL_SECONDARY,
  TONAL_ON_SECONDARY,
  TONAL_SECONDARY_CONTAINER,
  TONAL_ON_SECONDARY_CONTAINER,
  TONAL_SECONDARY_FIXED,
  TONAL_SECONDARY_FIXED_DIM,
  TONAL_ON_SECONDARY_FIXED,
  TONAL_ON_SECONDARY_FIXED_VARIANT,
  TONAL_TERTIARY,
  TONAL_ON_TERTIARY,
  TONAL_TERTIARY_CONTAINER,
  TONAL_ON_TERTIARY_CONTAINER,
  TONAL_TERTIARY_FIXED,
  TONAL_TERTIARY_FIXED_DIM,
  TONAL_ON_TERTIARY_FIXED,
  TONAL_ON_TERTIARY_FIXED_VARIANT,
  TONAL_ERROR,
  TONAL_ON_ERROR,
  TONAL_ERROR_CONTAINER,
  TONAL_ON_ERROR_CONTAINER,
  TONAL_N_ROLES
} TonalRole;

/* the scheme's palettes; tonal_palette_name gives Android's names: accent1, accent2, accent3, neutral1, neutral2, and
 * error */
typedef enum {
  TONAL_PALETTE_PRIMARY,
  TONAL_PALETTE_SECONDARY,
  TONAL_PALETTE_TERTIARY,
  TONAL_PALETTE_NEUTRAL,
  TONAL_PALETTE_NEUTRAL_VARIANT,
  TONAL_PALETTE_ERROR,
  TONAL_N_PALETTES
} TonalPaletteId;

typedef struct {
  guint32      source;
  TonalHct     source_hct;
  TonalVariant variant;
  gboolean     dark;
  double       contrast;          /* -1 (least) .. 0 (standard) .. 0.5 (medium) .. 1 (high) */
  TonalSpec    spec;              /* the rules the roles follow (see the top of this file) */
  TonalPalette palettes[TONAL_N_PALETTES];
  /*< private >*/
  double       tones[TONAL_N_ROLES];
} TonalScheme;

/* A scheme for the seed `source`. `spec` is the rule set asked for: TONAL_SPEC_2025 is what Android uses. The
 * palettes depend on light or dark too: Tonal spot's primary palette is a little less colourful in dark. */
void     tonal_scheme_init  (TonalScheme *scheme, guint32 source, TonalVariant variant, gboolean dark,
                             double contrast, TonalSpec spec);
guint32  tonal_scheme_color (TonalScheme *scheme, TonalRole role);
double   tonal_scheme_tone  (TonalScheme *scheme, TonalRole role);
guint32  tonal_scheme_stop  (const TonalScheme *scheme, TonalPaletteId palette, int stop);

const char *tonal_role_name         (TonalRole role);
gboolean    tonal_role_from_name    (const char *name, TonalRole *role);
const char *tonal_variant_name      (TonalVariant variant);
gboolean    tonal_variant_from_name (const char *name, TonalVariant *variant);
const char *tonal_palette_name      (TonalPaletteId palette);

/* ---- a theme: what VibeOS publishes for one seed -------------------------------------------------------------- */

typedef struct {
  guint32 light[TONAL_N_ROLES];
  guint32 dark[TONAL_N_ROLES];
  guint32 light_stops[TONAL_N_PALETTES][TONAL_N_STOPS];
  guint32 dark_stops[TONAL_N_PALETTES][TONAL_N_STOPS];
} TonalTheme;

/* Both schemes of a seed by the current rules, and the palettes' stops in each. `pure_black` turns the dark scheme's
 * background surfaces (background, surface, surface_dim, surface_container_lowest) black; the containers above them
 * keep their tones. */
void     tonal_theme_compute (TonalTheme *theme, guint32 seed, TonalVariant variant, double contrast,
                              gboolean pure_black);

/* ---- seeds from an image ---------------------------------------------------------------------------------------- */

typedef struct {
  guint32 argb;
  guint   count;          /* the pixels it stands for */
} TonalColorCount;

/* Up to `max_colors` colours that stand for `pixels` (opaque 0xAARRGGBB), with their pixel counts: Wu's quantizer
 * picks the starting clusters, weighted k-means in L*a*b* refines them. An array of TonalColorCount. */
GArray  *tonal_quantize        (const guint32 *pixels, gsize n_pixels, guint max_colors);

/* The colours of `colors` worth theming with, best first, at most `desired` and at least 1 (`fallback` when none
 * qualifies). With `filter`, colours that are nearly grey or that cover too little of the image are left out. An
 * array of guint32. */
GArray  *tonal_score           (const TonalColorCount *colors, guint n_colors, guint desired, guint32 fallback,
                                gboolean filter);

/* The seeds of an image, best first, as a wallpaper's are found: the image (`width` x `height` opaque pixels, rows
 * `stride` pixels apart) is scaled down to at most 112 x 112 pixels' worth of area, quantized to 128 colours and
 * scored for 4. Fully transparent pixels are left out. An array of guint32. */
GArray  *tonal_seeds_from_image (const guint32 *pixels, int width, int height, int stride);

G_END_DECLS
