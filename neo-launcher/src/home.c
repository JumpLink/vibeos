/* The home screen: Launcher3's workspace (5 × 5 cells, the clock widget across the first page's top row), the dock
 * and the page dots, with PagedView's paging: 1:1 drag after the paging slop, fling or snap with the quintic
 * scroller, and the edge stretch past the first and last page. */
#include <math.h>

#include "neo.h"

#define PAGING_SLOP_DP        16          /* 2 × the 8 dp touch slop */
#define TOUCH_SLOP_DP         8
#define FLING_PX_S_DP         500         /* dp/s */
#define MIN_FLING_DP          250
#define MIN_SNAP_DP           1500
#define SIGNIFICANT_MOVE      0.4f
#define SNAP_MS               750.0
#define MAX_ANGLE             (G_PI / 3)  /* 60°: never a page swipe */
#define DAMP_ANGLE            (G_PI / 6)  /* 30°: the slop grows from here */

#define CLOCK_TIME_DP         51
#define CLOCK_TIME_BASELINE_DP (208.5 / 3.0)   /* below the widget cell's top */
#define CLOCK_DATE_DP         9
#define CLOCK_DATE_BASELINE_DP (247 / 3.0)
#define CLOCK_TIME_RGB        0xE0E0E0      /* a soft grey-white, the owner's choice over the accent (0xA9EEF8) */

static struct {
  /* geometry */
  float cell_x0, cell_w, cell_y0, cell_h, icon, icon_top, dock_top, dock_icon_top, dots_y;

  /* paging */
  float       scroll;              /* px: page × W at rest */
  int         page;
  bool        down, dragging, vertical, gave_up;
  int         touch_page;
  float       down_x, down_y, last_x, scroll_down;
  SpongyVelocity vel;
  SpongyEdge  left, right;
  float       stretch;             /* this frame's edge stretch amount */
  bool        snapping;
  double      snap_t0, snap_ms;
  float       snap_from, snap_to;

  /* the clock widget */
  TextFont   *time_font, *date_font;
  TextLayout *time, *date;
} H;

static int
n_pages (void)
{
  return MAX (1, (int) N.model->pages->len);
}

void
home_layout (void)
{
  H.cell_x0 = dp (WS_SIDE_DP);
  H.cell_w = (N.W - 2 * H.cell_x0) / GRID_COLS;
  H.cell_y0 = roundf (dp (STATUS_BAR_DP + WS_TOP_DP));
  float bottom = N.H - dp (HOTSEAT_DP) - dp (PAGE_IND_DP);
  H.cell_h = floorf ((bottom - H.cell_y0) / GRID_ROWS);
  H.icon = roundf (dp (ICON_DP));
  H.icon_top = floorf ((H.cell_h - dp (WS_CONTENT_DP)) / 2);
  H.dock_top = N.H - dp (HOTSEAT_DP);
  H.dock_icon_top = H.dock_top + floorf ((dp (HOTSEAT_CELL_DP) - H.icon) / 2);
  H.dots_y = bottom + dp (PAGE_IND_DP) / 2;
  spongy_edge_init (&H.left, N.W);
  spongy_edge_init (&H.right, N.W);
  H.page = CLAMP (H.page, 0, n_pages () - 1);
  H.scroll = H.page * N.W;
  if (!H.time_font) {
    H.time_font = text_font_axes ("Google Sans Flex", 600, dp (CLOCK_TIME_DP), "opsz=144,wdth=86");
    H.date_font = text_font_axes ("Google Sans Flex", 700, dp (CLOCK_DATE_DP), "opsz=18,ROND=100");
  }
  home_clock_tick ();
}

void
home_clock_tick (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *time = g_date_time_format (now, "%H:%M");
  g_autofree char *date = g_date_time_format (now, "%a, %b %-d");
  text_layout_free (H.time);
  text_layout_free (H.date);
  H.time = text_layout (H.time_font, time, 0, 1, TEXT_CENTER);
  H.date = text_layout (H.date_font, date, 0, 1, TEXT_CENTER);
  wl_schedule (N.wl);
}

