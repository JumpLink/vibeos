/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * libtonal against colours recorded on real devices: every role and palette stop must match to the byte. */
#include "tonal.h"

#include <math.h>
#include <string.h>

typedef struct {
  TonalRole role;
  guint32   light, dark;
} RoleCase;

/* a tablet whose wallpaper gave the seed #24222D, Tonal spot, standard contrast */
static const RoleCase tablet_roles[] = {
  { TONAL_SURFACE, 0xFDF8FE, 0x0E0E12 },
  { TONAL_BACKGROUND, 0xFDF8FE, 0x0E0E12 },
  { TONAL_SURFACE_DIM, 0xDDD8E4, 0x0E0E12 },
  { TONAL_SURFACE_BRIGHT, 0xFDF8FE, 0x2D2B34 },
  { TONAL_SURFACE_CONTAINER_LOWEST, 0xFFFFFF, 0x000000 },
  { TONAL_SURFACE_CONTAINER, 0xF1ECF6, 0x1A191F },
  { TONAL_SURFACE_CONTAINER_HIGH, 0xEBE6F1, 0x201F26 },
  { TONAL_SURFACE_CONTAINER_HIGHEST, 0xE5E0ED, 0x26252D },
  { TONAL_ON_SURFACE, 0x33313A, 0xE8E3F0 },
  { TONAL_ON_SURFACE_VARIANT, 0x605E68, 0xADA9B5 },
  { TONAL_OUTLINE, 0x7C7984, 0x77747E },
  { TONAL_OUTLINE_VARIANT, 0xB4B0BC, 0x494750 },
  { TONAL_PRIMARY, 0x60598A, 0xC9C1ED },
  { TONAL_ON_PRIMARY, 0xFCF7FF, 0x413B60 },
  { TONAL_PRIMARY_CONTAINER, 0xCCC2FB, 0x544E73 },
  { TONAL_ON_PRIMARY_CONTAINER, 0x433B6B, 0xE6DFFF },
  { TONAL_SECONDARY_CONTAINER, 0xE6DFF9, 0x3C394D },
  { TONAL_ON_SECONDARY_CONTAINER, 0x534F64, 0xC2BCD5 },
  { TONAL_ON_SECONDARY, 0xFCF7FF, 0x413E52 },
};

/* its palettes, stops 0 .. 900 (stop 1000 is black in every palette) */
static const guint32 tablet_light_stops[5][12] = {
  { 0xFFFFFF, 0xFFFBFF, 0xF4EEFF, 0xE6DEFF, 0xC9BFF8, 0xAEA4DB, 0x938ABF, 0x7970A4, 0x60588A, 0x484070, 0x312A58,
    0x1C1442 },
  { 0xFFFFFF, 0xFFFBFF, 0xF4EEFF, 0xE6DFF9, 0xC9C3DC, 0xAEA8C0, 0x938EA5, 0x79748B, 0x605C71, 0x484459, 0x312E41,
    0x1C192B },
  { 0xFFFFFF, 0xFFFBFF, 0xFFEBF8, 0xFFD7F7, 0xE7B8DF, 0xCA9DC3, 0xAE83A8, 0x926A8D, 0x785273, 0x5E3B5B, 0x452543,
    0x2E0F2D },
  { 0xFFFFFF, 0xFFFBFF, 0xF4EFF6, 0xE5E1E7, 0xC9C5CB, 0xADAAB0, 0x938F95, 0x79767C, 0x605D63, 0x48464B, 0x313034,
    0x1C1B1F },
  { 0xFFFFFF, 0xFFFBFF, 0xF4EEFB, 0xE5E0ED, 0xC9C4D0, 0xADA9B5, 0x938F9A, 0x797580, 0x605D67, 0x48454F, 0x312F38,
    0x1C1A23 },
};

/* in dark mode the primary palette is the less colourful one the system prints */
static const guint32 tablet_dark_accent1[12] = {
  0xFFFFFF, 0xFFFBFF, 0xF4EEFF, 0xE6DEFF, 0xC9C1ED, 0xAEA6D1, 0x938CB5, 0x79729A, 0x605980, 0x484267, 0x312C4F,
  0x1C1639,
};

static void
check (const char *what, guint32 got, guint32 want, int *failures)
{
  if ((got & 0xffffff) == (want & 0xffffff))
    return;
  g_printerr ("%s: got #%06X, want #%06X\n", what, got & 0xffffff, want & 0xffffff);
  (*failures)++;
}

