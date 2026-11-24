/* ns-monitors against mutter's own answers: the S20's GetCurrentState with an LG TV on its DisplayPort (s20-lg-tv.state,
 * as gdbus printed it), and two made-up ones for what that pair cannot do (a mirror, a display that is off). The
 * configurations are checked against mutter's rules (see ns-monitors.c); ApplyMonitorsConfig's method 0 on the S20
 * accepted the Join and main-display ones built here. */
#include "../src/ns-monitors.h"

static NsMonitors *
load_s20 (void)
{
  g_autofree char *text = NULL;
  g_autoptr (GError) err = NULL;
  g_assert_true (g_file_get_contents (TEST_DATA "/s20-lg-tv.state", &text, NULL, &err));
  NsMonitors *m = ns_monitors_new_from_text (text, &err);
  g_assert_no_error (err);
  g_assert_nonnull (m);
  return m;
}

static NsMonitors *
parse (const char *text)
{
  g_autoptr (GError) err = NULL;
  NsMonitors *m = ns_monitors_new_from_text (text, &err);
  g_assert_no_error (err);
  return m;
}

static NsLogicalConfig *
logical_of (NsConfig *c, NsMonitors *m, const char *connector)
{
  NsLogicalConfig *l = ns_config_find (c, ns_monitors_find (m, connector));
  g_assert_nonnull (l);
  return l;
}

static const char *
mode_of (NsLogicalConfig *l, const char *connector)
{
  for (guint k = 0; k < l->placed->len; k++) {
    NsPlaced *p = l->placed->pdata[k];
    if (!g_strcmp0 (p->monitor->connector, connector))
      return p->mode->id;
  }
  return NULL;
}

static void
assert_at (NsLogicalConfig *l, int x, int y, double scale, gboolean primary)
{
  g_assert_cmpint (l->x, ==, x);
  g_assert_cmpint (l->y, ==, y);
  g_assert_cmpfloat_with_epsilon (l->scale, scale, 0.0001);
  g_assert_cmpint (l->primary, ==, primary);
}

static void
test_parse (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  g_assert_cmpuint (m->serial, ==, 6);
  g_assert_cmpuint (m->layout_mode, ==, NS_LAYOUT_LOGICAL);
  g_assert_true (m->can_change_layout_mode);
  g_assert_cmpuint (m->monitors->len, ==, 2);
  g_assert_cmpuint (m->logical->len, ==, 2);

  NsMonitor *panel = ns_monitors_find (m, "DSI-1"), *tv = ns_monitors_find (m, "DP-1");
  g_assert_true (panel == ns_monitors_builtin (m));
  g_assert_cmpstr (panel->name, ==, "Built-in display");
  g_assert_cmpstr (tv->name, ==, "LG Electronics LG TV SSCR2");
  g_assert_cmpstr (tv->product, ==, "LG TV SSCR2");
  g_assert_false (tv->builtin);
  g_assert_cmpuint (panel->modes->len, ==, 1);
  g_assert_cmpuint (tv->modes->len, ==, 21);
  g_assert_cmpstr (panel->mode->id, ==, "1080x2400@120.000");
  g_assert_cmpstr (tv->mode->id, ==, "1920x1080@60.000");
  g_assert_true (ns_monitor_preferred_mode (tv) == tv->mode);
  g_assert_cmpfloat_with_epsilon (panel->logical->scale, 3.0, 0.0001);
  g_assert_cmpint (tv->logical->x, ==, 360);

  g_assert_true (ns_monitors_primary (m) == panel);
  g_assert_false (ns_monitors_is_mirror (m));
  g_assert_cmpuint (ns_monitors_n_on (m), ==, 2);

  /* the rates the TV has at 1920 x 1080, the one nearest a rate asked for */
  g_assert_cmpstr (ns_monitor_mode_for_size (tv, 1920, 1080, 0)->id, ==, "1920x1080@60.000");
  g_assert_cmpstr (ns_monitor_mode_for_size (tv, 1920, 1080, 24.1)->id, ==, "1920x1080@24.000");
  g_assert_cmpstr (ns_monitor_mode_for_size (tv, 1280, 720, 59.94)->id, ==, "1280x720@59.940");
  g_assert_null (ns_monitor_mode_for_size (tv, 1080, 2400, 0));
}

