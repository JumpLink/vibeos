/* The app drawer: Launcher3's all-apps bottom sheet. Its open fraction f follows the finger 1:1 after the slop
 * (AbstractStateChangeTouchController); a release flings (> 1 dp/ms) or commits at f = 0.4 opening / 0.6 closing and
 * settles in max(100, 1200/max(2, |v|/2)·max(0.2, rest)) ms, quintic for fast flings, cubic otherwise. Every settle
 * into the open sheet starts with a stretch bump of the whole container; the list inside scrolls and stretches the
 * RecyclerView way (libspongy). A swipe up from the bar while it is open goes home (NavBarToHomeTouchController):
 * the sheet shrinks towards 0.9 on DECELERATE_3 of the swipe over the screen's height, and the release closes it
 * past half of that or on an upward fling, else it grows back in 80 ms. A tile opens on a tap only: a finger that
 * moved past the slop, or that stopped a fling, opens nothing. */
#include <librsvg/rsvg.h>
#include <math.h>

#include "image.h"
#include "neo.h"

#define SLOP_DP              8
#define FLING_PX_MS_DP       1.0          /* 1 dp/ms */
#define FAST_FLING_PX_MS_DP  (10 / 3.0)
#define OPEN_THRESHOLD       0.4f
#define SETTLE_BASE_MS       1200.0
#define SETTLE_MIN_MS        100.0
#define SINGLE_FRAME_MS      16.0
#define PULL_MULTIPLIER      0.02f
#define FLING_VELOCITY_MULTIPLIER 1200.0f
#define MIN_ABSORB           1000
#define INTERP_COEFF         1.7f
#define PROG_CLOSE_MS        200.0        /* back, a launch: ACCELERATE */
#define HOME_MIN_SCALE       0.9f         /* PREDICTIVE_BACK_MIN_SCALE */
#define HOME_SUCCESS         0.5f         /* SUCCESS_TRANSITION_PROGRESS, of DECELERATE_3 */
#define HOME_REVERT_MS       80.0

#define SHEET_RGB            0x082D31     /* over the blur at 0.54, measured (18, 38, 42) on the S10e */
#define SHEET_ALPHA          0.54f
#define HANDLE_RGB           0xDDE7E9
#define SEARCH_RGB           0x121F21
#define HINT_RGB             0xA2ADAF
#define LABEL_RGB            0xDDE7E9
#define THUMB_RGB            0xB8EBF3

/* Material "search" (Apache 2.0), 24 × 24 */
static const char SEARCH_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='#fff' d='M15.5 14h-.79l-.28-.27C15.41 "
  "12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 "
  "4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z'/></svg>";

static struct {
  float       f;                   /* 0 home .. 1 open */
  /* the settle */
  bool        settling;
  double      t0, ms;
  float       from, to;
  SpongyCurve curve;
  /* the state drag */
  bool        down, dragging, decided, opening, overshot, list_touch, catch_anim;
  bool        moved;               /* past the slop: no tap */
  float       down_x, down_y, f_start;
  SpongyVelocity vel;
  /* the swipe up from the bar (home): its progress over the screen's height, and the way back */
  bool        home_down, home_dragging;
  float       home_x0, home_y0, home_p;
  double      revert_t0;           /* 0: none */
  float       revert_from;
  /* the container's own stretch (the bottom edge: over-drag past open and the settle bump) */
  SpongyEdge  bump;
  float       bump_amount;
  /* the list */
  SpongyList  list;
  float       list_top_amount, list_bottom_amount;
  float       list_h, cols_x0, col_w, icon;
  /* drawing */
  GfxTex     *search_icon;
  TextLayout *hint;
  GfxTex     *list_target, *box_target;
} D;

static float
status_px (void)
{
  return roundf (dp (STATUS_BAR_DP));
}

static float
sheet_top (void)
{
  return status_px () + N.H * (1 - D.f);
}

float
drawer_progress (void)
{
  return D.f;
}

bool
drawer_is_open (void)
{
  return D.f > 0.5f || (D.settling && D.to == 1);
}

bool
drawer_dragging (void)
{
  return D.dragging;
}