/* ---- where things are ----------------------------------------------------------------------------------------- */

/* the icon box of a page's slot (page coordinates: page p starts at x = p·W) */
static void
slot_rect (int page, int slot, float *x, float *y)
{
  if (page == 0)
    slot += N.model->reserved;
  int col = slot % GRID_COLS, row = slot / GRID_COLS;
  *x = roundf (page * N.W + H.cell_x0 + col * H.cell_w + (H.cell_w - H.icon) / 2);
  *y = roundf (H.cell_y0 + row * H.cell_h + H.icon_top);
}

static void
dock_rect (int slot, float *x, float *y)
{
  *x = roundf (H.cell_x0 + slot * H.cell_w + (H.cell_w - H.icon) / 2);
  *y = roundf (H.dock_icon_top);
}

Tile *
home_tile_at (float x, float y, float *ix, float *iy)
{
  for (guint i = 0; i < N.model->dock->len; i++) {
    float rx, ry;
    dock_rect (i, &rx, &ry);
    if (x >= rx - (H.cell_w - H.icon) / 2 && x < rx + H.icon + (H.cell_w - H.icon) / 2 && y >= H.dock_top &&
        y < H.dock_top + dp (HOTSEAT_CELL_DP)) {
      *ix = rx;
      *iy = ry;
      return tiles_get (N.model->dock->pdata[i]);
    }
  }
  int page = (int) floorf ((x + H.scroll) / N.W);
  if (page < 0 || page >= n_pages ())
    return NULL;
  GPtrArray *items = N.model->pages->pdata[page];
  for (guint i = 0; i < items->len; i++) {
    float rx, ry;
    slot_rect (page, i, &rx, &ry);
    rx -= H.scroll;
    /* the icon and its label, Launcher3's BubbleTextView box */
    if (x >= rx - dp (8) && x < rx + H.icon + dp (8) && y >= ry - dp (8) && y < ry + dp (LABEL_BASELINE_DP + 8)) {
      *ix = rx;
      *iy = ry;
      return tiles_get (items->pdata[i]);
    }
  }
  return NULL;
}

