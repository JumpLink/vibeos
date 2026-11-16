/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2025 Google LLC (Material Color Utilities, of which this is a port to C)
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * The names of the roles, styles and palettes, and a theme: both schemes of a seed and their palettes' stops. */
#include "tonal-private.h"

#include <string.h>

static const char *const role_names[TONAL_N_ROLES] = {
  [TONAL_PRIMARY_PALETTE_KEY_COLOR] = "primary_palette_key_color",
  [TONAL_SECONDARY_PALETTE_KEY_COLOR] = "secondary_palette_key_color",
  [TONAL_TERTIARY_PALETTE_KEY_COLOR] = "tertiary_palette_key_color",
  [TONAL_NEUTRAL_PALETTE_KEY_COLOR] = "neutral_palette_key_color",
  [TONAL_NEUTRAL_VARIANT_PALETTE_KEY_COLOR] = "neutral_variant_palette_key_color",
  [TONAL_ERROR_PALETTE_KEY_COLOR] = "error_palette_key_color",
  [TONAL_BACKGROUND] = "background",
  [TONAL_ON_BACKGROUND] = "on_background",
  [TONAL_SURFACE] = "surface",
  [TONAL_SURFACE_DIM] = "surface_dim",
  [TONAL_SURFACE_BRIGHT] = "surface_bright",
  [TONAL_SURFACE_CONTAINER_LOWEST] = "surface_container_lowest",
  [TONAL_SURFACE_CONTAINER_LOW] = "surface_container_low",
  [TONAL_SURFACE_CONTAINER] = "surface_container",
  [TONAL_SURFACE_CONTAINER_HIGH] = "surface_container_high",
  [TONAL_SURFACE_CONTAINER_HIGHEST] = "surface_container_highest",
  [TONAL_ON_SURFACE] = "on_surface",
  [TONAL_SURFACE_VARIANT] = "surface_variant",
  [TONAL_ON_SURFACE_VARIANT] = "on_surface_variant",
  [TONAL_OUTLINE] = "outline",
  [TONAL_OUTLINE_VARIANT] = "outline_variant",
  [TONAL_INVERSE_SURFACE] = "inverse_surface",
  [TONAL_INVERSE_ON_SURFACE] = "inverse_on_surface",
  [TONAL_SHADOW] = "shadow",
  [TONAL_SCRIM] = "scrim",
  [TONAL_SURFACE_TINT] = "surface_tint",
  [TONAL_PRIMARY] = "primary",
  [TONAL_ON_PRIMARY] = "on_primary",
  [TONAL_PRIMARY_CONTAINER] = "primary_container",
  [TONAL_ON_PRIMARY_CONTAINER] = "on_primary_container",
  [TONAL_PRIMARY_FIXED] = "primary_fixed",
  [TONAL_PRIMARY_FIXED_DIM] = "primary_fixed_dim",
  [TONAL_ON_PRIMARY_FIXED] = "on_primary_fixed",
  [TONAL_ON_PRIMARY_FIXED_VARIANT] = "on_primary_fixed_variant",
  [TONAL_INVERSE_PRIMARY] = "inverse_primary",
  [TONAL_SECONDARY] = "secondary",
  [TONAL_ON_SECONDARY] = "on_secondary",
  [TONAL_SECONDARY_CONTAINER] = "secondary_container",
  [TONAL_ON_SECONDARY_CONTAINER] = "on_secondary_container",
  [TONAL_SECONDARY_FIXED] = "secondary_fixed",
  [TONAL_SECONDARY_FIXED_DIM] = "secondary_fixed_dim",
  [TONAL_ON_SECONDARY_FIXED] = "on_secondary_fixed",
  [TONAL_ON_SECONDARY_FIXED_VARIANT] = "on_secondary_fixed_variant",
  [TONAL_TERTIARY] = "tertiary",
  [TONAL_ON_TERTIARY] = "on_tertiary",
  [TONAL_TERTIARY_CONTAINER] = "tertiary_container",
  [TONAL_ON_TERTIARY_CONTAINER] = "on_tertiary_container",
  [TONAL_TERTIARY_FIXED] = "tertiary_fixed",
  [TONAL_TERTIARY_FIXED_DIM] = "tertiary_fixed_dim",
  [TONAL_ON_TERTIARY_FIXED] = "on_tertiary_fixed",
  [TONAL_ON_TERTIARY_FIXED_VARIANT] = "on_tertiary_fixed_variant",
  [TONAL_ERROR] = "error",
  [TONAL_ON_ERROR] = "on_error",
  [TONAL_ERROR_CONTAINER] = "error_container",
  [TONAL_ON_ERROR_CONTAINER] = "on_error_container",
};

