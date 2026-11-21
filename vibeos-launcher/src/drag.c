/* Moving an icon: drag and drop on the home screen, the dock and from the drawer. Lengths are logical px (dp ()).
 *
 *   hold     the long press opens the icon's menu and lifts the icon: the drag view (the icon alone, at the press's
 *            1.10, settling at 1.065 in 150 ms) follows the finger while the icon's own cell shows nothing. Let go
 *            before the finger has gone START_DP and it flies back into its cell, the menu staying open.
 *   drag     past START_DP the menu closes and the workspace zooms out to take the drop in 150 ms: the pages
 *            shrink under the drop bar, each on its card, with an empty page after the last (unless the icon is the
 *            last page's only one); the wallpaper's depth goes to 0.15 (zoomed out, blurred to 45 px) under a
 *            neutral scrim at 0.2, and the drop bar fades in over 175 ms. An icon from the drawer slides the sheet
 *            away in 150 ms, and is a copy.
 *   targets  the bar's Remove (Cancel from the drawer) under the finger once the drag has gone BAR_MIN_PATH_DP;
 *            else the hotseat under the finger; else a neighbouring page when the finger or the drag view's centre
 *            is over the strip of it that shows, the page under the finger otherwise (none over the clock, or off
 *            every page). A neighbour scrolls in after 500 ms.
 *   cells    the cell nearest the drag view's centre. An empty one takes the drop preview (a ring fading in over
 *            500 ms, the last ones fading out) and anything pushed aside goes back. On an occupied one, with the
 *            centre inside the circle round its icon, that icon leans toward where it would go, and 650 ms later
 *            the icons move there (150 ms), each moved one then swinging toward its own cell and back.
 *   drop     on a cell: the view flies into it (100 to 500 ms, by the distance), and the workspace comes back 500 ms
 *            later; on Remove it shrinks into the X in 285 ms and the icon goes; anywhere else it flies home (from
 *            the drawer it is just gone).
 *
 * Left out for now: the "Item removed" snackbar with its Undo, the "No room" toast, Uninstall, the fling to
 * Remove, the drag view's parallax and the moved icons' wiggle scale. */
#include <math.h>
#include <string.h>

#include "vibeos.h"
#include "popup_icons.h"

#define START_DP          16          /* the finger's path that turns a hold into a drag */
#define VIEW_SCALE        1.10f       /* the press's */
#define VIEW_SCALE_TO     1.065f      /* where it settles */
#define VIEW_SCALE_MS     150.0
#define SPRING_MS         150.0
#define EXIT_DELAY_MS     500.0
#define SCRIM_RGB         0x191C1D
#define SCRIM_ALPHA       0.2f

#define BAR_FADE_MS       175.0
#define BAR_TOP_DP        32          /* below the status bar */
#define BAR_H_DP          56
#define BAR_PAD_DP        16
#define BAR_ICON_DP       20
#define BAR_GAP_DP        8
#define BAR_TEXT_SP       16
#define BAR_STROKE_DP     2
#define BAR_HIT_BELOW_DP  20
#define BAR_MIN_PATH_DP   30
#define CAP_HEIGHT        0.711f      /* VibeOS Roboto: capHeight 1456 / 2048 */
#define HOVER_ALPHA       0.65f
#define ACCENT_RGB        0xB8EBF3    /* the drop bar's ink and the drop preview */
#define ON_ACCENT_RGB     0x001F23    /* the ink over a hovered drop target's fill */
#define REMOVE_MS         285.0
#define REMOVE_SCALE      0.1f
#define REMOVE_ALPHA      0.3f

#define PREVIEW_STROKE_PX 8
#define PREVIEW_RADIUS_DP 16
#define PREVIEW_INSET_DP  6
#define PREVIEW_FADE_MS   500.0
#define N_PREVIEWS        4

#define REORDER_MS        650.0       /* held over an icon this long, the icons make room; the hint's lean too */
#define MOVE_MS           150.0       /* the icons moving to make room */
#define BOUNCE_MS         300.0       /* one swing of a moved icon */
#define BOUNCE_DELAY_MS   60
#define BOUNCE_AMP        0.12f       /* the swing, × the icon's size */
#define PAGE_ALARM_MS     500.0
#define PAGE_SCROLL_MS    750.0
#define NEIGHBOUR_DROP_MS 300.0

#define MAX_CELLS         25
#define MAX_ARR           8
#define DOCK              (-1)
#define NOWHERE           (-2)

typedef enum { PHASE_NONE, PHASE_HOLD, PHASE_ACTIVE, PHASE_DONE } Phase;
typedef enum { LAND_NONE, LAND_CELL, LAND_HOME, LAND_REMOVE } Land;

/* An icon's lean (a hint, once) or swing (back and forth), from idx/idy to fdx/fdy */
typedef struct {
  float  fdx, fdy, idx, idy;
  double t0, ms;                      /* t0 0: none */
  bool   repeat;
  double stop_t0;                     /* finishing: back to 0 from stop_p in 150 ms */
  float  stop_p;
} Bounce;

/* a container's icons while they make room: each model cell's icon shows in to[] (itself unless moved aside) */
typedef struct {
  int     c;                          /* a page, DOCK, or NOWHERE: a free slot */
  int     to[MAX_CELLS];
  float   fx[MAX_CELLS], fy[MAX_CELLS];   /* a move's start, as an offset from the model cell */
  double  t0[MAX_CELLS];              /* 0: no move */
  Bounce  b[MAX_CELLS];
} Arr;

typedef struct {
  int    c, i;
  float  from, to;                    /* alpha */
  double t0;
} Preview;

