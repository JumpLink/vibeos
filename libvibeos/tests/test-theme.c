/* The theme's colours without a display: what vibeos_color () gives for vibeos.Theme's settings, light and dark,
 * as they change, and without the schema. Run by meson with the settings in memory (GSETTINGS_BACKEND=memory). */
#include "vibeos.h"

/* the dark table the apps had before the theme: vibeos.Theme's default seed must give it to the byte */
static const guint32 old_dark[] = {
  [VIBEOS_PRIMARY] = 0x9ccfd7, [VIBEOS_ON_PRIMARY] = 0x0d474e, [VIBEOS_PRIMARY_CONTAINER] = 0x265a61,
  [VIBEOS_ON_PRIMARY_CONTAINER] = 0xb9ecf4, [VIBEOS_INVERSE_PRIMARY] = 0x34666d, [VIBEOS_SECONDARY] = 0xb1cbcf,
  [VIBEOS_ON_SECONDARY] = 0x2c4448, [VIBEOS_SECONDARY_CONTAINER] = 0x273f43, [VIBEOS_ON_SECONDARY_CONTAINER] = 0xaac4c8,
  [VIBEOS_TERTIARY] = 0xc9e2ff, [VIBEOS_ON_TERTIARY] = 0x315371, [VIBEOS_TERTIARY_CONTAINER] = 0xb3d5f9,
  [VIBEOS_ON_TERTIARY_CONTAINER] = 0x284a67, [VIBEOS_ERROR] = 0xfa746f, [VIBEOS_ERROR_CONTAINER] = 0x871f21,
  [VIBEOS_SURFACE] = 0x0a0f10, [VIBEOS_SURFACE_CONTAINER_LOWEST] = 0x000000,
  [VIBEOS_SURFACE_CONTAINER_LOW] = 0x0e1415, [VIBEOS_SURFACE_CONTAINER] = 0x131b1c,
  [VIBEOS_SURFACE_CONTAINER_HIGH] = 0x192122, [VIBEOS_SURFACE_CONTAINER_HIGHEST] = 0x1e2729,
  [VIBEOS_SURFACE_BRIGHT] = 0x242e2f, [VIBEOS_ON_SURFACE] = 0xdde7e9, [VIBEOS_ON_SURFACE_VARIANT] = 0xa2adaf,
  [VIBEOS_OUTLINE] = 0x6d7779, [VIBEOS_OUTLINE_VARIANT] = 0x404a4b, [VIBEOS_INVERSE_SURFACE] = 0xf6fafb,
  [VIBEOS_WHITE] = 0xffffff,
};

static guint32
rgb (VibeosColor color)
{
  GdkRGBA c = vibeos_color (color);

  return (guint32) (c.red * 255 + 0.5) << 16 | (guint32) (c.green * 255 + 0.5) << 8 | (guint32) (c.blue * 255 + 0.5);
}

static void
assert_old_dark (void)
{
  for (gsize i = 0; i < G_N_ELEMENTS (old_dark); i++)
    g_assert_cmphex (rgb (i), ==, old_dark[i]);
}

static void
on_changed (VibeosTheme *theme, int *count)
{
  (*count)++;
}

