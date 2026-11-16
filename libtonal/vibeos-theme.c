/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 pulpoff <pashkovsky@gmail.com>
 *
 * vibeos-theme: computes the system's colours from a seed colour or a wallpaper, prints them, and stores them in
 * vibeos.Theme, where every VibeOS app picks them up. */
#include "tonal.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>

static char *opt_seed, *opt_wallpaper, *opt_style;
static double opt_contrast = NAN;
static gboolean opt_pure_black, opt_no_pure_black, opt_print, opt_apply;

static const GOptionEntry entries[] = {
  { "seed", 0, 0, G_OPTION_ARG_STRING, &opt_seed, "The seed colour", "#RRGGBB" },
  { "wallpaper", 0, 0, G_OPTION_ARG_FILENAME, &opt_wallpaper, "Take the seed from an image", "FILE" },
  { "style", 0, 0, G_OPTION_ARG_STRING, &opt_style, "tonal-spot (the default), neutral, vibrant, expressive, "
    "fidelity, content, monochrome, rainbow or fruit-salad", "STYLE" },
  { "contrast", 0, 0, G_OPTION_ARG_DOUBLE, &opt_contrast, "From -1 to 1: 0 standard, 0.5 medium, 1 high", "LEVEL" },
  { "pure-black", 0, 0, G_OPTION_ARG_NONE, &opt_pure_black, "Black backgrounds in dark mode", NULL },
  { "no-pure-black", 0, 0, G_OPTION_ARG_NONE, &opt_no_pure_black, "The scheme's own dark backgrounds", NULL },
  { "print", 0, 0, G_OPTION_ARG_NONE, &opt_print, "Print the colours (the default without --apply)", NULL },
  { "apply", 0, 0, G_OPTION_ARG_NONE, &opt_apply, "Store the theme in vibeos.Theme", NULL },
  { NULL }
};

static gboolean
parse_color (const char *text, guint32 *argb)
{
  char *end;
  guint64 value;

  if (*text == '#')
    text++;
  else if (g_str_has_prefix (text, "0x"))
    text += 2;
  if (strlen (text) != 6)
    return FALSE;
  value = g_ascii_strtoull (text, &end, 16);
  if (*end != '\0')
    return FALSE;
  *argb = 0xff000000u | (guint32) value;
  return TRUE;
}

/* the seeds of an image, best first */
static GArray *
seeds_from_file (const char *path, GError **error)
{
  g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file (path, error);
  g_autofree guint32 *pixels = NULL;
  int width, height, stride, channels;
  const guchar *data;

  if (!pixbuf)
    return NULL;
  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  stride = gdk_pixbuf_get_rowstride (pixbuf);
  channels = gdk_pixbuf_get_n_channels (pixbuf);
  data = gdk_pixbuf_read_pixels (pixbuf);
  pixels = g_new (guint32, (gsize) width * height);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        const guchar *p = data + (gsize) y * stride + (gsize) x * channels;
        guint32 alpha = gdk_pixbuf_get_has_alpha (pixbuf) ? p[3] : 255;

        pixels[(gsize) y * width + x] = alpha << 24 | (guint32) p[0] << 16 | (guint32) p[1] << 8 | p[2];
      }
  return tonal_seeds_from_image (pixels, width, height, width);
}

static void
print_theme (const TonalTheme *theme, guint32 seed, TonalVariant variant, double contrast, gboolean pure_black)
{
  g_print ("seed #%06X, %s, contrast %g%s\n\n", seed & 0xffffff, tonal_variant_name (variant), contrast,
           pure_black ? ", pure black" : "");
  g_print ("%-34s %-8s %s\n", "role", "light", "dark");
  for (int i = 0; i < TONAL_N_ROLES; i++)
    g_print ("%-34s #%06X  #%06X\n", tonal_role_name (i), theme->light[i] & 0xffffff, theme->dark[i] & 0xffffff);
  for (int mode = 0; mode < 2; mode++)
    {
      g_print ("\n%s palettes, stops", mode ? "dark" : "light");
      for (int i = 0; i < TONAL_N_STOPS; i++)
        g_print (" %d", tonal_stops[i]);
      g_print ("\n");
      for (int p = 0; p < TONAL_N_PALETTES; p++)
        {
          g_print ("%-9s", tonal_palette_name (p));
          for (int i = 0; i < TONAL_N_STOPS; i++)
            g_print (" %06X", (mode ? theme->dark_stops : theme->light_stops)[p][i] & 0xffffff);
          g_print ("\n");
        }
    }
}