/* The panel is 1080 x 2400 and the TV has nothing that size (nor 2400 x 1080): no mirror */
static void
test_no_mirror (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  g_autoptr (GArray) sizes = ns_monitors_mirror_sizes (m);
  g_assert_cmpuint (sizes->len, ==, 0);
  g_assert_null (ns_config_mirror (m, 1920, 1080));
  g_assert_null (ns_config_mirror (m, 1080, 2400));
}

static void
test_join (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  g_autoptr (NsConfig) c = ns_config_join (m);
  g_assert_cmpuint (c->len, ==, 2);
  /* the panel at scale 3 is 360 wide; the TV right of it */
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 3.0, TRUE);
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, FALSE);
  g_assert_cmpstr (mode_of (logical_of (c, m, "DP-1"), "DP-1"), ==, "1920x1080@60.000");

  /* what goes to mutter: as it is now, the monitors' own properties handed back */
  g_autoptr (GVariant) v = g_variant_ref_sink (ns_config_to_variant (m, c, NS_MONITORS_VERIFY));
  g_autofree char *text = g_variant_print (v, FALSE);
  g_assert_cmpstr (text, ==,
                   "(6, 0, [(0, 0, 3.0, 0, true, [('DSI-1', '1080x2400@120.000', "
                   "{'color-mode': <uint32 0>, 'rgb-range': <uint32 1>})]), "
                   "(360, 0, 1.0, 0, false, [('DP-1', '1920x1080@60.000', "
                   "{'color-mode': <uint32 0>, 'rgb-range': <uint32 1>})])], {'layout-mode': <uint32 1>})");

  /* the configuration as it is reads the same */
  g_autoptr (NsConfig) now = ns_config_current (m);
  g_autoptr (GVariant) w = g_variant_ref_sink (ns_config_to_variant (m, now, NS_MONITORS_VERIFY));
  g_assert_true (g_variant_equal (v, w));
}

static void
test_primary (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  g_autoptr (NsConfig) c = ns_config_current (m);
  ns_config_set_primary (c, ns_monitors_find (m, "DP-1"));
  /* only the main display moves: the screens stay where they are */
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 3.0, FALSE);
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, TRUE);
  ns_config_set_primary (c, ns_monitors_find (m, "DSI-1"));
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 3.0, TRUE);
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, FALSE);
}

static void
test_mode_and_scale (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  NsMonitor *panel = ns_monitors_find (m, "DSI-1"), *tv = ns_monitors_find (m, "DP-1");
  g_autoptr (NsConfig) c = ns_config_current (m);

  ns_config_set_mode (m, c, tv, ns_monitor_find_mode (tv, "1280x720@50.000"));
  g_assert_cmpstr (mode_of (logical_of (c, m, "DP-1"), "DP-1"), ==, "1280x720@50.000");
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, FALSE);

  /* 640 x 480 takes scale 1 only */
  ns_config_set_scale (m, c, tv, 2.0);
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 2.0, FALSE);
  ns_config_set_mode (m, c, tv, ns_monitor_find_mode (tv, "640x480@60.000"));
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, FALSE);

  /* the panel at 2 is 540 wide: the TV moves right, not under it */
  ns_config_set_scale (m, c, panel, 2.0);
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 2.0, TRUE);
  assert_at (logical_of (c, m, "DP-1"), 540, 0, 1.0, FALSE);
  g_autoptr (GArray) scales = ns_config_scales (c, panel);
  g_assert_cmpuint (scales->len, ==, 10);
}