/* the settings' change notifications arrive through the main context */
static void
settle (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_no_schema (void)
{
  VibeosTheme *theme = vibeos_theme_get_default ();

  g_assert_true (vibeos_theme_get_dark (theme));
  g_assert_cmphex (vibeos_theme_get_seed (theme), ==, 0x007c88);
  assert_old_dark ();
  g_assert_cmphex (rgb (VIBEOS_SCRIM), ==, 0x000000);
}

static void
test_settings (void)
{
  g_autoptr (GSettings) interface = g_settings_new ("org.gnome.desktop.interface");
  g_autoptr (GSettings) settings = g_settings_new ("vibeos.Theme");
  VibeosTheme *theme;
  int changes = 0;
  GdkRGBA stop;

  g_settings_set_string (interface, "color-scheme", "prefer-dark");
  theme = vibeos_theme_get_default ();
  g_signal_connect (theme, "changed", G_CALLBACK (on_changed), &changes);
  g_assert_true (vibeos_theme_get_dark (theme));
  assert_old_dark ();

  /* light: the phone's light scheme */
  g_settings_set_string (interface, "color-scheme", "default");
  settle ();
  g_assert_cmpint (changes, ==, 1);
  g_assert_false (vibeos_theme_get_dark (theme));
  g_assert_cmphex (rgb (VIBEOS_PRIMARY), ==, 0x1c6872);
  g_assert_cmphex (rgb (VIBEOS_SURFACE), ==, 0xf6fafb);
  g_assert_cmphex (rgb (VIBEOS_ON_SURFACE), ==, 0x2a3436);
  g_assert_cmphex (rgb (VIBEOS_SURFACE_DIM), ==, 0xd1dcde);
  g_assert_cmphex (rgb (VIBEOS_WHITE), ==, 0xffffff);
  g_assert_cmphex (rgb (VIBEOS_DIALOG_TITLE), ==, 0x1e1e1f);
  stop = vibeos_palette_stop (VIBEOS_PALETTE_ACCENT1, 200);
  g_assert_cmpfloat_with_epsilon (stop.red * 255, 0x8d, 0.01);

  /* another seed: the tablet's, in both modes */
  g_settings_set_uint (settings, "seed", 0x24222d);
  settle ();
  g_assert_cmpint (changes, ==, 2);
  g_assert_cmphex (vibeos_theme_get_seed (theme), ==, 0x24222d);
  g_assert_cmphex (rgb (VIBEOS_SURFACE_DIM), ==, 0xddd8e4);
  g_assert_cmphex (rgb (VIBEOS_PRIMARY), ==, 0x60598a);
  g_settings_set_string (interface, "color-scheme", "prefer-dark");
  settle ();
  g_assert_cmpint (changes, ==, 3);
  g_assert_cmphex (rgb (VIBEOS_SURFACE), ==, 0x0e0e12);
  g_assert_cmphex (rgb (VIBEOS_SURFACE_BRIGHT), ==, 0x2d2b34);
  g_assert_cmphex (rgb (VIBEOS_DIALOG_TITLE), ==, 0xffffff);

  /* pure black: the dark backgrounds only */
  g_settings_set_boolean (settings, "pure-black", TRUE);
  settle ();
  g_assert_cmphex (rgb (VIBEOS_SURFACE), ==, 0x000000);
  g_assert_cmphex (rgb (VIBEOS_SURFACE_CONTAINER), ==, 0x1a191f);

  /* a key that changes nothing the colours come from: no signal */
  changes = 0;
  g_settings_set_string (settings, "source", "preset");
  settle ();
  g_assert_cmpint (changes, ==, 0);
}

/* VIBEOS_COLOR_SCHEME=light and VIBEOS_SEED=#24222D over the settings */
static void
test_environment (void)
{
  g_autoptr (GSettings) interface = g_settings_new ("org.gnome.desktop.interface");
  VibeosTheme *theme;

  g_settings_set_string (interface, "color-scheme", "prefer-dark");
  theme = vibeos_theme_get_default ();
  g_assert_false (vibeos_theme_get_dark (theme));
  g_assert_cmphex (rgb (VIBEOS_SURFACE), ==, 0xfdf8fe);
  g_assert_cmphex (rgb (VIBEOS_ON_SECONDARY_CONTAINER), ==, 0x534f64);
}

int
main (int argc, char **argv)
{
  const char *which = argc > 1 ? argv[1] : "";

  g_test_init (&argc, &argv, NULL);
  if (g_str_equal (which, "no-schema"))
    g_test_add_func ("/theme/no-schema", test_no_schema);
  else if (g_str_equal (which, "settings"))
    g_test_add_func ("/theme/settings", test_settings);
  else if (g_str_equal (which, "environment"))
    g_test_add_func ("/theme/environment", test_environment);
  return g_test_run ();
}