static void
check_roles (guint32 seed, const RoleCase *cases, gsize n_cases, gboolean light, gboolean dark)
{
  int failures = 0;

  for (int mode = 0; mode < 2; mode++)
    {
      TonalScheme scheme;

      if ((mode == 0 && !light) || (mode == 1 && !dark))
        continue;
      tonal_scheme_init (&scheme, seed, TONAL_VARIANT_TONAL_SPOT, mode == 1, 0.0, TONAL_SPEC_2025);
      for (gsize i = 0; i < n_cases; i++)
        {
          g_autofree char *what = g_strdup_printf ("%s %s", mode ? "dark" : "light", tonal_role_name (cases[i].role));

          check (what, tonal_scheme_color (&scheme, cases[i].role), mode ? cases[i].dark : cases[i].light, &failures);
        }
    }
  g_assert_cmpint (failures, ==, 0);
}

static void
test_tablet_roles (void)
{
  check_roles (0x24222D, tablet_roles, G_N_ELEMENTS (tablet_roles), TRUE, TRUE);
}

static void
test_tablet_palettes (void)
{
  TonalScheme light, dark;
  int failures = 0;

  tonal_scheme_init (&light, 0x24222D, TONAL_VARIANT_TONAL_SPOT, FALSE, 0.0, TONAL_SPEC_2025);
  tonal_scheme_init (&dark, 0x24222D, TONAL_VARIANT_TONAL_SPOT, TRUE, 0.0, TONAL_SPEC_2025);
  for (int p = 0; p < 5; p++)
    for (int i = 0; i < TONAL_N_STOPS; i++)
      {
        g_autofree char *what = g_strdup_printf ("light %s_%d", tonal_palette_name (p), tonal_stops[i]);
        guint32 want = i < 12 ? tablet_light_stops[p][i] : 0x000000;

        check (what, tonal_scheme_stop (&light, p, tonal_stops[i]), want, &failures);
        g_free (what);
        what = g_strdup_printf ("dark %s_%d", tonal_palette_name (p), tonal_stops[i]);
        want = i == 12 ? 0x000000 : p == 0 ? tablet_dark_accent1[i] : tablet_light_stops[p][i];
        check (what, tonal_scheme_stop (&dark, p, tonal_stops[i]), want, &failures);
      }
  g_assert_cmpint (failures, ==, 0);
}

/* a phone whose wallpaper's seed is not on record: any seed of hue 208.25 to 208.31 reproduces every colour it
 * shows, and #007C88 is one (it is vibeos.Theme's default seed, so that the default theme is the phone's) */
#define PHONE_SEED 0x007C88

static const RoleCase phone_roles[] = {
  { TONAL_PRIMARY, 0x1c6872, 0x9ccfd7 },
  { TONAL_ON_PRIMARY, 0xebfcff, 0x0d474e },
  { TONAL_PRIMARY_CONTAINER, 0xa9eef8, 0x265a61 },
  { TONAL_ON_PRIMARY_CONTAINER, 0x005b64, 0xb9ecf4 },
  { TONAL_INVERSE_PRIMARY, 0xaef4fe, 0x34666d },
  { TONAL_SECONDARY, 0x4b6367, 0xb1cbcf },
  { TONAL_ON_SECONDARY, 0xebfcff, 0x2c4448 },
  { TONAL_SECONDARY_CONTAINER, 0xcde7ec, 0x273f43 },
  { TONAL_ON_SECONDARY_CONTAINER, 0x3d5659, 0xaac4c8 },
  { TONAL_TERTIARY, 0x416281, 0xc9e2ff },
  { TONAL_ON_TERTIARY, 0xf6f9ff, 0x315371 },
  { TONAL_TERTIARY_CONTAINER, 0xb3d5f9, 0xb3d5f9 },
  { TONAL_ON_TERTIARY_CONTAINER, 0x284a67, 0x284a67 },
  { TONAL_ERROR, 0xa83836, 0xfa746f },
  { TONAL_ON_ERROR, 0xfff7f6, 0x490006 },
  { TONAL_ERROR_CONTAINER, 0xfa746f, 0x871f21 },
  { TONAL_ON_ERROR_CONTAINER, 0x6e0a12, 0xff9993 },
  { TONAL_BACKGROUND, 0xf6fafb, 0x0a0f10 },
  { TONAL_SURFACE, 0xf6fafb, 0x0a0f10 },
  { TONAL_ON_SURFACE, 0x2a3436, 0xdde7e9 },
  { TONAL_SURFACE_VARIANT, 0xdae4e6, 0x1e2729 },
  { TONAL_ON_SURFACE_VARIANT, 0x576163, 0xa2adaf },
  { TONAL_OUTLINE, 0x727d7e, 0x6d7779 },
  { TONAL_OUTLINE_VARIANT, 0xa9b4b6, 0x404a4b },
  { TONAL_INVERSE_SURFACE, 0x0a0f10, 0xf6fafb },
  { TONAL_INVERSE_ON_SURFACE, 0x999e9e, 0x515657 },
  { TONAL_SURFACE_DIM, 0xd1dcde, 0x0a0f10 },
  { TONAL_SURFACE_BRIGHT, 0xf6fafb, 0x242e2f },
  { TONAL_SURFACE_CONTAINER_LOWEST, 0xffffff, 0x000000 },
  { TONAL_SURFACE_CONTAINER_LOW, 0xeef5f6, 0x0e1415 },
  { TONAL_SURFACE_CONTAINER, 0xe8eff0, 0x131b1c },
  { TONAL_SURFACE_CONTAINER_HIGH, 0xe1eaeb, 0x192122 },
  { TONAL_SURFACE_CONTAINER_HIGHEST, 0xdae4e6, 0x1e2729 },
};