static struct {
  Phase     phase;
  Tile     *tile;
  ModelPos  from;
  int       from_c, from_i;           /* the source as a container and cell; NOWHERE from the drawer */
  bool      from_drawer;
  float     hold_x, hold_y;           /* the icon's box at the hold */
  float     off_x, off_y;             /* the icon's centre from the finger */
  float     x, y, path, snap_path;
  float     size;
  double    t_hold;

  /* the zoomed-out workspace, 0 → 1: the zoom-out curve for the workspace, linear for the scrim */
  bool      sprung;
  float     sp_from, sp_to;
  double    sp_t0;                    /* 0: at sp_to */
  double    exit_at;                  /* the workspace comes back then (0: not yet) */
  float     bar_from, bar_to;
  double    bar_t0;
  bool      hover;

  /* the target */
  int       tc, ti;                   /* the drop layout and the cell under the drag view's centre (-1: none) */
  int       last_i;                   /* the last cell a reorder was asked for */
  bool      reordered;                /* the icons made room at ti */
  double    alarm_at;                 /* the reorder's, 0: none */
  int       page_c;
  double    page_at;                  /* the neighbouring page's, 0: none */

  Arr       arr[MAX_ARR];
  Preview   prev[N_PREVIEWS];
  int       prev_cur;                 /* the slot showing, or -1 */

  /* the landing */
  Land      land;
  int       land_c, land_i;
  bool      flying;
  double    fly_t0, fly_ms;
  float     fly_x, fly_y, fly_s, fly_a;   /* the view when it began: centre, size, alpha */

  TextFont   *font;
  TextLayout *remove, *cancel;
  GfxTex     *glyph;
} G = { .prev_cur = -1 };

/* ---- curves ------------------------------------------------------------------------------------------------------ */

static float
zoom_out (float t)
{
  float x = 1 - t;
  return 1 - (1 - 0.35f / (0.35f + x)) / (1 - 0.35f / 1.35f);
}

static float
decelerate2 (float t)
{
  return 1 - powf (1 - t, 4);
}

static float
progress (double t0, double ms)
{
  return CLAMP ((N.now - t0) / ms, 0, 1);
}

/* ---- the spring-loaded state ------------------------------------------------------------------------------------- */

float
drag_spring (void)
{
  if (!G.sprung)
    return 0;
  if (!G.sp_t0)
    return G.sp_to;
  return G.sp_from + (G.sp_to - G.sp_from) * zoom_out (progress (G.sp_t0, SPRING_MS));
}

static float
spring_linear (void)
{
  if (!G.sprung)
    return 0;
  if (!G.sp_t0)
    return G.sp_to;
  return G.sp_from + (G.sp_to - G.sp_from) * progress (G.sp_t0, SPRING_MS);
}

static void
spring_to (float to)
{
  G.sp_from = drag_spring ();
  G.sp_to = to;
  G.sp_t0 = N.now;
  G.sprung = true;
}

static float
bar_alpha (void)
{
  if (!G.bar_t0)
    return G.bar_to;
  return G.bar_from + (G.bar_to - G.bar_from) * spongy_curve (SPONGY_ACCELERATE, progress (G.bar_t0, BAR_FADE_MS));
}

static void
bar_to (float to)
{
  if (G.bar_to == to)
    return;
  G.bar_from = bar_alpha ();
  G.bar_to = to;
  G.bar_t0 = N.now;
}

/* ---- containers and cells ---------------------------------------------------------------------------------------- */

static ModelPos
cell_pos (int c, int i)
{
  return c == DOCK ? (ModelPos) { i, -1, -1 } : (ModelPos) { -1, c, i };
}

/* the screen in a container's coordinates: a page's are the strip's, the dock's the screen's */
static void
to_cont (int c, float sx, float sy, float *x, float *y)
{
  if (c == DOCK) {
    *x = sx;
    *y = sy;
  } else {
    home_from_screen (sx, sy, x, y);
  }
}

static void
from_cont (int c, float x, float y, float *sx, float *sy)
{
  if (c == DOCK) {
    *sx = x;
    *sy = y;
  } else {
    home_to_screen (x, y, sx, sy);
  }
}

static float
cont_scale (int c)
{
  return c == DOCK ? 1 : home_spring_scale ();
}

/* a cell's icon centre, in its container's coordinates */
static void
icon_centre (int c, int i, float *x, float *y)
{
  home_icon_pos (c, i, x, y);
  *x += G.size / 2;
  *y += G.size / 2;
}

/* an icon at rest there, the one being dragged not counted */
static bool
occupied (int c, int i)
{
  if (c == G.from_c && i == G.from_i)
    return false;
  return model_at (N.model, cell_pos (c, i)) != NULL;
}

static int
nearest (int c, float x, float y)
{
  return c == DOCK ? home_dock_nearest (x) : home_cell_nearest (c, x, y);
}

/* the cell dx, dy away, or -1 off the grid (or in the clock's row) */
static int
cell_step (int c, int i, int dx, int dy)
{
  int col, row;
  home_cell_grid (c, i, &col, &row);
  col += dx;
  row += dy;
  if (c == DOCK)
    return row == 0 && col >= 0 && col < home_cells (DOCK) ? col : -1;
  if (col < 0 || col >= N.model->cols || row < 0 || row >= N.model->rows)
    return -1;
  int k = row * N.model->cols + col - (c == 0 ? N.model->reserved : 0);
  return k >= 0 && k < home_cells (c) ? k : -1;
}

/* the largest circle round the icon's centre inside its cell (getReorderRadius for a folder-capable icon) */
static float
reorder_radius (int c, int i)
{
  float r[4], x, y;
  home_cell_box (c, i, r);
  icon_centre (c, i, &x, &y);
  return MIN (MIN (x - r[0], r[0] + r[2] - x), MIN (y - r[1], r[1] + r[3] - y));
}

