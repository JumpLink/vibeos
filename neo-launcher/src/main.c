/* Neo Launcher 2: the home screen as a Vulkan Wayland client. Launcher3 on Android 16, 1:1: the parallax
 * wallpaper zoomed ×1.10, the workspace and dock, and the app drawer sheet whose depth zooms the wallpaper out,
 * shrinks the home to 0.97 and blurs both behind it. */
#include <glib.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <time.h>

#include "image.h"
#include "neo.h"
#include "parallax.h"

#define DRAWER_DEPTH       0.33f      /* bottomSheetDepth */
#define BLUR_MAX_DP        30         /* max_depth_blur_radius_enhanced */
#define BLUR_FULL_DEPTH    0.3f

void drawer_prepare (void);

Neo N;

typedef enum { OWNER_NONE, OWNER_HOME, OWNER_DRAWER } Owner;

static struct {
  Vk          vk;
  bool        ready;
  int         vk_w, vk_h;
  GMainLoop  *loop;
  Parallax   *par;
  GfxTex     *home_target, *bg_target;

  int         touch;              /* the finger being followed, or -1 */
  Owner       owner;
  float       down_x, down_y;
  bool        moved;
  bool        debug_touch;        /* NEO_DEBUG_TOUCH or ~/.config/neo-launcher/debug-touch */
  Tile       *down_tile;
  float       down_ix, down_iy;

  /* frame timing (NEO_STATS) */
  TextFont   *stats_font;
  TextLayout *stats;
  gint64      prev, stats_t0;
  int         frames, late;
  double      worst_ms, cpu_ms;
} A = { .touch = -1 };

static gint64
mono_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (gint64) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* an event's time (ms, 32 bits of CLOCK_MONOTONIC) on the frame clock's scale */
static double
event_ms (guint32 t)
{
  double now = mono_ns () / 1e6;
  guint32 behind = (guint32) (guint64) now - t;
  return behind < 10000 ? now - behind : now;
}

static void
wake (void *data)
{
  wl_schedule (N.wl);
}

static gboolean
clock_tick (gpointer data)
{
  home_clock_tick ();
  /* on the next minute's boundary */
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int ms = (60 - g_date_time_get_second (now)) * 1000 + 20;
  g_timeout_add (MAX (ms, 1000), clock_tick, NULL);
  return G_SOURCE_REMOVE;
}

/* ---- the blur ------------------------------------------------------------------------------------------------- */

/* σ (full-size px) of gfx_blur on a half-size picture, per levels and offset 0.5 … 4 (simulated, then drawn back
 * up bilinearly): pick the levels whose range holds σ, interpolate the offset */
static const float SIGMA[4][8] = {
  { 1.7f, 1.7f, 2.1f, 2.4f, 2.5f, 2.6f, 3.1f, 3.3f },
  { 4.2f, 6.0f, 8.4f, 10.7f, 12.7f, 14.9f, 17.6f, 20.0f },
  { 8.7f, 13.0f, 18.5f, 23.5f, 27.9f, 33.0f, 38.8f, 44.2f },
  { 17.6f, 26.5f, 37.9f, 48.0f, 57.1f, 67.4f, 79.2f, 90.3f },
};

static void
blur_params (float sigma, int *levels, float *offset)
{
  int l = sigma <= 3.3f ? 0 : sigma <= 14.9f ? 1 : sigma <= 33.0f ? 2 : 3;
  const float *row = SIGMA[l];
  float o = 4;
  for (int i = 0; i < 7; i++)
    if (sigma <= row[i + 1]) {
      float span = row[i + 1] - row[i];
      o = 0.5f + 0.5f * (i + (span > 0 ? CLAMP ((sigma - row[i]) / span, 0, 1) : 1));
      break;
    }
  if (sigma < row[0])
    o = 0.5f;
  *levels = l + 1;
  *offset = o;
}

static GfxTex *
target (GfxTex **t, int w, int h)
{
  if (!*t || (*t)->w != w || (*t)->h != h) {
    gfx_tex_free (*t);
    *t = gfx_target_new (w, h);
  }
  return *t;
}

/* ---- frames --------------------------------------------------------------------------------------------------- */

static bool
init_gpu (void)
{
  if (!vk_init (&A.vk, wl_get_display (N.wl), wl_get_surface (N.wl), N.W, N.H) || !gfx_init (&A.vk) || !text_init ())
    return false;
  A.vk_w = N.W;
  A.vk_h = N.H;
  const char *dir = g_getenv ("NEO_PARALLAX");
  g_autofree char *set = g_settings_get_string (N.settings, "wallpaper-parallax");
  A.par = parallax_new (dir ? dir : set, wake, NULL);
  if (A.par) {
    parallax_resize (A.par, N.W, N.H, N.dp);
    parallax_run (A.par, true);
  }
  tiles_load ();
  home_layout ();
  drawer_layout ();
  nav_init ();
  clock_tick (NULL);
  if (g_getenv ("NEO_STATS"))
    A.stats_font = text_font ("Roboto Mono", 400, dp (12));
  A.ready = true;
  return true;
}

