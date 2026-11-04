/* Neo Launcher 3: the home screen as a Vulkan Wayland client. For now the renderer's test scene: the parallax
 * wallpaper under every installed app in a five-column stretch-scrolling grid, with frame timing on screen. */
#include <glib.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <time.h>

#include "apps.h"
#include "gfx.h"
#include "image.h"
#include "parallax.h"
#include "spongy.h"
#include "text.h"
#include "wl.h"

typedef struct {
  GAppInfo   *info;
  GfxTex     *icon;
  TextLayout *label;
} Tile;

static struct {
  Wl         *wl;
  Vk          vk;
  bool        ready;
  int         W, H, vk_w, vk_h;
  double      dp;                  /* pixels per dp: Android's density (480 dpi = 3), whatever the shell's scale */
  GMainLoop  *loop;

  Parallax   *par;
  GPtrArray  *tiles;
  SpongyList  list;
  float       list_top, row_h, col_w, icon_px;
  TextFont   *label_font, *stats_font;
  GfxTex     *target;              /* the list, while it is stretched */
  int         touch;               /* the touch that drives the list, or -1 */
  float       down_x, down_y;
  GDBusConnection *bus;

  /* frame timing */
  gint64      prev, stats_t0;
  int         frames, late;
  double      worst_ms, cpu_ms;
  TextLayout *stats;
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
  wl_schedule (A.wl);
}

/* ---- the grid ------------------------------------------------------------------------------------------------- */

#define COLS 5

static void
layout_grid (void)
{
  A.col_w = A.W / (float) COLS;
  A.icon_px = roundf (52 * A.dp);
  A.row_h = roundf (A.icon_px + 8 * A.dp + text_line_height (A.label_font) * 2 + 16 * A.dp);
  A.list_top = roundf (48 * A.dp);
  int rows = (A.tiles->len + COLS - 1) / COLS;
  float content = rows * A.row_h + 16 * A.dp;
  if (A.list.view_height == 0)
    spongy_list_init (&A.list, A.H - A.list_top, content);
  else
    spongy_list_set_size (&A.list, A.H - A.list_top, content);
}

static void
load_tiles (void)
{
  g_autoptr (GHashTable) apps = apps_installed ();
  GList *infos = g_list_sort (g_hash_table_get_values (apps), app_compare);
  A.tiles = g_ptr_array_new ();
  gint64 t0 = g_get_monotonic_time ();
  int px = (int) roundf (52 * A.dp);
  float label_w = A.W / (float) COLS - 8 * A.dp;
  for (GList *l = infos; l; l = l->next) {
    Tile *t = g_new0 (Tile, 1);
    t->info = g_object_ref (l->data);
    GdkPixbuf *pb = image_icon (g_app_info_get_icon (t->info), px);
    if (pb) {
      t->icon = image_texture (pb);
      g_object_unref (pb);
    }
    t->label = text_layout (A.label_font, app_name (t->info), label_w, 1, TEXT_CENTER);
    g_ptr_array_add (A.tiles, t);
  }
  g_list_free (infos);
  g_message ("%u apps, icons and labels in %.0f ms", A.tiles->len, (g_get_monotonic_time () - t0) / 1000.0);
}

static void
draw_grid (float view_h)
{
  float off = roundf (A.list.offset);        /* whole pixels, as Android scrolls */
  int first = MAX (0, (int) floorf (off / A.row_h)) * COLS;
  int last = MIN ((int) A.tiles->len, ((int) ceilf ((off + view_h) / A.row_h) + 1) * COLS);
  GfxColor white = { 1, 1, 1, 1 };
  for (int i = first; i < last; i++) {
    Tile *t = A.tiles->pdata[i];
    float x = (i % COLS) * A.col_w, y = (i / COLS) * A.row_h - off + 8 * A.dp;
    if (t->icon)
      gfx_image (t->icon, roundf (x + (A.col_w - A.icon_px) / 2), roundf (y), A.icon_px, A.icon_px, 0, 1);
    else
      gfx_rect (roundf (x + (A.col_w - A.icon_px) / 2), roundf (y), A.icon_px, A.icon_px, A.icon_px / 2,
                GFX_RGBA (0x5f6368, 1));
    text_draw (t->label, x + 4 * A.dp, y + A.icon_px + 8 * A.dp, white);
  }
}