/* ---- the icons making room --------------------------------------------------------------------------------------- */

static Arr *
arr_find (int c)
{
  if (G.phase == PHASE_NONE)
    return NULL;
  for (int k = 0; k < MAX_ARR; k++)
    if (G.arr[k].c == c && c != NOWHERE)
      return &G.arr[k];
  return NULL;
}

static void
arr_reset (Arr *a, int c)
{
  *a = (Arr) { .c = c };
  for (int i = 0; i < MAX_CELLS; i++)
    a->to[i] = i;
}

static Arr *
arr_get (int c)
{
  Arr *a = arr_find (c);
  for (int k = 0; !a && k < MAX_ARR; k++)
    if (G.arr[k].c == NOWHERE) {
      a = &G.arr[k];
      arr_reset (a, c);
    }
  return a;
}

static float
bounce_p (const Bounce *b)
{
  if (b->stop_t0)
    return b->stop_p * (1 - spongy_curve (SPONGY_DECELERATE_CUBIC, progress (b->stop_t0, MOVE_MS)));
  if (!b->t0 || N.now < b->t0)
    return 0;
  double e = (N.now - b->t0) / b->ms;
  if (b->repeat) {
    e = fmod (e, 2);
    if (e > 1)
      e = 2 - e;
  } else {
    e = MIN (e, 1);
  }
  return spongy_curve (SPONGY_ACCELERATE_DECELERATE, (float) e);
}

static void
bounce_offset (const Bounce *b, float *dx, float *dy)
{
  float p = bounce_p (b);
  if (b->stop_t0) {
    *dx = p * b->fdx;
    *dy = p * b->fdy;
  } else {
    *dx = p * b->fdx + (1 - p) * b->idx;
    *dy = p * b->fdy + (1 - p) * b->idy;
  }
}

static bool
bounce_active (const Bounce *b)
{
  return b->t0 || b->stop_t0;
}

static void
bounce_finish (Bounce *b)
{
  if (!bounce_active (b) || b->stop_t0)
    return;
  b->stop_p = bounce_p (b);
  b->stop_t0 = N.now;
}

/* the icon of model cell i, its own cell `home`, its new one `to`: a hint leans toward the new cell, a preview
 * swings back toward its own */
static void
bounce_start (Arr *a, int i, int home, int to, bool hint)
{
  Bounce *b = &a->b[i];
  float x0, y0, x1, y1;
  icon_centre (a->c, home, &x0, &y0);
  icon_centre (a->c, to, &x1, &y1);
  float dX = x1 - x0, dY = y1 - y0, mag = BOUNCE_AMP * G.size, dir = hint ? -1 : 1, fdx = 0, fdy = 0;
  if (fabsf (dX) >= 0.5f || fabsf (dY) >= 0.5f) {
    if (fabsf (dY) < 0.5f) {
      fdx = -dir * copysignf (mag, dX);
    } else if (fabsf (dX) < 0.5f) {
      fdy = -dir * copysignf (mag, dY);
    } else {
      float angle = atanf (dY / dX);
      fdx = -dir * copysignf (fabsf (cosf (angle) * mag), dX);
      fdy = -dir * copysignf (fabsf (sinf (angle) * mag), dY);
    }
  }
  if (!fdx && !fdy) {
    bounce_finish (b);
    return;
  }
  float ix, iy;
  bounce_offset (b, &ix, &iy);
  *b = (Bounce) { .fdx = fdx, .fdy = fdy, .idx = ix, .idy = iy, .repeat = !hint,
                  .ms = hint ? REORDER_MS : BOUNCE_MS, .t0 = N.now + g_random_int_range (0, BOUNCE_DELAY_MS) };
}

/* where the icon of model cell i shows now, from its cell, before its bounce */
static void
arr_base (const Arr *a, int i, float *dx, float *dy)
{
  float tx, ty, ox, oy;
  home_icon_pos (a->c, a->to[i], &tx, &ty);
  home_icon_pos (a->c, i, &ox, &oy);
  *dx = tx - ox;
  *dy = ty - oy;
  if (a->t0[i]) {
    float e = spongy_curve (SPONGY_ACCELERATE_DECELERATE, progress (a->t0[i], MOVE_MS));
    *dx = a->fx[i] + (*dx - a->fx[i]) * e;
    *dy = a->fy[i] + (*dy - a->fy[i]) * e;
  }
}

void
drag_offset (int c, int i, float *dx, float *dy)
{
  *dx = *dy = 0;
  Arr *a = arr_find (c);
  if (!a || i < 0 || i >= MAX_CELLS)
    return;
  float bx, by;
  arr_base (a, i, dx, dy);
  bounce_offset (&a->b[i], &bx, &by);
  *dx += bx;
  *dy += by;
}

static void
arr_move (Arr *a, int i, int to)
{
  if (a->to[i] == to)
    return;
  arr_base (a, i, &a->fx[i], &a->fy[i]);
  a->to[i] = to;
  a->t0[i] = N.now;
}

/* revertTempState: the swings stop, the moved icons go home */
static void
revert (int c)
{
  Arr *a = arr_find (c);
  if (!a)
    return;
  for (int i = 0; i < MAX_CELLS; i++) {
    bounce_finish (&a->b[i]);
    arr_move (a, i, i);
  }
}

/* ---- the reorder ------------------------------------------------------------------------------------------------- */