static void
test_on_off (void)
{
  g_autoptr (NsMonitors) m = load_s20 ();
  NsMonitor *panel = ns_monitors_find (m, "DSI-1"), *tv = ns_monitors_find (m, "DP-1");
  g_autoptr (NsConfig) c = ns_config_current (m);

  ns_config_set_on (m, c, tv, FALSE);
  g_assert_cmpuint (c->len, ==, 1);
  g_assert_null (ns_config_find (c, tv));
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 3.0, TRUE);
  /* the last display on stays on */
  ns_config_set_on (m, c, panel, FALSE);
  g_assert_cmpuint (c->len, ==, 1);

  /* back on: right of the panel, at its preferred mode and scale */
  ns_config_set_on (m, c, tv, TRUE);
  assert_at (logical_of (c, m, "DP-1"), 360, 0, 1.0, FALSE);
  g_assert_cmpstr (mode_of (logical_of (c, m, "DP-1"), "DP-1"), ==, "1920x1080@60.000");

  /* the main display off: the panel is main */
  g_autoptr (NsConfig) d = ns_config_current (m);
  ns_config_set_primary (d, tv);
  ns_config_set_on (m, d, tv, FALSE);
  assert_at (logical_of (d, m, "DSI-1"), 0, 0, 3.0, TRUE);
}

/* the panel turned on its side (transform 1, 90°): 2400 / 3 = 800 wide */
static void
test_rotated (void)
{
  g_autofree char *text = NULL;
  g_assert_true (g_file_get_contents (TEST_DATA "/s20-lg-tv.state", &text, NULL, NULL));
  g_autofree char *turned = NULL;
  {
    g_auto (GStrv) parts = g_strsplit (text, "(0, 0, 3.0, uint32 0, true", 2);
    g_assert_cmpuint (g_strv_length (parts), ==, 2);
    turned = g_strjoin ("(0, 0, 3.0, uint32 1, true", parts[0], parts[1], NULL);
  }
  g_autoptr (NsMonitors) m = parse (turned);
  g_autoptr (NsConfig) c = ns_config_join (m);
  assert_at (logical_of (c, m, "DSI-1"), 0, 0, 3.0, TRUE);
  g_assert_cmpuint (logical_of (c, m, "DSI-1")->transform, ==, 1);
  assert_at (logical_of (c, m, "DP-1"), 800, 0, 1.0, FALSE);
}

/* A laptop-like panel and a TV that has its resolution: what the S20 and the LG cannot do */
#define SCALES "[1.0, 1.25, 1.5, 2.0]"
static const char *const pair =
  "(uint32 3, ["
  "(('eDP-1', 'BOE', '0x0a2f', '0x00000000'), [('1920x1080@60.000', 1920, 1080, 60.0, 1.5, " SCALES ", "
  "{'is-current': <true>, 'is-preferred': <true>})], {'is-builtin': <true>, 'display-name': <'Built-in display'>}), "
  "(('HDMI-1', 'SAM', 'TV', '0x1'), ["
  "('1920x1080@60.000', 1920, 1080, 60.0, 1.0, [1.0, 2.0], {'is-current': <true>, 'is-preferred': <true>}), "
  "('1920x1080@50.000', 1920, 1080, 50.0, 1.0, [1.0, 2.0], @a{sv} {}), "
  "('1280x720@60.000', 1280, 720, 60.0, 1.0, [1.0], @a{sv} {})], "
  "{'is-builtin': <false>, 'display-name': <'Samsung TV'>})], "
  "[(0, 0, 1.5, uint32 0, true, [('eDP-1', 'BOE', '0x0a2f', '0x00000000')], @a{sv} {}), "
  "(1280, 0, 1.0, 0, false, [('HDMI-1', 'SAM', 'TV', '0x1')], {})], "
  "{'layout-mode': <uint32 1>, 'supports-changing-layout-mode': <true>})";