bool
home_icon_rect (const char *id, float r[4])
{
  for (guint i = 0; i < N.model->dock->len; i++)
    if (!g_strcmp0 (N.model->dock->pdata[i], id)) {
      dock_rect (i, &r[0], &r[1]);
      r[2] = r[3] = H.icon;
      return true;
    }
  GPtrArray *items = N.model->pages->pdata[H.page];
  for (guint i = 0; i < items->len; i++)
    if (!g_strcmp0 (items->pdata[i], id)) {
      /* where it rests once the page has settled: the shell reads this 100 ms after a swipe, mid-snap */
      slot_rect (H.page, i, &r[0], &r[1]);
      r[0] -= H.page * N.W;
      r[2] = r[3] = H.icon;
      return true;
    }
  return false;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
draw_icon_and_label (Tile *t, float x, float y, bool label)
{
  if (!t)
    return;
  tile_draw_icon (t, x, y, H.icon, 1);
  if (label) {
    float lw = dp (LABEL_HOME_MAX_DP);
    label_draw (t->label, x + H.icon / 2 - lw / 2, y + dp (LABEL_BASELINE_DP) - text_ascent (N.label_font),
                (GfxColor) { 1, 1, 1, 1 }, true);
  }
}

static void
draw_clock (float x0)
{
  float top = H.cell_y0, cx = x0 + N.W / 2;
  if (H.time)
    text_draw (H.time, cx, top + dp (CLOCK_TIME_BASELINE_DP) - text_ascent (H.time_font),
               GFX_RGBA (CLOCK_TIME_RGB, 1));
  if (H.date)
    label_draw (H.date, cx, top + dp (CLOCK_DATE_BASELINE_DP) - text_ascent (H.date_font),
                (GfxColor) { 1, 1, 1, 1 }, false);
}

static void
draw_dots (void)
{
  int n = n_pages ();
  if (n < 2)
    return;
  float d = dp (PAGE_DOT_DP), pitch = d + dp (PAGE_DOT_GAP_DP);
  float x0 = N.W / 2 - (n * pitch - dp (PAGE_DOT_GAP_DP)) / 2;
  for (int i = 0; i < n; i++)
    gfx_rect (x0 + i * pitch, H.dots_y - d / 2, d, d, d / 2, (GfxColor) { 1, 1, 1, 0.5f });
  float at = CLAMP (H.scroll / N.W, 0, n - 1);
  gfx_rect (x0 + at * pitch, H.dots_y - d / 2, d, d, d / 2, (GfxColor) { 1, 1, 1, 1 });
}

void
home_draw (float content_scale)
{
  gfx_save ();
  if (content_scale != 1)
    gfx_scale (content_scale, content_scale, N.W / 2.0f, N.H / 2.0f);
  int first = MAX (0, (int) floorf (H.scroll / N.W)), last = MIN (n_pages () - 1, (int) ceilf (H.scroll / N.W));
  gfx_save ();
  gfx_translate (-H.scroll, 0);
  for (int p = first; p <= last; p++) {
    if (p == 0)
      draw_clock (0);
    GPtrArray *items = N.model->pages->pdata[p];
    for (guint i = 0; i < items->len; i++) {
      float x, y;
      slot_rect (p, i, &x, &y);
      draw_icon_and_label (tiles_get (items->pdata[i]), x, y, true);
    }
  }
  gfx_restore ();
  draw_dots ();
  for (guint i = 0; i < N.model->dock->len; i++) {
    float x, y;
    dock_rect (i, &x, &y);
    draw_icon_and_label (tiles_get (N.model->dock->pdata[i]), x, y, false);
  }
  gfx_restore ();
}

/* ---- paging --------------------------------------------------------------------------------------------------- */

static void
snap_to (int page, float velocity)
{
  page = CLAMP (page, 0, n_pages () - 1);
  float to = page * N.W, delta = to - H.scroll;
  if (page != H.page)
    nav_icons_changed ();
  H.page = page;
  if (fabsf (delta) < 0.5f) {
    H.scroll = to;
    H.snapping = false;
    return;
  }
  double ms = SNAP_MS;
  float v = fabsf (velocity);
  if (v >= dp (MIN_FLING_DP)) {
    /* snapToPageWithVelocity: the distance through a sine so a short swipe isn't slow */
    float r = MIN (1, fabsf (delta) / N.W);
    float dist = N.W / 2.0f + N.W / 2.0f * sinf ((r - 0.5f) * 0.3f * (float) G_PI / 2);
    ms = 4 * lroundf (1000 * dist / MAX (dp (MIN_SNAP_DP), v));
  }
  H.snapping = true;
  H.snap_t0 = N.now;
  H.snap_ms = ms;
  H.snap_from = H.scroll;
  H.snap_to = to;
  wl_schedule (N.wl);
}

bool
home_frame (void)
{
  bool more = false;
  if (H.snapping) {
    float t = CLAMP ((N.now - H.snap_t0) / H.snap_ms, 0, 1);
    H.scroll = H.snap_from + (H.snap_to - H.snap_from) * spongy_curve (SPONGY_DECELERATE_QUINT, t);
    H.snapping = t < 1;
    more |= H.snapping;
  }
  bool a = false, b = false;
  H.stretch = spongy_edge_frame (&H.left, N.now, false, &a) + spongy_edge_frame (&H.right, N.now, true, &b);
  return more || a || b || press_animating ();
}

float
home_stretch (void)
{
  return H.stretch;
}

bool
home_touch_down (float x, float y, double t)
{
  H.down = true;
  H.dragging = H.vertical = H.gave_up = false;
  H.down_x = H.last_x = x;
  H.down_y = y;
  spongy_velocity_clear (&H.vel);
  spongy_velocity_add (&H.vel, t, x, y);
  /* a finger stops a snap where it is, and catches a receding stretch */
  if (H.snapping) {
    H.snapping = false;
    H.dragging = true;
  }
  H.scroll_down = H.scroll;
  if (!spongy_edge_is_finished (&H.left))
    spongy_edge_pull_distance (&H.left, 0, t);
  if (!spongy_edge_is_finished (&H.right))
    spongy_edge_pull_distance (&H.right, 0, t);
  return true;
}

void
home_touch_move (float x, float y, double t, bool *vertical)
{
  *vertical = false;
  if (!H.down || H.gave_up)
    return;
  spongy_velocity_add (&H.vel, t, x, y);
  float dx = x - H.down_x, dy = y - H.down_y;
  if (!H.dragging) {
    float adx = fabsf (dx), ady = fabsf (dy);
    if (ady > dp (TOUCH_SLOP_DP) && ady > adx) {
      /* up: the drawer; down: the shade (the shell's) */
      H.gave_up = true;
      *vertical = true;
      return;
    }
    double angle = atan2 (ady, adx);
    if (angle > MAX_ANGLE)
      return;
    float slop = dp (PAGING_SLOP_DP);
    if (angle > DAMP_ANGLE)
      slop *= 1 + 4 * sqrtf ((angle - DAMP_ANGLE) / DAMP_ANGLE);
    if (adx <= slop)
      return;
    H.dragging = true;
    H.last_x = x;
    press_up ();
    return;
  }
  float delta = H.last_x - x;
  H.last_x = x;
  float max = (n_pages () - 1) * (float) N.W;
  float next = H.scroll + delta;
  /* a stretch showing shrinks first when the finger goes back */
  if (!spongy_edge_is_finished (&H.left) && delta > 0) {
    float took = spongy_edge_pull_distance (&H.left, -delta / N.W, t);
    next = H.scroll + delta + took * N.W;
  } else if (!spongy_edge_is_finished (&H.right) && delta < 0) {
    float took = spongy_edge_pull_distance (&H.right, delta / N.W, t);
    next = H.scroll + delta - took * N.W;
  }
  if (next < 0) {
    spongy_edge_pull_distance (&H.left, -next / N.W, t);
    next = 0;
  } else if (next > max) {
    spongy_edge_pull_distance (&H.right, (next - max) / N.W, t);
    next = max;
  }
  H.scroll = next;
  wl_schedule (N.wl);
}

void
home_touch_up (float x, float y, double t)
{
  if (!H.down)
    return;
  H.down = false;
  spongy_edge_release (&H.left, t);
  spongy_edge_release (&H.right, t);
  if (!H.dragging) {
    if (H.scroll != H.page * N.W)
      snap_to (lroundf (H.scroll / N.W), 0);
    return;
  }
  H.dragging = false;
  float vx, vy;
  spongy_velocity_get (&H.vel, t, &vx, &vy);
  vx = CLAMP (vx, -dp (8000), dp (8000));
  float delta = x - H.down_x;
  int page = (int) floorf (H.scroll_down / N.W + 0.5f);
  bool fling = fabsf (delta) > dp (PAGING_SLOP_DP) && fabsf (vx) > dp (FLING_PX_S_DP);
  bool significant = fabsf (delta) > N.W * SIGNIFICANT_MOVE;
  if ((significant && !fling && delta > 0) || (fling && vx > 0))
    snap_to (page - 1, vx);
  else if ((significant && !fling && delta < 0) || (fling && vx < 0))
    snap_to (page + 1, vx);
  else
    snap_to (lroundf (H.scroll / N.W), 0);
  wl_schedule (N.wl);
}

void
home_touch_cancel (double t)
{
  H.down = false;
  H.dragging = false;
  spongy_edge_release (&H.left, t);
  spongy_edge_release (&H.right, t);
  snap_to (lroundf (H.scroll / N.W), 0);
}