/* from the drag view's centre to the occupied cell's icon: 8 ways, a component counting past 60° */
static void
direction (int c, int t, float x, float y, int *dx, int *dy)
{
  float cx, cy;
  icon_centre (c, t, &cx, &cy);
  double a = atan2 (cy - y, cx - x);
  *dx = fabs (cos (a)) > 0.5 ? (cos (a) > 0 ? 1 : -1) : 0;
  *dy = fabs (sin (a)) > 0.5 ? (sin (a) > 0 ? 1 : -1) : 0;
  if (c == DOCK)
    *dy = 0;
  if (!*dx && !*dy)
    *dx = 1;
}

/* the icon in t and those in a line behind it, one cell along (dx, dy), up to the first free cell */
static bool
push (int c, int t, int dx, int dy, int *sol)
{
  int chain[MAX_CELLS], n = 0, k = t;
  while (k >= 0 && sol[k] >= 0) {
    chain[n++] = k;
    k = cell_step (c, k, dx, dy);
  }
  if (k < 0)
    return false;
  for (int j = n; j-- > 0;) {
    int next = cell_step (c, chain[j], dx, dy);
    sol[next] = sol[chain[j]];
    sol[chain[j]] = -1;
  }
  return true;
}

/* where everything in c goes for the dragged icon to land in t: sol[cell] = the model cell whose icon ends up
 * there (-1 empty), from what is there at rest. A push along d, -d, then the two across, else the icon in t to
 * the nearest free cell (ties to the one most along d). */
static bool
solve (int c, int t, float x, float y, int *sol)
{
  int n = home_cells (c);
  for (int k = 0; k < MAX_CELLS; k++)
    sol[k] = k < n && occupied (c, k) ? k : -1;
  if (sol[t] < 0)
    return true;
  int dx, dy;
  direction (c, t, x, y, &dx, &dy);
  const int dirs[4][2] = { { dx, dy }, { -dx, -dy }, { dy, dx }, { -dy, -dx } };
  for (int d = 0; d < 4; d++) {
    int try[MAX_CELLS];
    memcpy (try, sol, sizeof try);
    if ((dirs[d][0] || dirs[d][1]) && push (c, t, dirs[d][0], dirs[d][1], try)) {
      memcpy (sol, try, sizeof try);
      return true;
    }
  }
  float tx, ty;
  icon_centre (c, t, &tx, &ty);
  int best = -1;
  float best_d = G_MAXFLOAT, best_dot = -G_MAXFLOAT;
  for (int k = 0; k < n; k++) {
    if (k == t || sol[k] >= 0)
      continue;
    float kx, ky;
    icon_centre (c, k, &kx, &ky);
    float d = hypotf (kx - tx, ky - ty), dot = (kx - tx) * dx + (ky - ty) * dy;
    if (d < best_d - 0.5f || (d < best_d + 0.5f && dot > best_dot)) {
      best = k;
      best_d = d;
      best_dot = dot;
    }
  }
  if (best < 0)
    return false;
  sol[best] = sol[t];
  sol[t] = -1;
  return true;
}

/* the inverse: the cell each model cell's icon goes to */
static void
destinations (const int *sol, int *to)
{
  for (int k = 0; k < MAX_CELLS; k++)
    to[k] = k;
  for (int k = 0; k < MAX_CELLS; k++)
    if (sol[k] >= 0)
      to[sol[k]] = k;
}

static void
hint (int c, int t, const int *sol)
{
  Arr *a = arr_get (c);
  int to[MAX_CELLS];
  destinations (sol, to);
  if (a)
    bounce_start (a, t, t, to[t], true);
}

/* the icons go where the solution puts them, and the moved ones swing */
static void
apply (int c, const int *sol)
{
  Arr *a = arr_get (c);
  if (!a)
    return;
  int to[MAX_CELLS];
  destinations (sol, to);
  for (int i = 0; i < home_cells (c); i++)
    if (occupied (c, i)) {
      arr_move (a, i, to[i]);
      bounce_start (a, i, i, to[i], false);
    }
}

/* ---- the drop preview -------------------------------------------------------------------------------------------- */

static float
preview_alpha (const Preview *p)
{
  if (!p->t0)
    return p->to;
  return p->from + (p->to - p->from) * spongy_curve (SPONGY_DECELERATE_QUINT, progress (p->t0, PREVIEW_FADE_MS));
}

static void
preview_fade (Preview *p, float to)
{
  p->from = preview_alpha (p);
  p->to = to;
  p->t0 = N.now;
}

static void
outline_out (void)
{
  if (G.prev_cur >= 0)
    preview_fade (&G.prev[G.prev_cur], 0);
  G.prev_cur = -1;
}

/* visualizeDropLocation: the ring moves to (c, i), the one before fading out */
static void
outline (int c, int i)
{
  if (G.prev_cur >= 0 && G.prev[G.prev_cur].c == c && G.prev[G.prev_cur].i == i)
    return;
  int next = (G.prev_cur + 1 + N_PREVIEWS) % N_PREVIEWS;
  outline_out ();
  Preview *p = &G.prev[next];
  p->from = preview_alpha (p);
  p->c = c;
  p->i = i;
  preview_fade (p, 1);
  G.prev_cur = next;
}

void
drag_draw_cells (int c)
{
  for (int k = 0; k < N_PREVIEWS; k++) {
    Preview *p = &G.prev[k];
    float a = preview_alpha (p);
    if (p->c != c || a <= 0 || G.phase == PHASE_NONE)
      continue;
    float r[4];
    home_cell_box (c, p->i, r);
    float ix = MIN ((r[2] - G.size) / 2, dp (PREVIEW_INSET_DP)), iy = MIN ((r[3] - G.size) / 2, dp (PREVIEW_INSET_DP));
    float half = PREVIEW_STROKE_PX / 2.0f;
    gfx_ring (r[0] + ix - half, r[1] + iy - half, r[2] - 2 * ix + 2 * half, r[3] - 2 * iy + 2 * half,
              dp (PREVIEW_RADIUS_DP) + half, PREVIEW_STROKE_PX, GFX_RGBA (ACCENT_RGB, a));
  }
}

