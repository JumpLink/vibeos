/* The theme's colours by the original rules (vibeos-original-colors.h) without a display: the default seed must give
 * the conversation colours the reference phone's messaging app draws, in dark and in light, and follow the mode as
 * it changes. Run by meson with the settings in memory (GSETTINGS_BACKEND=memory). */
#include "vibeos.h"

#include <stdlib.h>

static guint32
rgb (GdkRGBA c)
{
  return (guint32) (c.red * 255 + 0.5) << 16 | (guint32) (c.green * 255 + 0.5) << 8 | (guint32) (c.blue * 255 + 0.5);
}

/* within one step per channel of what was measured on the reference phone's screen */
static void
assert_near (VibeosColor color, guint32 expected)
{
  guint32 got = rgb (vibeos_original_color (color));

  for (int shift = 0; shift <= 16; shift += 8)
    if (abs ((int) ((got >> shift) & 0xff) - (int) ((expected >> shift) & 0xff)) > 1)
      g_error ("colour %d: #%06x, expected #%06x", color, got, expected);
}

static void
settle (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_modes (void)
{
  g_autoptr (GSettings) interface = g_settings_new ("org.gnome.desktop.interface");

  g_settings_set_string (interface, "color-scheme", "prefer-dark");
  settle ();
  g_assert_true (vibeos_theme_get_dark (vibeos_theme_get_default ()));
  assert_near (VIBEOS_SURFACE, 0x0f1415);
  assert_near (VIBEOS_SURFACE_CONTAINER, 0x1c2021);
  assert_near (VIBEOS_SURFACE_CONTAINER_HIGH, 0x262b2c);
  assert_near (VIBEOS_ON_SURFACE, 0xdfe3e4);
  assert_near (VIBEOS_ON_SURFACE_VARIANT, 0xbec8ca);
  assert_near (VIBEOS_TERTIARY_CONTAINER, 0x274967);
  assert_near (VIBEOS_ON_TERTIARY_CONTAINER, 0xcee5ff);
  assert_near (VIBEOS_SECONDARY_CONTAINER, 0x324b4e);
  assert_near (VIBEOS_ON_SECONDARY_CONTAINER, 0xcde7ec);
  /* a fixed colour is the theme's */
  g_assert_cmphex (rgb (vibeos_original_color (VIBEOS_CALL)), ==, rgb (vibeos_color (VIBEOS_CALL)));

  g_settings_set_string (interface, "color-scheme", "default");
  settle ();
  g_assert_false (vibeos_theme_get_dark (vibeos_theme_get_default ()));
  assert_near (VIBEOS_SURFACE, 0xf5fafc);
  assert_near (VIBEOS_SURFACE_CONTAINER, 0xe9eff0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/original-colors/modes", test_modes);
  return g_test_run ();
}
