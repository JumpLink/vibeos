/* Font size and Bold text without a display: the font-size curve, the stylesheet conversion, and what an app that
 * follows the settings gets as they change (GSETTINGS_BACKEND=memory), with the environment's overrides and without
 * the schemas. */
#include "vibeos.h"

/* VIBEOS_INIT_SCALED_TEXT without vibeos_init_full, which needs a display (vibeos-text.c) */
extern void _vibeos_text_set_following (gboolean following);

#define assert_px(sp, scale, px) g_assert_cmpfloat_with_epsilon (vibeos_sp_at_scale ((sp), (scale)), (px), 1e-4)

static const double sizes[] = { 8, 10, 12, 14, 18, 20, 24, 30, 100 };

/* the curve at the font sizes Settings offers, size by size */
static const struct {
  double scale;
  double px[G_N_ELEMENTS (sizes)];
} rows[] = {
  { 1.15, { 9.2, 11.5, 13.8, 16.4, 19.8, 21.8, 25.2, 30, 100 } },
  { 1.3, { 10.4, 13, 15.6, 18.8, 21.6, 23.6, 26.4, 30, 100 } },
  { 1.5, { 12, 15, 18, 22, 24, 26, 28, 30, 100 } },
  { 1.8, { 14.4, 18, 21.6, 24.4, 27.6, 30.8, 32.8, 34.8, 100 } },
  { 2.0, { 16, 20, 24, 26, 30, 34, 36, 38, 100 } },
};

static void
test_curve (void)
{
  for (guint r = 0; r < G_N_ELEMENTS (rows); r++)
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
      assert_px (sizes[i], rows[r].scale, rows[r].px[i]);

  /* 1 and under: plain multiplication, large text included */
  for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      g_assert_cmpfloat (vibeos_sp_at_scale (sizes[i], 1.0), ==, sizes[i]);
      assert_px (sizes[i], 0.85, sizes[i] * 0.85);
    }
  assert_px (125, 0.85, 106.25);
  assert_px (57, 1.0, 57);
  /* up to the curve's start, and between 1 and its first font size: plain multiplication */
  assert_px (14, 1.03, 14.42);
  assert_px (14, 1.045, 14.63);
  /* between the sizes of a curve: interpolated */
  assert_px (16, 1.3, 20.2);
  assert_px (22, 2.0, 35);
  assert_px (57, 2.0, 38 + (57 - 30) / 70.0 * 62);
  /* under its smallest size: from 0 */
  assert_px (4, 2.0, 8);
  /* 100 sp and more do not grow */
  assert_px (125, 1.3, 125);
  assert_px (125, 2.0, 125);
  /* between two curves: each size between the two */
  assert_px (14, 1.25, (17.2 + 18.8) / 2);
  assert_px (125, 1.25, 125);
  /* over the last curve: plain multiplication again */
  assert_px (14, 2.5, 35);
  assert_px (100, 2.5, 250);
  /* signs are kept */
  assert_px (-14, 1.3, -18.8);
  assert_px (0, 1.3, 0);
}

static void
assert_css (const char *css, double scale, gboolean bold, const char *expected)
{
  g_autofree char *converted = vibeos_css_convert (css, scale, bold);

  g_assert_cmpstr (converted, ==, expected);
}

static void
test_css (void)
{
  /* what an app that does not follow the settings gets: px at 1, weights 300 heavier */
  assert_css (".a { font-size: 14sp; min-height: 64dp; font-weight: 400; }", 1, TRUE,
              ".a { font-size: 14px; min-height: 64px; font-weight: 700; }");
  assert_css (".a { font-size: 14sp; font-weight: 500; }\n.b:hover { font-weight: 600 }", 1.3, FALSE,
              ".a { font-size: 18.8px; font-weight: 500; }\n.b:hover { font-weight: 600 }");
  assert_css ("label { font-size: 16sp; padding: 0 4dp 2.5dp -4dp; }", 2, TRUE,
              "label { font-size: 28px; padding: 0 4px 2.5px -4px; }");
  /* weights: keywords too, and never over 1000 */
  assert_css ("a{font-weight:normal}b{font-weight: bold;}c{font-weight: 900 !important}", 1, TRUE,
              "a{font-weight:700}b{font-weight: 1000;}c{font-weight: 1000 !important}");
  assert_css ("a { font-weight: bolder; opacity: 400; }", 1, TRUE, "a { font-weight: bolder; opacity: 400; }");
  /* only sp and dp: px, em, percentages and plain numbers stay as they are */
  assert_css ("a { font-size: 14px; margin: 1.5em 50%; -gtk-icon-size: 24dp; }", 1.3, TRUE,
              "a { font-size: 14px; margin: 1.5em 50%; -gtk-icon-size: 24px; }");
  /* nothing in a comment, a string, a URL, a name or a colour */
  assert_css ("/* 14sp, font-weight: 400 */ .icon-24dp { background: url(icons/24dp.svg); content: \"14sp\"; "
              "color: #12dd00; }",
              1.3, TRUE,
              "/* 14sp, font-weight: 400 */ .icon-24dp { background: url(icons/24dp.svg); content: \"14sp\"; "
              "color: #12dd00; }");
  assert_css ("@define-color vibeos_primary #9ccfd7;\n.a { color: @vibeos_primary; }", 1.3, TRUE,
              "@define-color vibeos_primary #9ccfd7;\n.a { color: @vibeos_primary; }");
  /* nested blocks */
  assert_css ("@keyframes k { from { min-width: 0dp; } to { min-width: 10dp; font-weight: 400; } }", 1, TRUE,
              "@keyframes k { from { min-width: 0px; } to { min-width: 10px; font-weight: 700; } }");
}