/* the phone's palettes in dark mode, all 13 stops */
static const struct {
  TonalPaletteId palette;
  guint32        stops[TONAL_N_STOPS];
} phone_dark_stops[] = {
  { TONAL_PALETTE_PRIMARY, { 0xffffff, 0xf6feff, 0xcef8ff, 0xb8ebf3, 0x9ccfd7, 0x81b4bb, 0x6799a0, 0x4d7f86, 0x33666d,
                             0x174e55, 0x00363c, 0x001f23, 0x000000 } },
  { TONAL_PALETTE_NEUTRAL, { 0xffffff, 0xf9fdfd, 0xedf1f2, 0xdfe3e4, 0xc3c7c8, 0xa8acad, 0x8d9192, 0x737879, 0x5a5f60,
                             0x434848, 0x2c3132, 0x181c1d, 0x000000 } },
  { TONAL_PALETTE_NEUTRAL_VARIANT, { 0xffffff, 0xf6feff, 0xe8f3f4, 0xdae4e6, 0xbec8ca, 0xa2adaf, 0x889394, 0x6e797b,
                                     0x566062, 0x3e494a, 0x283234, 0x131d1f, 0x000000 } },
};

static void
test_phone_roles (void)
{
  check_roles (PHONE_SEED, phone_roles, G_N_ELEMENTS (phone_roles), TRUE, TRUE);
}

static void
test_phone_palettes (void)
{
  TonalScheme dark;
  int failures = 0;

  tonal_scheme_init (&dark, PHONE_SEED, TONAL_VARIANT_TONAL_SPOT, TRUE, 0.0, TONAL_SPEC_2025);
  for (gsize p = 0; p < G_N_ELEMENTS (phone_dark_stops); p++)
    for (int i = 0; i < TONAL_N_STOPS; i++)
      {
        g_autofree char *what = g_strdup_printf ("dark %s_%d", tonal_palette_name (phone_dark_stops[p].palette),
                                                 tonal_stops[i]);

        check (what, tonal_scheme_stop (&dark, phone_dark_stops[p].palette, tonal_stops[i]),
               phone_dark_stops[p].stops[i], &failures);
      }
  g_assert_cmpint (failures, ==, 0);
}

/* ---- every style, both rule sets, light and dark, seven contrast levels: digests of the colours the reference
 * implementation gives for nine seeds (yellow, a disliked olive, cyan, blue, black among them) */

static guint64
fnv (guint64 hash, guint32 value)
{
  for (int i = 0; i < 4; i++)
    {
      hash ^= (value >> (8 * i)) & 255;
      hash *= G_GUINT64_CONSTANT (0x100000001b3);
    }
  return hash;
}

static const guint32 digest_seeds[] = {
  0x24222D, 0x007C88, 0x4285F4, 0xFFFF00, 0xB3A000, 0x00BCD4, 0x3F51B5, 0x000000, 0xFF0000,
};
static const double digest_contrasts[] = { -1, -0.5, 0, 0.25, 0.5, 0.75, 1 };
static const double digest_tones[] = { 100, 99, 95, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0 };