void
drawer_layout (void)
{
  D.list_h = N.H - status_px () - dp (LIST_TOP_DP);
  D.cols_x0 = dp (WS_SIDE_DP);
  D.col_w = (N.W - 2 * D.cols_x0) / GRID_COLS;
  D.icon = roundf (dp (ICON_DP));
  int rows = (N.sorted->len + GRID_COLS - 1) / GRID_COLS;
  float content = dp (LIST_ICON_TOP_DP) + rows * dp (LIST_ROW_DP);
  if (D.list.view_height == 0)
    spongy_list_init (&D.list, D.list_h, content);
  else
    spongy_list_set_size (&D.list, D.list_h, content);
  spongy_edge_init (&D.bump, N.H - status_px ());
  if (!D.hint) {
    TextFont *font = text_font_axes ("Google Sans Flex", 600, dp (SEARCH_TEXT_SP), "opsz=18");
    D.hint = text_layout (font, "Search apps", 0, 1, TEXT_LEFT);
    int px = (int) lroundf (dp (24));
    RsvgHandle *h = rsvg_handle_new_from_data ((const guint8 *) SEARCH_SVG, sizeof SEARCH_SVG - 1, NULL);
    if (h) {
      cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
      cairo_t *cr = cairo_create (s);
      RsvgRectangle vp = { 0, 0, px, px };
      rsvg_handle_render_document (h, cr, &vp, NULL);
      cairo_destroy (cr);
      cairo_surface_flush (s);
      D.search_icon = gfx_tex_from_bgra (px, px, cairo_image_surface_get_data (s), cairo_image_surface_get_stride (s));
      cairo_surface_destroy (s);
      g_object_unref (h);
    }
  }
}

/* ---- the settle ----------------------------------------------------------------------------------------------- */

static void
settle (float to, float v_px_ms)
{
  /* progress of the transition being finished, in its own direction */
  bool open = to == 1;
  float rest = open ? 1 - D.f : D.f;
  float av = fabsf (v_px_ms);
  double ms = MAX (SETTLE_MIN_MS, SETTLE_BASE_MS / MAX (2, av / 2) * MAX (0.2, rest));
  float start = D.f + (open ? 1 : -1) * av * SINGLE_FRAME_MS / N.H;   /* one frame ahead */
  D.from = CLAMP (start, 0, 1);
  D.to = to;
  D.t0 = N.now;
  D.ms = ms;
  D.curve = av > dp (FAST_FLING_PX_MS_DP) ? SPONGY_DECELERATE_QUINT : SPONGY_DECELERATE_CUBIC;
  D.settling = true;
  if (open) {
    if (D.overshot) {
      spongy_edge_release (&D.bump, N.now);
    } else {
      /* the container's bump: at least 13 × 1000 px/s into its bottom edge */
      float s = MIN (0, (1 - D.f) * (N.H - status_px ()) / (INTERP_COEFF * ms) + v_px_ms);
      int v = MAX (MIN_ABSORB, abs ((int) lroundf (s * FLING_VELOCITY_MULTIPLIER)));
      spongy_edge_absorb (&D.bump, v, N.now);
    }
  }
  D.overshot = false;
  wl_schedule (N.wl);
}

void
drawer_close_now (void)
{
  D.settling = false;
  D.f = 0;
  D.home_p = 0;
  D.revert_t0 = 0;
  spongy_list_scroll_to (&D.list, 0);
  spongy_edge_finish (&D.bump);
}

/* back with the drawer open, or home from the bar: the programmatic close, 200 ms on ACCELERATE (back comes over
 * D-Bus, between frames) */
void
drawer_back (void)
{
  if (D.f <= 0)
    return;
  N.now = MAX (N.now, g_get_monotonic_time () / 1000.0);
  D.revert_t0 = 0;
  D.from = D.f;
  D.to = 0;
  D.t0 = N.now;
  D.ms = PROG_CLOSE_MS;
  D.curve = SPONGY_ACCELERATE;
  D.settling = true;
  D.overshot = false;
  wl_schedule (N.wl);
}