/* ---- the drop bar ------------------------------------------------------------------------------------------------ */

static TextLayout *
bar_label (void)
{
  if (!G.font) {
    G.font = text_font_axes ("VibeOS Roboto", 600, dp (BAR_TEXT_SP), "opsz=16");
    G.remove = text_layout (G.font, "Remove", 0, 1, TEXT_LEFT);
    G.cancel = text_layout (G.font, "Cancel", 0, 1, TEXT_LEFT);
    G.glyph = svg_texture (REMOVE_SVG, (int) lroundf (dp (BAR_ICON_DP)));
  }
  return G.from_drawer ? G.cancel : G.remove;
}

static void
bar_rect (float r[4])
{
  float w = 2 * dp (BAR_PAD_DP) + dp (BAR_ICON_DP) + dp (BAR_GAP_DP) + text_layout_width (bar_label ());
  r[0] = roundf ((N.W - w) / 2);
  r[1] = roundf (dp (N.status_dp + BAR_TOP_DP));
  r[2] = roundf (w);
  r[3] = dp (BAR_H_DP);
}

static bool
in_bar (float x, float y)
{
  float r[4];
  bar_rect (r);
  return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3] + dp (BAR_HIT_BELOW_DP);
}

static void
glyph_centre (float *x, float *y)
{
  float r[4];
  bar_rect (r);
  *x = r[0] + dp (BAR_PAD_DP) + dp (BAR_ICON_DP) / 2;
  *y = r[1] + r[3] / 2;
}

static void
tinted (GfxTex *tex, float x, float y, float s, GfxColor c)
{
  if (!tex)
    return;
  GfxInst *q = gfx_push (tex, GFX_PIPE_QUAD);
  float a = q->a * c.a;
  q->x = x; q->y = y; q->w = s; q->h = s;
  q->u1 = q->v1 = 1;
  q->r = c.r * a; q->g = c.g * a; q->b = c.b * a; q->a = a;
  q->mode = GFX_MODE_TEX;
}

static void
draw_bar (float alpha)
{
  float r[4];
  bar_rect (r);
  /* lit while it is under the finger, and while the icon flies into it */
  bool hover = G.hover && (G.phase == PHASE_ACTIVE || G.land == LAND_REMOVE);
  GfxColor ink = hover ? GFX_RGBA (ON_ACCENT_RGB, alpha) : GFX_RGBA (ACCENT_RGB, alpha);
  if (hover)
    gfx_rect (r[0], r[1], r[2], r[3], r[3] / 2, GFX_RGBA (ACCENT_RGB, alpha));
  else
    gfx_ring (r[0], r[1], r[2], r[3], r[3] / 2, dp (BAR_STROKE_DP), ink);
  float mid = r[1] + r[3] / 2, x = r[0] + dp (BAR_PAD_DP);
  tinted (G.glyph, roundf (x), roundf (mid - dp (BAR_ICON_DP) / 2), dp (BAR_ICON_DP), ink);
  text_draw (bar_label (), x + dp (BAR_ICON_DP + BAR_GAP_DP),
             roundf (mid + CAP_HEIGHT * dp (BAR_TEXT_SP) / 2 - text_ascent (G.font)), ink);
}

/* ---- the drag view ----------------------------------------------------------------------------------------------- */

static void
view_now (float *x, float *y, float *s, float *a)
{
  *x = G.x + G.off_x;
  *y = G.y + G.off_y;
  float e = spongy_curve (SPONGY_ACCELERATE_DECELERATE, progress (G.t_hold, VIEW_SCALE_MS));
  *s = G.size * (VIEW_SCALE + (VIEW_SCALE_TO - VIEW_SCALE) * e);
  *a = G.hover && G.phase == PHASE_ACTIVE ? HOVER_ALPHA : 1;
}

/* where a landing ends, this frame (the workspace may be moving under it) */
static void
fly_dest (float *x, float *y, float *s, float *a)
{
  *a = 1;
  if (G.land == LAND_REMOVE) {
    glyph_centre (x, y);
    *s = G.fly_s * REMOVE_SCALE;
    *a = REMOVE_ALPHA;
  } else if (G.land == LAND_HOME && G.from_drawer) {
    *x = G.hold_x + G.size / 2;
    *y = G.hold_y + G.size / 2;
    *s = G.size;
  } else {
    float cx, cy;
    icon_centre (G.land_c, G.land_i, &cx, &cy);
    from_cont (G.land_c, cx, cy, x, y);
    *s = G.size * cont_scale (G.land_c);
  }
}

static void
fly (Land land, int c, int i)
{
  float x, y, s, a, dx, dy, ds, da;
  view_now (&x, &y, &s, &a);
  G.land = land;
  G.land_c = c;
  G.land_i = i;
  G.fly_x = x;
  G.fly_y = y;
  G.fly_s = s;
  G.fly_a = a;
  fly_dest (&dx, &dy, &ds, &da);
  float d = hypotf (dx - x, dy - y);
  G.fly_ms = land == LAND_REMOVE ? REMOVE_MS
                                 : MAX (100, 500 * spongy_curve (SPONGY_DECELERATE_CUBIC, MIN (d / 800, 1)));
  G.fly_t0 = N.now;
  G.flying = true;
}