/* by rule set (2021, 2025), then by style in TonalVariant order */
static const guint64 reference_digests[2][TONAL_N_VARIANTS] = {
  { G_GUINT64_CONSTANT (0xdfd555f846730547), G_GUINT64_CONSTANT (0x1c4eeb956363398a),
    G_GUINT64_CONSTANT (0x09d409b3075dcfe6), G_GUINT64_CONSTANT (0x1125daeb5be475cb),
    G_GUINT64_CONSTANT (0x1ab425bb6bfd6c40), G_GUINT64_CONSTANT (0xae2046180ecf3fc3),
    G_GUINT64_CONSTANT (0x73f57d7ede90bfcc), G_GUINT64_CONSTANT (0xf43bba726fdbec37),
    G_GUINT64_CONSTANT (0x062dba1a9a453ff6) },
  { G_GUINT64_CONSTANT (0xdfd555f846730547), G_GUINT64_CONSTANT (0x29e19dfed5651457),
    G_GUINT64_CONSTANT (0xf28461910ae8ae2e), G_GUINT64_CONSTANT (0x28ff2de2be541199),
    G_GUINT64_CONSTANT (0x350a58660cfd8d9c), G_GUINT64_CONSTANT (0xae2046180ecf3fc3),
    G_GUINT64_CONSTANT (0x73f57d7ede90bfcc), G_GUINT64_CONSTANT (0xf43bba726fdbec37),
    G_GUINT64_CONSTANT (0x062dba1a9a453ff6) },
};

static void
test_reference_digests (void)
{
  int failures = 0;

  for (int spec = 0; spec < 2; spec++)
    for (int v = 0; v < TONAL_N_VARIANTS; v++)
      {
        guint64 hash = G_GUINT64_CONSTANT (0xcbf29ce484222325);

        for (gsize k = 0; k < G_N_ELEMENTS (digest_seeds); k++)
          for (int dark = 0; dark < 2; dark++)
            for (gsize c = 0; c < G_N_ELEMENTS (digest_contrasts); c++)
              {
                TonalScheme scheme;

                tonal_scheme_init (&scheme, digest_seeds[k], v, dark, digest_contrasts[c],
                                   spec ? TONAL_SPEC_2025 : TONAL_SPEC_2021);
                for (int r = 0; r < TONAL_N_ROLES; r++)
                  hash = fnv (hash, tonal_scheme_color (&scheme, r) & 0xffffff);
                for (int p = 0; p < TONAL_N_PALETTES; p++)
                  for (gsize t = 0; t < G_N_ELEMENTS (digest_tones); t++)
                    hash = fnv (hash, tonal_palette_tone (&scheme.palettes[p], digest_tones[t]) & 0xffffff);
              }
        if (hash != reference_digests[spec][v])
          {
            g_printerr ("%s, %s rules: digest %016" G_GINT64_MODIFIER "x, want %016" G_GINT64_MODIFIER "x\n",
                        tonal_variant_name (v), spec ? "2025" : "2021", hash, reference_digests[spec][v]);
            failures++;
          }
      }
  g_assert_cmpint (failures, ==, 0);
}

/* ---- seeds from images */

#include "synth.h"

typedef struct {
  const char *name;
  guint32     seed;
  int         n_pixels, noise;
  const Blob *blobs;
  int         n_blobs;
  guint       n_colors;        /* what the reference's quantizer gives: this many colours, with this digest */
  guint64     digest;
  guint32     scored[4];       /* and what its scoring picks from them */
} ImageCase;

static const ImageCase image_cases[] = {
  { "blue-orange", 1, 112 * 112, 12, blue_orange, 2, 70, G_GUINT64_CONSTANT (0x46a88b9f71347445),
    { 0x224FD2, 0xF07017 } },
  { "grey", 2, 112 * 112, 2, grey_only, 1, 1, G_GUINT64_CONSTANT (0x1b54bd6a775d5caa), { 0x4285F4 } },
  { "three", 3, 300 * 300, 20, three, 3, 126, G_GUINT64_CONSTANT (0x9ff2a8150d6e5e91),
    { 0x159E2F, 0xFEEC1A, 0x690EB0, 0xFECB2C } },
  { "mostly-grey", 4, 112 * 112, 6, mostly_grey, 2, 21, G_GUINT64_CONSTANT (0xfeba1bc250427f1c),
    { 0x757480, 0xCB1C3B, 0x757C7A, 0x757B7F } },
};

static void
test_quantize_and_score (void)
{
  for (gsize i = 0; i < G_N_ELEMENTS (image_cases); i++)
    {
      const ImageCase *c = &image_cases[i];
      g_autofree guint32 *pixels = synth_image (c->seed, c->n_pixels, c->blobs, c->n_blobs, c->noise);
      g_autoptr (GArray) colors = tonal_quantize (pixels, c->n_pixels, 128);
      g_autoptr (GArray) scored = NULL;
      guint64 hash = G_GUINT64_CONSTANT (0xcbf29ce484222325);
      guint n_scored = 0;

      for (guint j = 0; j < colors->len; j++)
        {
          const TonalColorCount *e = &g_array_index (colors, TonalColorCount, j);

          hash = fnv (fnv (hash, e->argb & 0xffffff), e->count);
        }
      g_test_message ("%s: %u colours", c->name, colors->len);
      g_assert_cmpuint (colors->len, ==, c->n_colors);
      g_assert_cmphex (hash, ==, c->digest);

      scored = tonal_score ((const TonalColorCount *) colors->data, colors->len, 4, 0xff4285f4, TRUE);
      while (n_scored < 4 && c->scored[n_scored])
        n_scored++;
      g_assert_cmpuint (scored->len, ==, n_scored);
      for (guint j = 0; j < scored->len; j++)
        g_assert_cmphex (g_array_index (scored, guint32, j) & 0xffffff, ==, c->scored[j]);
    }
}