/* a drag took an icon out: the sheet slides away in 150 ms (DECELERATE_1_7 in Launcher3, 1.5 here) */
void
drawer_close_for_drag (void)
{
  if (D.f <= 0)
    return;
  D.down = D.dragging = false;
  D.revert_t0 = 0;
  D.from = D.f;
  D.to = 0;
  D.t0 = N.now;
  D.ms = 150;
  D.curve = SPONGY_DECELERATE_CUBIC;
  D.settling = true;
  D.overshot = false;
  wl_schedule (N.wl);
}

void
drawer_open (void)
{
  if (D.f >= 1 && !D.settling)
    return;
  N.now = MAX (N.now, g_get_monotonic_time () / 1000.0);
  settle (1, 0);
}

bool
drawer_frame (void)
{
  bool more = false;
  if (D.settling) {
    float t = CLAMP ((N.now - D.t0) / D.ms, 0, 1);
    D.f = D.from + (D.to - D.from) * spongy_curve (D.curve, t);
    if (t >= 1) {
      D.settling = false;
      D.f = D.to;
      if (D.f == 0) {
        spongy_list_scroll_to (&D.list, 0);
        D.home_p = 0;               /* the sheet's scale goes back to 1 once it is gone */
      }
    }
    more = true;
  }
  if (D.revert_t0) {
    float t = CLAMP ((N.now - D.revert_t0) / HOME_REVERT_MS, 0, 1);
    D.home_p = D.revert_from * (1 - t);
    if (t >= 1)
      D.revert_t0 = 0;
    more = true;
  }
  bool a = false;
  D.bump_amount = spongy_edge_frame (&D.bump, N.now, true, &a);
  more |= a;
  if (D.f > 0)
    more |= spongy_list_frame (&D.list, N.now, &D.list_top_amount, &D.list_bottom_amount);
  return more;
}

/* ---- touch ---------------------------------------------------------------------------------------------------- */

static void
drag_to (float y, double t)
{
  /* past the slop the displacement counts from the slop's edge (BaseSwipeDetector), 1:1 with the finger */
  float disp = y - D.down_y;
  if (!D.catch_anim)
    disp += D.opening ? dp (SLOP_DP) : -dp (SLOP_DP);
  float f = D.f_start - disp / N.H;
  if (f > 1) {
    /* over-drag past open: Launcher3 feeds the whole overshoot × 0.02 to the bottom edge on every move */
    spongy_edge_pull (&D.bump, PULL_MULTIPLIER * (f - 1), t);
    D.overshot = true;
    f = 1;
  }
  D.f = MAX (0, f);
  wl_schedule (N.wl);
}

void
drawer_drag_begin (float y_down, float y, double t)
{
  D.down = D.dragging = D.decided = true;
  D.list_touch = false;
  D.opening = true;
  D.catch_anim = D.settling;
  D.settling = false;
  D.f_start = D.f;
  D.down_y = y_down;
  D.overshot = false;
  spongy_velocity_clear (&D.vel);
  spongy_velocity_add (&D.vel, t, 0, y);
  drag_to (y, t);
}

bool
drawer_touch_down (float x, float y, double t)
{
  if (D.f == 0 && !D.settling)
    return false;
  D.down = true;
  D.down_x = x;
  D.down_y = y;
  D.dragging = D.decided = D.moved = false;
  D.overshot = false;
  D.opening = false;
  spongy_velocity_clear (&D.vel);
  spongy_velocity_add (&D.vel, t, x, y);
  if (D.settling) {
    /* caught mid-settle: the drag goes on from here, no slop */
    D.settling = false;
    D.catch_anim = true;
    D.dragging = D.decided = true;
    D.f_start = D.f;
    return true;
  }
  D.catch_anim = false;
  D.f_start = D.f;
  D.list_touch = y >= sheet_top () + dp (LIST_TOP_DP);
  if (D.list_touch)
    spongy_list_down (&D.list, y, t);
  return true;
}