static void
stats_frame (gint64 time, gint64 cpu_start)
{
  gint64 refresh = wl_refresh_ns (N.wl);
  if (A.prev) {
    double dt = (time - A.prev) / 1e6;
    if (dt < 100) {
      A.frames++;
      A.worst_ms = MAX (A.worst_ms, dt);
      if (refresh && time - A.prev > refresh * 3 / 2)
        A.late++;
    }
  }
  A.prev = time;
  A.cpu_ms = MAX (A.cpu_ms, (mono_ns () - cpu_start) / 1e6);
  if (!A.stats_t0)
    A.stats_t0 = time;
  if (time - A.stats_t0 > 1000000000) {
    double secs = (time - A.stats_t0) / 1e9;
    g_autofree char *s = g_strdup_printf ("%.1f fps  late %d  worst %.1f ms  cpu %.1f ms", A.frames / secs, A.late,
                                          A.worst_ms, A.cpu_ms);
    if (A.stats_font) {
      text_layout_free (A.stats);
      A.stats = text_layout (A.stats_font, s, 0, 1, TEXT_LEFT);
    }
    A.frames = A.late = 0;
    A.worst_ms = A.cpu_ms = 0;
    A.stats_t0 = time;
  }
}

/* the workspace and dock, through the page stretch when one shows */
static void
draw_home (float scale, float stretch)
{
  float alpha = nav_home_alpha ();
  if (alpha <= 0)
    return;
  gfx_save ();
  gfx_opacity (alpha);
  if (stretch != 0 && A.home_target) {
    float p[8] = { stretch, 0, N.W, N.H };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (A.home_target, GFX_PIPE_STRETCH);
    q->w = N.W;
    q->h = N.H;
    q->u1 = q->v1 = 1;
    q->r = q->g = q->b = q->a;
    q->mode = GFX_MODE_TEX;
  } else {
    home_draw (scale);
  }
  gfx_restore ();
}

static void
on_frame (gpointer data, gint64 time)
{
  gint64 cpu_start = mono_ns ();
  N.now = time / 1e6;
  if (!A.ready && !init_gpu ()) {
    g_main_loop_quit (A.loop);
    return;
  }
  if (A.vk_w != N.W || A.vk_h != N.H) {
    vk_resize (&A.vk, N.W, N.H);
    A.vk_w = N.W;
    A.vk_h = N.H;
    if (A.par)
      parallax_resize (A.par, N.W, N.H, N.dp);
    home_layout ();
    drawer_layout ();
    nav_icons_changed ();
  }
  bool more = A.par ? parallax_tick (A.par) : false;
  more |= home_frame ();
  more |= drawer_frame ();
  more |= nav_frame ();
  more |= N.launch_t0 != 0;

  /* the drawer's depth, or an app's over the launcher: the wallpaper zooms out, the home shrinks, both blur
   * (BaseDepthController) */
  float f = drawer_progress ();
  float de = spongy_curve (SPONGY_LINEAR_OUT_SLOW_IN, f);           /* DECELERATED_EASE (0, 0, 0.2, 1) */
  float depth = floorf (MAX (DRAWER_DEPTH * de, nav_depth ()) * 256) / 256;
  float blurring = floorf (MAX (DRAWER_DEPTH * de, nav_blurs () ? nav_depth () : 0) * 256) / 256;
  float radius = floorf (MIN (1, blurring / BLUR_FULL_DEPTH) * dp (BLUR_MAX_DP));
  float zoom = 1 + (WALLPAPER_ZOOM - 1) * (1 - depth);
  float scale = (1 + (CONTENT_SCALE - 1) * de) * launch_content_scale () * nav_home_scale ();
  float stretch = home_stretch ();

  if (!gfx_frame_begin ()) {
    wl_schedule (N.wl);
    return;
  }
  if (stretch != 0) {
    gfx_pass_begin (target (&A.home_target, N.W, N.H), (GfxColor) { 0, 0, 0, 0 });
    home_draw (scale);
    gfx_pass_end ();
  }
  GfxTex *blurred = NULL;
  if (radius >= 1) {
    /* the wallpaper and home at half size, blurred to hwui's σ = 0.57735·r + 0.5 */
    gfx_pass_begin (target (&A.bg_target, N.W / 2, N.H / 2), (GfxColor) { 0, 0, 0, 1 });
    gfx_scale (0.5f, 0.5f, 0, 0);
    if (A.par)
      parallax_draw (A.par, zoom);
    draw_home (scale, stretch);
    gfx_pass_end ();
    int levels;
    float offset;
    blur_params (0.57735f * radius + 0.5f, &levels, &offset);
    blurred = gfx_blur (A.bg_target, levels, offset, 0);
  }
  drawer_prepare ();

  gfx_pass_begin (NULL, (GfxColor) { 0, 0, 0, 1 });
  if (blurred) {
    gfx_image (blurred, 0, 0, N.W, N.H, 0, 1);
  } else {
    if (A.par)
      parallax_draw (A.par, zoom);
    draw_home (scale, stretch);
  }
  drawer_draw ();
  if (A.stats)
    text_draw (A.stats, dp (16), dp (48), (GfxColor) { 1, 1, 1, 0.9f });
  gfx_pass_end ();
  wl_frame_requested (N.wl);       /* before the present, which commits it */
  gfx_frame_end ();
  stats_frame (time, cpu_start);
  if (more || g_getenv ("NEO_SPIN"))
    wl_schedule (N.wl);
}