/* a large wallpaper: seven tenths blue, the rest orange; the blue is the seed, the orange the second choice */
static void
test_seeds_from_image (void)
{
  const int width = 1080, height = 2340;
  g_autofree guint32 *pixels = synth_image (5, width * height, blue_orange, 2, 12);
  g_autoptr (GArray) seeds = tonal_seeds_from_image (pixels, width, height, width);
  TonalHct blue = tonal_hct_from_argb (0xff285ac8), orange = tonal_hct_from_argb (0xffe6781e);
  TonalHct first, second;

  g_assert_cmpuint (seeds->len, >=, 2);
  first = tonal_hct_from_argb (g_array_index (seeds, guint32, 0));
  second = tonal_hct_from_argb (g_array_index (seeds, guint32, 1));
  g_assert_cmpfloat (fabs (first.hue - blue.hue), <, 10);
  g_assert_cmpfloat (fabs (second.hue - orange.hue), <, 10);

  /* a transparent image has no colours: the fallback blue */
  memset (pixels, 0, sizeof (guint32) * width * 10);
  g_clear_pointer (&seeds, g_array_unref);
  seeds = tonal_seeds_from_image (pixels, width, 10, width);
  g_assert_cmpuint (seeds->len, ==, 1);
  g_assert_cmphex (g_array_index (seeds, guint32, 0), ==, 0xff4285f4);
}

/* a theme: the pure-black option darkens only the dark scheme's backgrounds */
static void
test_theme (void)
{
  TonalTheme theme, black;

  tonal_theme_compute (&theme, PHONE_SEED, TONAL_VARIANT_TONAL_SPOT, 0, FALSE);
  tonal_theme_compute (&black, PHONE_SEED, TONAL_VARIANT_TONAL_SPOT, 0, TRUE);
  g_assert_cmphex (theme.dark[TONAL_SURFACE], ==, 0xff0a0f10);
  g_assert_cmphex (black.dark[TONAL_SURFACE], ==, 0xff000000);
  g_assert_cmphex (black.dark[TONAL_SURFACE_CONTAINER], ==, theme.dark[TONAL_SURFACE_CONTAINER]);
  g_assert_cmpmem (black.light, sizeof black.light, theme.light, sizeof theme.light);
  /* accent1_200 in each mode, as the phone's keyboard draws its enter key: lighter-toned in light mode, from the
   * more colourful palette */
  g_assert_cmphex (theme.light_stops[TONAL_PALETTE_PRIMARY][4], ==, 0xff8dd2dc);
  g_assert_cmphex (theme.dark_stops[TONAL_PALETTE_PRIMARY][4], ==, 0xff9ccfd7);
}

static void
test_names (void)
{
  for (int i = 0; i < TONAL_N_ROLES; i++)
    {
      TonalRole role;

      g_assert_true (tonal_role_from_name (tonal_role_name (i), &role));
      g_assert_cmpint (role, ==, i);
    }
  for (int i = 0; i < TONAL_N_VARIANTS; i++)
    {
      TonalVariant variant;

      g_assert_true (tonal_variant_from_name (tonal_variant_name (i), &variant));
      g_assert_cmpint (variant, ==, i);
    }
  g_assert_cmpstr (tonal_palette_name (TONAL_PALETTE_NEUTRAL_VARIANT), ==, "neutral2");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/tonal/tablet/roles", test_tablet_roles);
  g_test_add_func ("/tonal/tablet/palettes", test_tablet_palettes);
  g_test_add_func ("/tonal/phone/roles", test_phone_roles);
  g_test_add_func ("/tonal/phone/palettes", test_phone_palettes);
  g_test_add_func ("/tonal/reference", test_reference_digests);
  g_test_add_func ("/tonal/image/quantize-and-score", test_quantize_and_score);
  g_test_add_func ("/tonal/image/seeds", test_seeds_from_image);
  g_test_add_func ("/tonal/theme", test_theme);
  g_test_add_func ("/tonal/names", test_names);
  return g_test_run ();
}