void
drawer_touch_move (float x, float y, double t)
{
  if (!D.down)
    return;
  spongy_velocity_add (&D.vel, t, x, y);
  if (D.dragging) {
    drag_to (y, t);
    return;
  }
  float dx = x - D.down_x, dy = y - D.down_y;
  if (hypotf (dx, dy) > dp (SLOP_DP))
    D.moved = true;
  /* sideways past the slop first: not the list's (a back swipe from the edge) */
  if (D.list_touch && !D.decided && !D.list.dragging && fabsf (dx) > dp (SLOP_DP) && fabsf (dx) > fabsf (dy)) {
    spongy_list_cancel (&D.list, t);
    D.list_touch = false;
    D.decided = true;
    wl_schedule (N.wl);
    return;
  }
  if (!D.decided) {
    /* down: closing, when the list is at its top or the finger is on the handle/search */
    bool at_top = !D.list_touch || D.list.offset <= 0;
    if (dy > dp (SLOP_DP) && at_top && fabsf (dy) > fabsf (x - D.down_x)) {
      D.decided = D.dragging = true;
      if (D.list_touch)
        spongy_list_cancel (&D.list, t);
      D.list_touch = false;
      drag_to (y, t);
      return;
    }
  }
  if (D.list_touch) {
    if (spongy_list_move (&D.list, y, t))
      D.decided = true;
    wl_schedule (N.wl);
  }
}

/* the drawer tile under a point, and its icon box */
static Tile *
tile_at (float x, float y, float *ix, float *iy)
{
  float top = sheet_top () + dp (LIST_TOP_DP);
  if (y < top)
    return NULL;
  float ly = y - top + roundf (D.list.offset) - dp (LIST_ICON_TOP_DP);
  int row = (int) floorf (ly / dp (LIST_ROW_DP)), col = (int) floorf ((x - D.cols_x0) / D.col_w);
  if (ly < -dp (8) || col < 0 || col >= GRID_COLS)
    return NULL;
  row = MAX (row, 0);
  float in_row = ly - row * dp (LIST_ROW_DP);
  if (in_row > dp (LIST_LABEL_BASELINE_DP + 8))
    return NULL;
  guint i = row * GRID_COLS + col;
  if (i >= N.sorted->len)
    return NULL;
  *ix = roundf (D.cols_x0 + col * D.col_w + (D.col_w - D.icon) / 2);
  *iy = roundf (top + dp (LIST_ICON_TOP_DP) + row * dp (LIST_ROW_DP) - roundf (D.list.offset));
  return N.sorted->pdata[i];
}

Tile *
drawer_hold_tile (float x, float y, float *ix, float *iy)
{
  if (!D.down || !D.list_touch || D.decided || D.dragging || D.list.dragging || D.settling || D.f < 1)
    return NULL;
  return tile_at (x, y, ix, iy);
}

void
drawer_touch_up (float x, float y, double t)
{
  if (!D.down)
    return;
  D.down = false;
  if (D.dragging) {
    D.dragging = false;
    float vx, vy;
    spongy_velocity_get (&D.vel, t, &vx, &vy);
    float v = vy / 1000;                       /* px/ms, + down */
    bool fling = fabsf (v) > dp (FLING_PX_MS_DP);
    /* opening commits past 0.4, closing past 0.6 of its own way: the same line, f = 0.4 */
    float to = fling ? (v < 0 ? 1 : 0) : (D.f > OPEN_THRESHOLD ? 1 : 0);
    settle (to, v);
    return;
  }
  if (D.list_touch) {
    bool tap = !D.list.dragging && !D.decided && !D.moved;
    spongy_list_up (&D.list, t);
    float ix, iy;
    Tile *tile = tap ? tile_at (D.down_x, D.down_y, &ix, &iy) : NULL;
    if (tile)
      tile_launch (tile, ix, iy, D.icon, true);
  }
  wl_schedule (N.wl);
}

/* ---- home from the bar ---------------------------------------------------------------------------------------- */

static float
decelerate3 (float t)
{
  return 1 - powf (1 - t, 6);
}

bool
drawer_home_down (float x, float y, double t)
{
  if (!drawer_is_open () || D.down)
    return false;
  D.home_down = true;
  D.home_dragging = false;
  D.home_x0 = x;
  D.home_y0 = y;
  D.revert_t0 = 0;
  spongy_velocity_clear (&D.vel);
  spongy_velocity_add (&D.vel, t, x, y);
  return true;
}