/* ---- frames --------------------------------------------------------------------------------------------------- */

static bool
init_gpu (void)
{
  if (!vk_init (&A.vk, wl_get_display (A.wl), wl_get_surface (A.wl), A.W, A.H) || !gfx_init (&A.vk) || !text_init ())
    return false;
  A.vk_w = A.W;
  A.vk_h = A.H;
  A.label_font = text_font ("Roboto", 400, roundf (14 * A.dp));
  A.stats_font = text_font ("Roboto Mono", 400, roundf (12 * A.dp));
  const char *dir = g_getenv ("NEO_PARALLAX");
  g_autofree char *def = g_build_filename (g_get_user_data_dir (), "neo-launcher", "parallax", "solar", NULL);
  A.par = parallax_new (dir ? dir : def, wake, NULL);
  if (A.par) {
    parallax_resize (A.par, A.W, A.H, A.dp);
    parallax_run (A.par, true);
  }
  load_tiles ();
  layout_grid ();
  A.ready = true;
  return true;
}

static void
stats_frame (gint64 time, gint64 cpu_start)
{
  gint64 refresh = wl_refresh_ns (A.wl);
  if (A.prev) {
    double dt = (time - A.prev) / 1e6;
    /* only frames in a run count: a gap of more than ~100 ms is the clock having stopped, not a dropped frame */
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
    g_autofree char *s = g_strdup_printf ("%.1f fps  late %d  worst %.1f ms  cpu %.1f ms  %.2f Hz", A.frames / secs,
                                          A.late, A.worst_ms, A.cpu_ms, refresh ? 1e9 / refresh : 0);
    g_message ("%s", s);
    text_layout_free (A.stats);
    A.stats = text_layout (A.stats_font, s, 0, 1, TEXT_LEFT);
    A.frames = A.late = 0;
    A.worst_ms = A.cpu_ms = 0;
    A.stats_t0 = time;
  }
}

static void
on_frame (gpointer data, gint64 time)
{
  gint64 cpu_start = mono_ns ();
  if (!A.ready && !init_gpu ()) {
    g_main_loop_quit (A.loop);
    return;
  }
  if (A.vk_w != A.W || A.vk_h != A.H) {
    vk_resize (&A.vk, A.W, A.H);
    A.vk_w = A.W;
    A.vk_h = A.H;
    if (A.par)
      parallax_resize (A.par, A.W, A.H, A.dp);
    layout_grid ();
  }
  double now = time / 1e6;
  bool more = parallax_tick (A.par);
  float top, bottom;
  more |= spongy_list_frame (&A.list, now, &top, &bottom);
  float amount = top + bottom;

  if (!gfx_frame_begin ()) {
    wl_schedule (A.wl);
    return;
  }
  float lh = A.H - A.list_top;
  if (amount != 0) {
    if (!A.target || A.target->w != A.W || A.target->h != (int) lh) {
      gfx_tex_free (A.target);
      A.target = gfx_target_new (A.W, (int) lh);
    }
    gfx_pass_begin (A.target, (GfxColor) { 0, 0, 0, 0 });
    draw_grid (lh);
    gfx_pass_end ();
  }
  gfx_pass_begin (NULL, (GfxColor) { 0, 0, 0, 1 });
  if (A.par)
    parallax_draw (A.par, 1);
  gfx_rect (0, 0, A.W, A.H, 0, (GfxColor) { 0, 0, 0, 0.45f });
  if (amount != 0 && A.target) {
    float p[8] = { 0, amount, A.W, lh };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (A.target, GFX_PIPE_STRETCH);
    q->x = 0;
    q->y = A.list_top;
    q->w = A.W;
    q->h = lh;
    q->u1 = q->v1 = 1;
    q->r = q->g = q->b = q->a;
    q->mode = GFX_MODE_TEX;
  } else {
    gfx_save ();
    gfx_translate (0, A.list_top);
    gfx_clip (0, 0, A.W, lh);
    draw_grid (lh);
    gfx_restore ();
  }
  if (A.stats)
    text_draw (A.stats, 16 * A.dp, 14 * A.dp, (GfxColor) { 1, 1, 1, 0.9f });
  gfx_pass_end ();
  wl_frame_requested (A.wl);       /* before the present, which commits it */
  gfx_frame_end ();
  stats_frame (time, cpu_start);
  if (more || g_getenv ("NEO_SPIN"))
    wl_schedule (A.wl);
}