static void
test_mirror (void)
{
  g_autoptr (NsMonitors) m = parse (pair);
  g_autoptr (GArray) sizes = ns_monitors_mirror_sizes (m);
  g_assert_cmpuint (sizes->len, ==, 1);
  g_assert_cmpint (g_array_index (sizes, NsSize, 0).width, ==, 1920);
  g_assert_cmpint (g_array_index (sizes, NsSize, 0).height, ==, 1080);
  g_assert_null (ns_config_mirror (m, 1280, 720));

  g_autoptr (NsConfig) c = ns_config_mirror (m, 1920, 1080);
  g_assert_nonnull (c);
  g_assert_cmpuint (c->len, ==, 1);
  NsLogicalConfig *l = c->pdata[0];
  /* 1.5 is the panel's only: the scale both take that is nearest it */
  assert_at (l, 0, 0, 1.0, TRUE);
  g_assert_cmpuint (l->placed->len, ==, 2);
  g_assert_cmpstr (mode_of (l, "eDP-1"), ==, "1920x1080@60.000");
  g_assert_cmpstr (mode_of (l, "HDMI-1"), ==, "1920x1080@60.000");
  g_autoptr (GArray) scales = ns_config_scales (c, ns_monitors_find (m, "HDMI-1"));
  g_assert_cmpuint (scales->len, ==, 2);

  /* a rate of the TV's own in the mirror: the panel keeps its */
  ns_config_set_mode (m, c, ns_monitors_find (m, "HDMI-1"), ns_monitor_find_mode (ns_monitors_find (m, "HDMI-1"),
                                                                                  "1920x1080@50.000"));
  g_assert_cmpstr (mode_of (l, "HDMI-1"), ==, "1920x1080@50.000");
  g_assert_cmpstr (mode_of (l, "eDP-1"), ==, "1920x1080@60.000");
}

/* mutter's state once mirrored, and Join out of it */
static void
test_join_from_mirror (void)
{
  g_autofree char *mirrored = NULL;
  {
    g_auto (GStrv) parts = g_strsplit (pair, "[(0, 0, 1.5, uint32 0, true, [('eDP-1', 'BOE', '0x0a2f', '0x00000000')], "
                                             "@a{sv} {}), (1280, 0, 1.0, 0, false, [('HDMI-1', 'SAM', 'TV', '0x1')], {})]", 2);
    g_assert_cmpuint (g_strv_length (parts), ==, 2);
    mirrored = g_strjoin ("[(0, 0, 1.0, uint32 0, true, [('eDP-1', 'BOE', '0x0a2f', '0x00000000'), "
                          "('HDMI-1', 'SAM', 'TV', '0x1')], @a{sv} {})]", parts[0], parts[1], NULL);
  }
  g_autoptr (NsMonitors) m = parse (mirrored);
  g_assert_true (ns_monitors_is_mirror (m));
  g_assert_true (ns_monitors_primary (m) == ns_monitors_builtin (m));
  g_autoptr (NsConfig) c = ns_config_join (m);
  g_assert_cmpuint (c->len, ==, 2);
  assert_at (logical_of (c, m, "eDP-1"), 0, 0, 1.0, TRUE);
  assert_at (logical_of (c, m, "HDMI-1"), 1920, 0, 1.0, FALSE);
}

/* the TV connected but off: Join turns it on, on the right, at its preferred mode */
static void
test_join_off (void)
{
  g_autofree char *off = NULL;
  {
    g_auto (GStrv) parts = g_strsplit (pair, ", (1280, 0, 1.0, 0, false, [('HDMI-1', 'SAM', 'TV', '0x1')], {})", 2);
    g_assert_cmpuint (g_strv_length (parts), ==, 2);
    off = g_strjoin ("", parts[0], parts[1], NULL);
  }
  g_autoptr (NsMonitors) m = parse (off);
  NsMonitor *tv = ns_monitors_find (m, "HDMI-1");
  g_assert_null (tv->logical);
  g_assert_null (tv->mode);
  g_assert_cmpuint (ns_monitors_n_on (m), ==, 1);
  g_autoptr (NsConfig) c = ns_config_join (m);
  assert_at (logical_of (c, m, "eDP-1"), 0, 0, 1.5, TRUE);
  assert_at (logical_of (c, m, "HDMI-1"), 1280, 0, 1.0, FALSE);
  g_assert_cmpstr (mode_of (logical_of (c, m, "HDMI-1"), "HDMI-1"), ==, "1920x1080@60.000");
}