static const char *const variant_names[TONAL_N_VARIANTS] = {
  [TONAL_VARIANT_MONOCHROME] = "monochrome",
  [TONAL_VARIANT_NEUTRAL] = "neutral",
  [TONAL_VARIANT_TONAL_SPOT] = "tonal-spot",
  [TONAL_VARIANT_VIBRANT] = "vibrant",
  [TONAL_VARIANT_EXPRESSIVE] = "expressive",
  [TONAL_VARIANT_FIDELITY] = "fidelity",
  [TONAL_VARIANT_CONTENT] = "content",
  [TONAL_VARIANT_RAINBOW] = "rainbow",
  [TONAL_VARIANT_FRUIT_SALAD] = "fruit-salad",
};

static const char *const palette_names[TONAL_N_PALETTES] = {
  [TONAL_PALETTE_PRIMARY] = "accent1",
  [TONAL_PALETTE_SECONDARY] = "accent2",
  [TONAL_PALETTE_TERTIARY] = "accent3",
  [TONAL_PALETTE_NEUTRAL] = "neutral1",
  [TONAL_PALETTE_NEUTRAL_VARIANT] = "neutral2",
  [TONAL_PALETTE_ERROR] = "error",
};

const char *
tonal_role_name (TonalRole role)
{
  g_return_val_if_fail (role < TONAL_N_ROLES, NULL);
  return role_names[role];
}

gboolean
tonal_role_from_name (const char *name, TonalRole *role)
{
  for (int i = 0; i < TONAL_N_ROLES; i++)
    if (g_strcmp0 (name, role_names[i]) == 0)
      {
        *role = i;
        return TRUE;
      }
  return FALSE;
}

const char *
tonal_variant_name (TonalVariant variant)
{
  g_return_val_if_fail (variant < TONAL_N_VARIANTS, NULL);
  return variant_names[variant];
}

gboolean
tonal_variant_from_name (const char *name, TonalVariant *variant)
{
  for (int i = 0; i < TONAL_N_VARIANTS; i++)
    if (g_strcmp0 (name, variant_names[i]) == 0)
      {
        *variant = i;
        return TRUE;
      }
  return FALSE;
}

const char *
tonal_palette_name (TonalPaletteId palette)
{
  g_return_val_if_fail (palette < TONAL_N_PALETTES, NULL);
  return palette_names[palette];
}

static void
compute_one (TonalScheme *scheme, guint32 *roles, guint32 (*stops)[TONAL_N_STOPS])
{
  for (int i = 0; i < TONAL_N_ROLES; i++)
    roles[i] = tonal_scheme_color (scheme, i);
  for (int p = 0; p < TONAL_N_PALETTES; p++)
    for (int i = 0; i < TONAL_N_STOPS; i++)
      stops[p][i] = tonal_scheme_stop (scheme, p, tonal_stops[i]);
}

void
tonal_theme_compute (TonalTheme *theme, guint32 seed, TonalVariant variant, double contrast, gboolean pure_black)
{
  TonalScheme light, dark;

  tonal_scheme_init (&light, seed, variant, FALSE, contrast, TONAL_SPEC_2025);
  tonal_scheme_init (&dark, seed, variant, TRUE, contrast, TONAL_SPEC_2025);
  compute_one (&light, theme->light, theme->light_stops);
  compute_one (&dark, theme->dark, theme->dark_stops);
  if (pure_black)
    {
      theme->dark[TONAL_BACKGROUND] = 0xff000000u;
      theme->dark[TONAL_SURFACE] = 0xff000000u;
      theme->dark[TONAL_SURFACE_DIM] = 0xff000000u;
      theme->dark[TONAL_SURFACE_CONTAINER_LOWEST] = 0xff000000u;
    }
}