static GVariant *
roles_variant (const guint32 *roles)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{su}"));
  for (int i = 0; i < TONAL_N_ROLES; i++)
    g_variant_builder_add (&builder, "{su}", tonal_role_name (i), roles[i]);
  return g_variant_builder_end (&builder);
}

static GVariant *
palettes_variant (guint32 (*stops)[TONAL_N_STOPS])
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sau}"));
  for (int p = 0; p < TONAL_N_PALETTES; p++)
    g_variant_builder_add (&builder, "{s@au}", tonal_palette_name (p),
                           g_variant_new_fixed_array (G_VARIANT_TYPE_UINT32, stops[p], TONAL_N_STOPS,
                                                      sizeof (guint32)));
  return g_variant_builder_end (&builder);
}

int
main (int argc, char **argv)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("- the system's colours from a seed or a wallpaper");
  g_autoptr (GError) error = NULL;
  g_autoptr (GSettings) settings = NULL;
  GSettingsSchema *schema;
  guint32 seed = 0xff007c88;
  TonalVariant variant = TONAL_VARIANT_TONAL_SPOT;
  double contrast = 0;
  gboolean pure_black = FALSE, have_seed = FALSE;
  TonalTheme theme;

  setlocale (LC_ALL, "");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_description (context,
    "Without --seed or --wallpaper, the seed is vibeos.Theme's. --apply stores the seed, the style, the contrast\n"
    "and the computed colours in vibeos.Theme; the apps follow it as it changes.");
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("vibeos-theme: %s\n", error->message);
      return 2;
    }

  schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), "vibeos.Theme", TRUE);
  if (schema)
    {
      g_autofree char *style = NULL;

      settings = g_settings_new_full (schema, NULL, NULL);
      g_settings_schema_unref (schema);
      seed = 0xff000000u | g_settings_get_uint (settings, "seed");
      style = g_settings_get_string (settings, "style");
      tonal_variant_from_name (style, &variant);
      contrast = g_settings_get_double (settings, "contrast");
      pure_black = g_settings_get_boolean (settings, "pure-black");
    }
  else if (opt_apply)
    {
      g_printerr ("vibeos-theme: the vibeos.Theme settings schema is not installed\n");
      return 1;
    }

  if (opt_seed && opt_wallpaper)
    {
      g_printerr ("vibeos-theme: --seed and --wallpaper are each a seed: give one\n");
      return 2;
    }
  if (opt_seed)
    {
      if (!parse_color (opt_seed, &seed))
        {
          g_printerr ("vibeos-theme: \"%s\" is not a colour (#RRGGBB)\n", opt_seed);
          return 2;
        }
      have_seed = TRUE;
    }
  if (opt_wallpaper)
    {
      g_autoptr (GArray) seeds = seeds_from_file (opt_wallpaper, &error);

      if (!seeds)
        {
          g_printerr ("vibeos-theme: %s\n", error->message);
          return 1;
        }
      seed = g_array_index (seeds, guint32, 0);
      have_seed = TRUE;
      if (opt_print || !opt_apply)
        {
          g_print ("seeds of %s, best first:", opt_wallpaper);
          for (guint i = 0; i < seeds->len; i++)
            g_print (" #%06X", g_array_index (seeds, guint32, i) & 0xffffff);
          g_print ("\n");
        }
    }
  if (opt_style && !tonal_variant_from_name (opt_style, &variant))
    {
      g_printerr ("vibeos-theme: no style \"%s\"\n", opt_style);
      return 2;
    }
  if (!isnan (opt_contrast))
    contrast = CLAMP (opt_contrast, -1.0, 1.0);
  if (opt_pure_black || opt_no_pure_black)
    pure_black = opt_pure_black;

  tonal_theme_compute (&theme, seed, variant, contrast, pure_black);
  if (opt_print || !opt_apply)
    print_theme (&theme, seed, variant, contrast, pure_black);

  if (opt_apply)
    {
      g_settings_delay (settings);
      if (have_seed)
        {
          g_settings_set_string (settings, "source", opt_wallpaper ? "wallpaper" : "preset");
          g_settings_set_uint (settings, "seed", seed & 0xffffff);
        }
      g_settings_set_string (settings, "style", tonal_variant_name (variant));
      g_settings_set_double (settings, "contrast", contrast);
      g_settings_set_boolean (settings, "pure-black", pure_black);
      g_settings_set_value (settings, "light-roles", roles_variant (theme.light));
      g_settings_set_value (settings, "dark-roles", roles_variant (theme.dark));
      g_settings_set_value (settings, "light-palettes", palettes_variant (theme.light_stops));
      g_settings_set_value (settings, "dark-palettes", palettes_variant (theme.dark_stops));
      g_settings_apply (settings);
      g_settings_sync ();
    }
  return 0;
}