/* the tile under a point on screen, or -1 */
static int
tile_at (float x, float y)
{
  float gy = y - A.list_top + roundf (A.list.offset) - 8 * A.dp;
  if (y < A.list_top || gy < 0 || x < 0 || x >= A.W)
    return -1;
  int row = (int) (gy / A.row_h), col = (int) (x / A.col_w);
  int i = row * COLS + col;
  if (i >= (int) A.tiles->len || gy - row * A.row_h > A.icon_px + 8 * A.dp + text_line_height (A.label_font))
    return -1;
  return i;
}

static void
launch (int i)
{
  Tile *t = A.tiles->pdata[i];
  float x = (i % COLS) * A.col_w + (A.col_w - A.icon_px) / 2;
  float y = (i / COLS) * A.row_h - roundf (A.list.offset) + 8 * A.dp + A.list_top;
  if (!A.bus)
    A.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!A.bus)
    return;
  g_dbus_connection_call (A.bus, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone", "Launch",
                          g_variant_new ("(s(dddd))", g_app_info_get_id (t->info), (double) x, (double) y,
                                         (double) A.icon_px, (double) A.icon_px),
                          NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

/* ---- input ---------------------------------------------------------------------------------------------------- */

static void
on_touch (gpointer data, WlTouchType type, int id, double x, double y, guint32 time)
{
  if (!A.ready)
    return;
  double t = event_ms (time);
  switch (type) {
  case WL_TOUCH_DOWN:
    if (A.touch >= 0)
      return;
    A.touch = id;
    A.down_x = x;
    A.down_y = y;
    spongy_list_down (&A.list, y, t);
    break;
  case WL_TOUCH_MOTION:
    if (id != A.touch)
      return;
    spongy_list_move (&A.list, y, t);
    break;
  case WL_TOUCH_UP:
    if (id != A.touch)
      return;
    {
      /* a tap: down and up without a drag, on a tile */
      bool tap = !A.list.dragging;
      spongy_list_up (&A.list, t);
      int i = tap ? tile_at (A.down_x, A.down_y) : -1;
      if (i >= 0)
        launch (i);
    }
    A.touch = -1;
    break;
  case WL_TOUCH_CANCEL:
    spongy_list_cancel (&A.list, t);
    A.touch = -1;
    break;
  }
  wl_schedule (A.wl);
}

static void
on_key (gpointer data, guint32 key, bool pressed)
{
  if (pressed && (key == KEY_ESC || key == KEY_Q))
    g_main_loop_quit (A.loop);
}

static void
on_resize (gpointer data, int w, int h, double scale)
{
  A.W = w;
  A.H = h;
  const char *d = g_getenv ("NEO_DENSITY");
  A.dp = d ? g_ascii_strtod (d, NULL) : 3.0;
  spongy_set_density (A.dp);
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
  A.wl = wl_open ("de.yesman.NeoLauncher", &cb, NULL);
  if (!A.wl)
    return 1;
  g_unix_signal_add (SIGTERM, on_signal, NULL);
  g_unix_signal_add (SIGINT, on_signal, NULL);
  wl_schedule (A.wl);
  g_main_loop_run (A.loop);
  if (A.ready)
    vkDeviceWaitIdle (A.vk.dev);
  return 0;
}