void
drawer_home_move (float x, float y, double t)
{
  if (!D.home_down)
    return;
  spongy_velocity_add (&D.vel, t, x, y);
  float up = D.home_y0 - y;
  if (!D.home_dragging) {
    /* SingleAxisSwipeDetector, up only: along past the slop and past the across */
    if (up < MAX (dp (SLOP_DP), fabsf (x - D.home_x0)))
      return;
    D.home_dragging = true;
    D.home_y0 -= dp (SLOP_DP);        /* from the slop's edge on */
    up = D.home_y0 - y;
  }
  D.home_p = CLAMP (up / N.H, 0, 1);
  wl_schedule (N.wl);
}

bool
drawer_home_up (float x, float y, double t)
{
  if (!D.home_down)
    return false;
  D.home_down = false;
  if (!D.home_dragging)
    return false;
  D.home_dragging = false;
  float vx, vy;
  spongy_velocity_get (&D.vel, t, &vx, &vy);
  float v = vy / 1000;                       /* px/ms, + down */
  bool home = decelerate3 (D.home_p) >= HOME_SUCCESS || (v < 0 && fabsf (v) > dp (FLING_PX_MS_DP));
  if (home) {
    drawer_back ();
  } else {
    D.revert_from = D.home_p;
    D.revert_t0 = N.now;
  }
  wl_schedule (N.wl);
  return home;
}

void
drawer_home_cancel (double t)
{
  if (!D.home_down)
    return;
  D.home_down = D.home_dragging = false;
  D.revert_from = D.home_p;
  D.revert_t0 = N.now;
  wl_schedule (N.wl);
}