static void
on_changed (VibeosText *text, int *count)
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
test_settings (void)
{
  g_autoptr (GSettings) interface = g_settings_new ("org.gnome.desktop.interface");
  g_autoptr (GSettings) settings = g_settings_new ("vibeos.Interface");
  VibeosText *text = vibeos_text_get_default ();
  int changes = 0;

  g_signal_connect (text, "changed", G_CALLBACK (on_changed), &changes);
  g_assert_true (g_settings_get_boolean (settings, "bold-text"));

  /* an app that does not follow the settings: px at 1 and bold, whatever they say */
  g_settings_set_double (interface, "text-scaling-factor", 1.3);
  g_settings_set_boolean (settings, "bold-text", FALSE);
  settle ();
  g_assert_cmpfloat (vibeos_font_scale (), ==, 1);
  g_assert_true (vibeos_bold_text ());
  g_assert_cmpfloat (vibeos_sp (14), ==, 14);
  g_assert_cmpint (vibeos_font_weight (400), ==, 700);
  g_assert_cmpint (changes, ==, 0);

  /* one that follows them */
  _vibeos_text_set_following (TRUE);
  g_assert_cmpint (changes, ==, 1);
  g_assert_cmpfloat (vibeos_font_scale (), ==, 1.3);
  g_assert_false (vibeos_bold_text ());
  g_assert_cmpfloat_with_epsilon (vibeos_sp (14), 18.8, 1e-4);
  g_assert_cmpint (vibeos_font_weight (400), ==, 400);

  /* as they change */
  g_settings_set_double (interface, "text-scaling-factor", 2.0);
  settle ();
  g_assert_cmpint (changes, ==, 2);
  g_assert_cmpfloat_with_epsilon (vibeos_sp (14), 26, 1e-4);
  g_settings_set_boolean (settings, "bold-text", TRUE);
  settle ();
  g_assert_cmpint (changes, ==, 3);
  g_assert_cmpint (vibeos_font_weight (400), ==, 700);
  g_assert_cmpint (vibeos_font_weight (800), ==, 1000);
  g_settings_set_double (interface, "text-scaling-factor", 1.0);
  settle ();
  g_assert_cmpint (changes, ==, 4);
  g_assert_cmpfloat (vibeos_sp (14), ==, 14);
  /* a setting set to what it is changes nothing */
  g_settings_set_boolean (settings, "bold-text", TRUE);
  settle ();
  g_assert_cmpint (changes, ==, 4);

  /* the stylesheets of an app follow, through its own sheet's provider */
  {
    g_autofree char *css = vibeos_css_convert (".a { font-size: 14sp; font-weight: 400; }", vibeos_font_scale (),
                                               vibeos_bold_text ());
    g_assert_cmpstr (css, ==, ".a { font-size: 14px; font-weight: 700; }");
  }

  /* and back to the fixed values when it stops following them */
  g_settings_set_double (interface, "text-scaling-factor", 1.5);
  settle ();
  g_assert_cmpint (changes, ==, 5);
  _vibeos_text_set_following (FALSE);
  g_assert_cmpint (changes, ==, 6);
  g_assert_cmpfloat (vibeos_sp (14), ==, 14);
}

static void
test_environment (void)
{
  g_autoptr (GSettings) interface = g_settings_new ("org.gnome.desktop.interface");
  VibeosText *text = vibeos_text_get_default ();
  int changes = 0;

  g_signal_connect (text, "changed", G_CALLBACK (on_changed), &changes);
  /* VIBEOS_FONT_SCALE=1.5 VIBEOS_BOLD_TEXT=0 (meson.build), over the settings' 1 and bold */
  _vibeos_text_set_following (TRUE);
  g_assert_cmpint (changes, ==, 1);
  g_assert_cmpfloat (vibeos_font_scale (), ==, 1.5);
  g_assert_false (vibeos_bold_text ());
  g_assert_cmpfloat_with_epsilon (vibeos_sp (14), 22, 1e-4);
  g_settings_set_double (interface, "text-scaling-factor", 2.0);
  settle ();
  g_assert_cmpint (changes, ==, 1);
  g_assert_cmpfloat (vibeos_font_scale (), ==, 1.5);
}

static void
test_no_schema (void)
{
  _vibeos_text_set_following (TRUE);
  g_assert_cmpfloat (vibeos_font_scale (), ==, 1);
  g_assert_true (vibeos_bold_text ());
  g_assert_cmpfloat (vibeos_sp (14), ==, 14);
}

int
main (int argc, char **argv)
{
  const char *which = argc > 1 ? argv[1] : "";

  g_test_init (&argc, &argv, NULL);
  if (g_str_equal (which, "settings"))
    g_test_add_func ("/text/settings", test_settings);
  else if (g_str_equal (which, "environment"))
    g_test_add_func ("/text/environment", test_environment);
  else if (g_str_equal (which, "no-schema"))
    g_test_add_func ("/text/no-schema", test_no_schema);
  else
    {
      g_test_add_func ("/text/curve", test_curve);
      g_test_add_func ("/text/css", test_css);
    }
  return g_test_run ();
}