static void
draw_view (float x, float y, float s, float a)
{
  Tile *t = G.tile;
  if (t->icon)
    gfx_image (t->icon, x - s / 2, y - s / 2, s, s, 0, a);
  else
    gfx_rect (x - s * 0.46f, y - s * 0.46f, s * 0.92f, s * 0.92f, s * 0.46f, (GfxColor) { 1, 1, 1, 0.18f * a });
}

void
drag_draw_scrim (void)
{
  float q = spring_linear ();
  if (q > 0)
    gfx_rect (0, 0, N.W, N.H, 0, GFX_RGBA (SCRIM_RGB, SCRIM_ALPHA * q));
}

void
drag_draw (void)
{
  if (G.phase == PHASE_NONE)
    return;
  float ba = bar_alpha ();
  if (ba > 0)
    draw_bar (ba);
  float x, y, s, a;
  if (G.phase == PHASE_HOLD || G.phase == PHASE_ACTIVE) {
    view_now (&x, &y, &s, &a);
  } else if (G.flying) {
    float dx, dy, ds, da, t = progress (G.fly_t0, G.fly_ms);
    float e = G.land == LAND_REMOVE ? decelerate2 (t) : spongy_curve (SPONGY_DECELERATE_CUBIC, t);
    fly_dest (&dx, &dy, &ds, &da);
    x = G.fly_x + (dx - G.fly_x) * e;
    y = G.fly_y + (dy - G.fly_y) * e;
    s = G.fly_s + (ds - G.fly_s) * e;
    a = G.fly_a + (da - G.fly_a) * e;
  } else {
    return;
  }
  draw_view (x, y, s, a);
}

/* ---- what the home and the drawer ask ---------------------------------------------------------------------------- */

bool
drag_holding (void)
{
  return G.phase == PHASE_HOLD || G.phase == PHASE_ACTIVE;
}

bool
drag_busy (void)
{
  return G.phase == PHASE_DONE && G.sprung;
}

int
drag_over_page (void)
{
  return G.phase == PHASE_ACTIVE && G.tc >= 0 ? G.tc : -1;
}

bool
drag_hides (int c, int i)
{
  if (drag_holding ())
    return c == G.from_c && i == G.from_i;
  if (G.phase == PHASE_DONE && G.flying && G.land != LAND_REMOVE)
    return c == G.land_c && i == G.land_i;
  if (G.phase == PHASE_DONE && G.flying)
    return c == G.from_c && i == G.from_i;
  return false;
}

Tile *
drag_drawer_tile (void)
{
  if (!G.from_drawer)
    return NULL;
  if (drag_holding () || (G.phase == PHASE_DONE && G.flying && G.land == LAND_HOME))
    return G.tile;
  return NULL;
}

/* ---- the drag ---------------------------------------------------------------------------------------------------- */

static void
clear (void)
{
  for (int k = 0; k < MAX_ARR; k++)
    G.arr[k].c = NOWHERE;
  for (int k = 0; k < N_PREVIEWS; k++)
    G.prev[k] = (Preview) { .c = NOWHERE };
  G.prev_cur = -1;
}

void
drag_hold (Tile *t, ModelPos from, bool from_drawer, float ix, float iy, float down_x, float down_y)
{
  clear ();
  G.phase = PHASE_HOLD;
  G.tile = t;
  G.from = from;
  G.from_drawer = from_drawer;
  G.from_c = from_drawer ? NOWHERE : from.dock >= 0 ? DOCK : from.page;
  G.from_i = from_drawer ? -1 : from.dock >= 0 ? from.dock : from.index;
  G.size = home_icon_size ();
  G.hold_x = ix;
  G.hold_y = iy;
  G.off_x = ix + G.size / 2 - down_x;
  G.off_y = iy + G.size / 2 - down_y;
  G.x = down_x;
  G.y = down_y;
  G.path = G.snap_path = 0;
  G.t_hold = N.now;
  G.sprung = false;
  G.sp_from = G.sp_to = 0;
  G.sp_t0 = G.exit_at = 0;
  G.bar_from = G.bar_to = 0;
  G.bar_t0 = 0;
  G.hover = false;
  G.tc = NOWHERE;
  G.ti = G.last_i = -1;
  G.reordered = false;
  G.alarm_at = G.page_at = 0;
  G.land = LAND_NONE;
  G.flying = false;
  N.press.tile = NULL;              /* the view is the icon now, at the press's scale */
  wl_schedule (N.wl);
}

/* the dragged icon is its page's only one, and that page the last: dragging it off leaves the empty page */
static bool
alone_on_last_page (void)
{
  if (G.from_c < 0 || G.from_c != (int) N.model->pages->len - 1)
    return false;
  GPtrArray *cells = N.model->pages->pdata[G.from_c];
  for (guint i = 0; i < cells->len; i++)
    if (cells->pdata[i] && (int) i != G.from_i)
      return false;
  return true;
}

static void
start (void)
{
  G.phase = PHASE_ACTIVE;
  popup_close ();
  spring_to (1);
  bar_to (1);
  if (!alone_on_last_page ())
    home_set_extra_page (true);
  if (G.from_drawer)
    drawer_close_for_drag ();
}

static bool
inside_page (int p, float x, float y)
{
  if (p < 0 || p >= home_n_pages ())
    return false;
  float r[4];
  home_card (p, r);
  return x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3];
}

/* the first page's clock: no drop over it */
static bool
over_clock (float x, float y)
{
  if (!N.model->reserved)
    return false;
  float r[4];
  home_cell_box (0, 0, r);
  int cols = N.model->cols;
  float top = r[1] - r[3] * (N.model->reserved / cols), left = r[0] - r[2] * (N.model->reserved % cols);
  return x >= left && x < left + cols * r[2] && y >= top && y < r[1];
}