void
drawer_touch_cancel (double t)
{
  if (D.list_touch)
    spongy_list_cancel (&D.list, t);
  if (D.dragging)
    settle (D.f > OPEN_THRESHOLD ? 1 : 0, 0);
  D.down = D.dragging = false;
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

static void
tinted (GfxTex *tex, float x, float y, float w, float h, GfxColor c)
{
  GfxInst *q = gfx_push (tex, GFX_PIPE_QUAD);
  float a = q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u1 = q->v1 = 1;
  q->r = c.r * c.a * a; q->g = c.g * c.a * a; q->b = c.b * c.a * a; q->a = c.a * a;
  q->mode = GFX_MODE_TEX;
}

/* the list's rows from `top` (screen y of the list window's top) */
static void
draw_list (float top, float view_h)
{
  float off = roundf (D.list.offset), row_h = dp (LIST_ROW_DP), first_icon = dp (LIST_ICON_TOP_DP);
  int first = MAX (0, (int) floorf ((off - first_icon - dp (LIST_LABEL_BASELINE_DP)) / row_h)) * GRID_COLS;
  int last = MIN ((int) N.sorted->len, ((int) ceilf ((off + view_h) / row_h) + 1) * GRID_COLS);
  float lw = dp (LABEL_DRAWER_MAX_DP);
  for (int i = first; i < last; i++) {
    Tile *t = N.sorted->pdata[i];
    float x = roundf (D.cols_x0 + (i % GRID_COLS) * D.col_w + (D.col_w - D.icon) / 2);
    float y = roundf (top + first_icon + (i / GRID_COLS) * row_h - off);
    if (t != drag_drawer_tile ())   /* held or dragged out: the drag view is it */
      tile_draw_icon (t, x, y, D.icon, 1);
    text_draw (t->dlabel, x + D.icon / 2 - lw / 2, y + dp (LIST_LABEL_BASELINE_DP) - text_ascent (N.label_font),
               GFX_RGBA (LABEL_RGB, 1));
  }
}

/* the handle, search pill and list, with the container at y = 0 */
static void
draw_box (bool list_stretched)
{
  float hw = dp (HANDLE_W_DP), hh = dp (HANDLE_H_DP);
  gfx_rect (roundf ((N.W - hw) / 2), dp (HANDLE_TOP_DP), hw, hh, hh / 2, GFX_RGBA (HANDLE_RGB, 1));
  float sx = dp (SEARCH_SIDE_DP), sy = dp (SEARCH_TOP_DP), sh = dp (SEARCH_H_DP);
  gfx_rect (sx, sy, N.W - 2 * sx, sh, sh / 2, GFX_RGBA (SEARCH_RGB, 1));
  float isz = dp (24);
  if (D.search_icon)
    tinted (D.search_icon, sx + dp (12), roundf (sy + (sh - isz) / 2), isz, isz, GFX_RGBA (HINT_RGB, 1));
  float base = sy + sh / 2 + dp (7.6);
  text_draw (D.hint, roundf (sx + dp (12 + 24 + 8) - dp (0.3)), roundf (base - dp (SEARCH_TEXT_SP) * 0.95f),
             GFX_RGBA (HINT_RGB, 1));

  float lt = dp (LIST_TOP_DP);
  if (list_stretched && D.list_target) {
    float p[8] = { 0, D.list_top_amount + D.list_bottom_amount, N.W, D.list_h };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (D.list_target, GFX_PIPE_STRETCH);
    q->x = 0; q->y = lt; q->w = N.W; q->h = D.list_h;
    q->u1 = q->v1 = 1;
    q->r = q->g = q->b = q->a;
    q->mode = GFX_MODE_TEX;
  } else {
    gfx_save ();
    gfx_clip (0, lt, N.W, lt + D.list_h);
    draw_list (lt, D.list_h);
    gfx_restore ();
  }

  /* the fast scroller's thumb */
  if (D.list.max_offset > 0) {
    float th = dp (FASTSCROLL_H_DP), tw = roundf (dp (FASTSCROLL_W_DP));
    float t0 = lt + dp (FASTSCROLL_TOP_DP), t1 = lt + D.list_h - dp (FASTSCROLL_TOP_DP) - th;
    float y = t0 + (t1 - t0) * CLAMP (D.list.offset / D.list.max_offset, 0, 1);
    gfx_rect (N.W - tw - dp (FASTSCROLL_END_DP), roundf (y), tw, th, tw / 2, GFX_RGBA (THUMB_RGB, 1));
  }
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

/* Offscreen passes, before the screen's: the list and the container when they are stretched */
void drawer_prepare (void);
void
drawer_prepare (void)
{
  if (D.f <= 0)
    return;
  bool list_stretched = D.list_top_amount + D.list_bottom_amount != 0;
  if (list_stretched) {
    gfx_pass_begin (target (&D.list_target, N.W, (int) ceilf (D.list_h)), (GfxColor) { 0, 0, 0, 0 });
    draw_list (0, D.list_h);
    gfx_pass_end ();
  }
  if (D.bump_amount != 0) {
    int h = (int) ceilf (N.H - status_px ());
    gfx_pass_begin (target (&D.box_target, N.W, h), (GfxColor) { 0, 0, 0, 0 });
    draw_box (list_stretched);
    gfx_pass_end ();
  }
}

/* the scrim, the sheet and its contents (the screen pass) */
void
drawer_draw (void)
{
  if (D.f <= 0)
    return;
  float scrim = CLAMP ((D.f - 0.1f) / 0.4f, 0, 1) * 0.40f;
  if (scrim > 0)
    gfx_rect (0, 0, N.W, N.H, 0, (GfxColor) { 0x40 / 255.f, 0x40 / 255.f, 0x40 / 255.f, scrim });
  float top = roundf (sheet_top ());
  if (top >= N.H)
    return;
  float r = dp (SHEET_RADIUS_DP);
  float scale = 1 - (1 - HOME_MIN_SCALE) * decelerate3 (D.home_p);
  gfx_save ();
  if (scale != 1)
    gfx_scale (scale, scale, N.W / 2.0f, N.H / 2.0f);   /* the apps view and its header, about their centre */
  gfx_rect (0, top, N.W, N.H - top + r, r, GFX_RGBA (SHEET_RGB, SHEET_ALPHA));
  gfx_save ();
  gfx_translate (0, top);
  if (D.bump_amount != 0 && D.box_target) {
    float p[8] = { 0, D.bump_amount, N.W, D.box_target->h };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (D.box_target, GFX_PIPE_STRETCH);
    q->w = N.W; q->h = D.box_target->h;
    q->u1 = q->v1 = 1;
    q->r = q->g = q->b = q->a;
    q->mode = GFX_MODE_TEX;
  } else {
    draw_box (D.list_top_amount + D.list_bottom_amount != 0);
  }
  gfx_restore ();
  gfx_restore ();
}