/* ---- input ---------------------------------------------------------------------------------------------------- */

static void
on_touch (gpointer data, WlTouchType type, int id, double x, double y, guint32 time)
{
  if (!A.ready)
    return;
  double t = event_ms (time);
  N.now = MAX (N.now, t);
  if (A.debug_touch && type != WL_TOUCH_MOTION)
    g_message ("touch %d id %d at %.0f,%.0f owner %d", type, id, x, y, A.owner);
  switch (type) {
  case WL_TOUCH_DOWN:
    if (A.touch >= 0)
      return;
    A.touch = id;
    A.down_x = x;
    A.down_y = y;
    A.moved = false;
    A.down_tile = NULL;
    if (drawer_touch_down (x, y, t)) {
      A.owner = OWNER_DRAWER;
      break;
    }
    A.owner = OWNER_HOME;
    home_touch_down (x, y, t);
    A.down_tile = home_tile_at (x, y, &A.down_ix, &A.down_iy);
    if (A.down_tile)
      press_down (A.down_tile);
    break;
  case WL_TOUCH_MOTION:
    if (id != A.touch)
      return;
    if (hypot (x - A.down_x, y - A.down_y) > dp (8))
      A.moved = true;
    if (A.owner == OWNER_HOME) {
      bool vertical = false;
      home_touch_move (x, y, t, &vertical);
      if (A.moved)
        press_up ();
      if (vertical) {
        if (y < A.down_y) {
          A.owner = OWNER_DRAWER;
          drawer_drag_begin (A.down_y, y, t);
        } else {
          A.owner = OWNER_NONE;     /* down: the shell's notification shade */
        }
      }
    } else if (A.owner == OWNER_DRAWER) {
      drawer_touch_move (x, y, t);
    }
    break;
  case WL_TOUCH_UP:
    if (id != A.touch)
      return;
    if (A.owner == OWNER_HOME) {
      home_touch_up (x, y, t);
      press_up ();
      if (!A.moved && A.down_tile)
        tile_launch (A.down_tile, A.down_ix, A.down_iy, roundf (dp (ICON_DP)), false);
    } else if (A.owner == OWNER_DRAWER) {
      drawer_touch_up (x, y, t);
    }
    A.touch = -1;
    A.owner = OWNER_NONE;
    break;
  case WL_TOUCH_CANCEL:
    if (A.owner == OWNER_HOME)
      home_touch_cancel (t);
    else if (A.owner == OWNER_DRAWER)
      drawer_touch_cancel (t);
    press_up ();
    A.touch = -1;
    A.owner = OWNER_NONE;
    break;
  }
  wl_schedule (N.wl);
}

static void
on_key (gpointer data, guint32 key, bool pressed)
{
  if (pressed && key == KEY_Q)
    g_main_loop_quit (A.loop);
}

static void
on_resize (gpointer data, int w, int h, double scale)
{
  N.W = w;
  N.H = h;
  const char *d = g_getenv ("NEO_DENSITY");
  N.dp = d ? g_ascii_strtod (d, NULL) : 3.0;
  spongy_set_density (N.dp);
}

static void
on_close (gpointer data)
{
  g_main_loop_quit (A.loop);
}

static gboolean
on_signal (gpointer data)
{
  g_main_loop_quit (A.loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  static const WlCallbacks cb = { on_resize, on_frame, on_touch, on_key, on_close };
  A.loop = g_main_loop_new (NULL, FALSE);
  N.settings = g_settings_new ("de.yesman.neo");
  g_autofree char *flag = g_build_filename (g_get_user_config_dir (), "neo-launcher", "debug-touch", NULL);
  A.debug_touch = g_getenv ("NEO_DEBUG_TOUCH") || g_file_test (flag, G_FILE_TEST_EXISTS);
  N.wl = wl_open ("de.yesman.NeoLauncher", &cb, NULL);
  if (!N.wl)
    return 1;
  g_unix_signal_add (SIGTERM, on_signal, NULL);
  g_unix_signal_add (SIGINT, on_signal, NULL);
  wl_schedule (N.wl);
  g_main_loop_run (A.loop);
  if (A.ready)
    vkDeviceWaitIdle (A.vk.dev);
  return 0;
}