/* setDropLayoutForDragObject: the hotseat under the finger, else a neighbour the finger or the centre is over
 * (not while the pages turn), else the page on show under the finger */
static int
drop_layout (float vx, float vy)
{
  if (home_in_dock (G.x, G.y))
    return DOCK;
  float fx, fy, cx, cy;
  home_from_screen (G.x, G.y, &fx, &fy);
  home_from_screen (vx, vy, &cx, &cy);
  if (over_clock (fx, fy))
    return NOWHERE;
  int cur = home_page ();
  if (!home_snapping ())
    for (int p = cur - 1; p <= cur + 1; p += 2)
      if (inside_page (p, p < cur ? MIN (fx, cx) : MAX (fx, cx), fy))
        return p;
  float l, r, unused;
  home_from_screen (0, 0, &l, &unused);
  home_from_screen (N.W, 0, &r, &unused);
  for (int p = home_page_at (l); p <= home_page_at (r); p++)
    if (inside_page (p, fx, fy))
      return p;
  return NOWHERE;
}

/* onDragOver */
static void
retarget (void)
{
  float vx = G.x + G.off_x, vy = G.y + G.off_y;
  G.hover = G.path >= dp (BAR_MIN_PATH_DP) && in_bar (G.x, G.y);
  int c = G.hover ? NOWHERE : drop_layout (vx, vy);
  if (c != G.tc) {
    /* setCurrentDropLayout */
    if (G.tc != NOWHERE)
      revert (G.tc);
    outline_out ();
    G.tc = c;
    G.ti = G.last_i = -1;
    G.reordered = false;
    G.alarm_at = 0;
    G.page_c = c;
    G.page_at = c >= 0 ? N.now + PAGE_ALARM_MS : 0;
  }
  if (c == NOWHERE)
    return;
  float x, y;
  to_cont (c, vx, vy, &x, &y);
  int t = nearest (c, x, y);
  if (t < 0)
    return;
  if (t != G.ti) {
    /* setCurrentDropOverCell: a reorder done is let go of (a pending one keeps its alarm) */
    G.ti = t;
    if (G.reordered) {
      G.reordered = false;
      G.last_i = -1;
    }
  }
  float cx, cy;
  icon_centre (c, t, &cx, &cy);
  if (!occupied (c, t)) {
    outline (c, t);
    revert (c);
  } else if (G.last_i != t && hypotf (x - cx, y - cy) < reorder_radius (c, t)) {
    G.last_i = t;
    int sol[MAX_CELLS];
    if (solve (c, t, x, y, sol))
      hint (c, t, sol);
    G.alarm_at = N.now + REORDER_MS;
  }
}

/* the reorder's alarm: room is made where the drag view is now */
static void
reorder_now (void)
{
  int c = G.tc;
  if (c == NOWHERE)
    return;
  float x, y;
  to_cont (c, G.x + G.off_x, G.y + G.off_y, &x, &y);
  int t = nearest (c, x, y), sol[MAX_CELLS];
  if (t < 0)
    return;
  if (!solve (c, t, x, y, sol)) {
    revert (c);
    outline_out ();
    return;
  }
  apply (c, sol);
  G.ti = t;
  G.reordered = true;
  outline (c, t);
}

void
drag_move (float x, float y)
{
  if (!drag_holding ())
    return;
  G.path += hypotf (x - G.x, y - G.y);
  G.x = x;
  G.y = y;
  if (G.phase == PHASE_HOLD && G.path > dp (START_DP))
    start ();
  if (G.phase == PHASE_ACTIVE)
    retarget ();
  wl_schedule (N.wl);
}

/* the icon goes into t of container c, the others where `sol` puts them; those that moved go on from where they
 * show now */
static void
commit (int c, int t, const int *sol)
{
  int n = home_cells (c);
  char *cells[MAX_CELLS] = { NULL };
  float sx[MAX_CELLS], sy[MAX_CELLS];
  bool have[MAX_CELLS] = { false };
  for (int k = 0; k < n; k++) {
    if (sol[k] < 0)
      continue;
    int i = sol[k];
    float px, py, dx, dy;
    home_icon_pos (c, i, &px, &py);
    drag_offset (c, i, &dx, &dy);
    cells[k] = g_strdup (model_at (N.model, cell_pos (c, i)));
    sx[k] = px + dx;
    sy[k] = py + dy;
    have[k] = true;
  }
  g_free (cells[t]);
  cells[t] = g_strdup (G.tile->id);
  have[t] = false;
  bool onto_extra = c >= (int) N.model->pages->len;
  model_drop (N.model, G.tile->id, G.from, G.from_drawer, c == DOCK ? -1 : c, cells, n);
  for (int k = 0; k < n; k++)
    g_free (cells[k]);
  if (G.from_drawer)
    home_unhide (G.tile->id);
  if (onto_extra)
    home_set_extra_page (false);

  Arr *a = arr_find (c);
  if (!a)
    a = arr_get (c);
  if (a) {
    arr_reset (a, c);
    for (int k = 0; k < n; k++) {
      float px, py;
      if (!have[k])
        continue;
      home_icon_pos (c, k, &px, &py);
      if (fabsf (sx[k] - px) >= 0.5f || fabsf (sy[k] - py) >= 0.5f) {
        a->fx[k] = sx[k] - px;
        a->fy[k] = sy[k] - py;
        a->t0[k] = N.now;
      }
    }
  }
  home_model_changed ();
}