static void
test_bad_state (void)
{
  g_autoptr (GError) err = NULL;
  g_assert_null (ns_monitors_new_from_text ("(uint32 1, [], [])", &err));
  g_assert_nonnull (err);
}

static GArray *
scales_of (const double *list, guint n)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (double));
  g_array_append_vals (a, list, n);
  return a;
}

static void
assert_stop (GArray *stops, guint i, int step, double scale)
{
  g_assert_cmpuint (i, <, stops->len);
  g_assert_cmpint (g_array_index (stops, NsSizeStop, i).step, ==, step);
  g_assert_cmpfloat_with_epsilon (g_array_index (stops, NsSizeStop, i).scale, scale, 1e-3);
}

/* a phone at 3 on stock mutter: the scales below 3 are coarse and there is none above it */
static void
test_display_size_phone (void)
{
  static const double list[] = { 1, 1.25, 4 / 3.0, 1.5, 5 / 3.0, 2, 2.5, 8 / 3.0, 3 };
  g_autoptr (GArray) scales = scales_of (list, G_N_ELEMENTS (list));
  g_autoptr (GArray) stops = ns_display_size_stops (scales, 3, 1080);
  g_assert_cmpuint (stops->len, ==, 4);
  assert_stop (stops, 0, -3, 2);
  assert_stop (stops, 1, -2, 2.5);
  assert_stop (stops, 2, -1, 8 / 3.0);
  assert_stop (stops, 3, 0, 3);
}

/* the same phone with scales in 1/120 steps: every step has its own */
static void
test_display_size_fine (void)
{
  g_autoptr (GArray) scales = g_array_new (FALSE, FALSE, sizeof (double));
  for (int k = 120; k <= 480; k++) {
    double s = k / 120.0;
    g_array_append_val (scales, s);
  }
  g_autoptr (GArray) stops = ns_display_size_stops (scales, 3, 1080);
  g_assert_cmpuint (stops->len, ==, 5);
  assert_stop (stops, 0, -3, 2.1);
  assert_stop (stops, 3, 0, 3);
  assert_stop (stops, 4, 1, 3.375);
}

/* a tablet (1600 screen pixels across at 2: 800 wide) has three sizes larger */
static void
test_display_size_tablet (void)
{
  g_autoptr (GArray) scales = g_array_new (FALSE, FALSE, sizeof (double));
  for (int k = 120; k <= 480; k++) {
    double s = k / 120.0;
    g_array_append_val (scales, s);
  }
  g_autoptr (GArray) stops = ns_display_size_stops (scales, 2, 1600);
  g_assert_cmpuint (stops->len, ==, 7);
  assert_stop (stops, 0, -3, 1.4);
  assert_stop (stops, 3, 0, 2);
  assert_stop (stops, 6, 3, 3);
  g_autoptr (GArray) none = ns_display_size_stops (NULL, 2, 1600);
  g_assert_cmpuint (none->len, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/monitors/parse", test_parse);
  g_test_add_func ("/monitors/no-mirror", test_no_mirror);
  g_test_add_func ("/monitors/join", test_join);
  g_test_add_func ("/monitors/primary", test_primary);
  g_test_add_func ("/monitors/mode-and-scale", test_mode_and_scale);
  g_test_add_func ("/monitors/on-off", test_on_off);
  g_test_add_func ("/monitors/rotated", test_rotated);
  g_test_add_func ("/monitors/mirror", test_mirror);
  g_test_add_func ("/monitors/join-from-mirror", test_join_from_mirror);
  g_test_add_func ("/monitors/join-off", test_join_off);
  g_test_add_func ("/monitors/bad-state", test_bad_state);
  g_test_add_func ("/monitors/display-size-phone", test_display_size_phone);
  g_test_add_func ("/monitors/display-size-fine", test_display_size_fine);
  g_test_add_func ("/monitors/display-size-tablet", test_display_size_tablet);
  return g_test_run ();
}