/* the drop: Remove, a cell (making room as the last reorder or hint had it), or home */
static void
drop (void)
{
  G.alarm_at = G.page_at = 0;
  outline_out ();
  if (G.hover) {
    fly (LAND_REMOVE, G.from_c, G.from_i);     /* the bar goes with the workspace's return, after it */
    return;
  }
  bar_to (0);
  G.exit_at = N.now + EXIT_DELAY_MS;
  int c = G.tc, sol[MAX_CELLS];
  if (c != NOWHERE) {
    float x, y;
    to_cont (c, G.x + G.off_x, G.y + G.off_y, &x, &y);
    int t = nearest (c, x, y);
    bool ok = t >= 0;
    if (ok && G.reordered && t == G.ti) {
      /* the room already made */
      Arr *a = arr_find (c);
      for (int k = 0; k < MAX_CELLS; k++)
        sol[k] = -1;
      for (int i = 0; i < home_cells (c); i++)
        if (occupied (c, i))
          sol[a ? a->to[i] : i] = i;
    } else if (ok) {
      ok = solve (c, t, x, y, sol);
    }
    if (ok) {
      commit (c, t, sol);
      fly (LAND_CELL, c, t);
      if (c >= 0 && c != home_page ())
        home_snap (c, NEIGHBOUR_DROP_MS);
      return;
    }
    revert (c);
  }
  /* nowhere to go (or no room): back to its cell; from the drawer it is just gone */
  if (!G.from_drawer)
    fly (LAND_HOME, G.from_c, G.from_i);
}

void
drag_up (float x, float y)
{
  if (G.phase == PHASE_HOLD) {
    /* let go before the drag began: back into its cell, the menu staying */
    fly (LAND_HOME, G.from_c, G.from_i);
    G.phase = PHASE_DONE;
    wl_schedule (N.wl);
    return;
  }
  if (G.phase != PHASE_ACTIVE)
    return;
  G.x = x;
  G.y = y;
  retarget ();
  drop ();                          /* still ACTIVE: the view leaves as it looked */
  G.phase = PHASE_DONE;
  wl_schedule (N.wl);
}

static void
finish (void)
{
  bool sprung = G.sprung;
  clear ();
  G.phase = PHASE_NONE;
  G.sprung = false;
  G.flying = false;
  G.tile = NULL;
  G.bar_from = G.bar_to = 0;
  G.bar_t0 = 0;
  if (sprung)
    home_tidy_pages ();
  wl_schedule (N.wl);
}

void
drag_cancel (bool now)
{
  if (G.phase == PHASE_NONE)
    return;
  if (now) {
    finish ();
    return;
  }
  if (drag_holding ()) {
    if (G.tc != NOWHERE)
      revert (G.tc);
    G.hover = false;
    G.tc = NOWHERE;
    drop ();
    G.phase = PHASE_DONE;
  }
}

void
drag_hurry (void)
{
  if (G.exit_at > N.now)
    G.exit_at = N.now;
  wl_schedule (N.wl);
}

/* the flight is over: Remove takes the icon off and the workspace comes back at once */
static void
landed (void)
{
  G.flying = false;
  if (G.land == LAND_REMOVE) {
    if (!G.from_drawer)
      home_remove_icon (G.tile->id, G.from);
    G.exit_at = N.now;
  }
}

bool
drag_frame (void)
{
  if (G.phase == PHASE_NONE)
    return false;
  bool more = false;
  if (G.sp_t0) {
    if (N.now - G.sp_t0 >= SPRING_MS)
      G.sp_t0 = 0;
    else
      more = true;
  }
  if (G.bar_t0) {
    if (N.now - G.bar_t0 >= BAR_FADE_MS)
      G.bar_t0 = 0;
    else
      more = true;
  }
  more |= N.now - G.t_hold < VIEW_SCALE_MS;
  if (G.phase == PHASE_ACTIVE) {
    if (G.alarm_at && N.now >= G.alarm_at) {
      G.alarm_at = 0;
      reorder_now ();
    }
    if (G.page_at && N.now >= G.page_at) {
      G.page_at = 0;
      /* onAlarm: only a page not on show, and only when the finger moved since the last scroll */
      if (G.page_c >= 0 && G.page_c != home_page () && G.path != G.snap_path) {
        G.snap_path = G.path;
        home_snap (G.page_c, PAGE_SCROLL_MS);
      }
    }
    more |= G.alarm_at || G.page_at;
  }
  for (int k = 0; k < MAX_ARR; k++) {
    Arr *a = &G.arr[k];
    if (a->c == NOWHERE)
      continue;
    for (int i = 0; i < MAX_CELLS; i++) {
      if (a->t0[i]) {
        if (N.now - a->t0[i] >= MOVE_MS)
          a->t0[i] = 0;
        else
          more = true;
      }
      Bounce *b = &a->b[i];
      if (b->stop_t0) {
        if (N.now - b->stop_t0 >= MOVE_MS)
          *b = (Bounce) { 0 };
        else
          more = true;
      } else if (b->t0) {
        more |= b->repeat || N.now < b->t0 + b->ms;
      }
    }
  }
  for (int k = 0; k < N_PREVIEWS; k++)
    if (G.prev[k].t0) {
      if (N.now - G.prev[k].t0 >= PREVIEW_FADE_MS)
        G.prev[k].t0 = 0;
      else
        more = true;
    }
  if (G.phase == PHASE_DONE) {
    if (G.flying && N.now - G.fly_t0 >= G.fly_ms)
      landed ();
    if (G.exit_at && N.now >= G.exit_at) {
      G.exit_at = 0;
      spring_to (0);
      bar_to (0);
      more = true;
    }
    more |= G.flying || G.exit_at;
    if (!G.flying && !G.exit_at && (!G.sprung || (!G.sp_t0 && G.sp_to == 0)))
      finish ();
  }
  return more;
}
