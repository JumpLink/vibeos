/* The long-press menus:
 *
 *   an icon's   [App info, Remove] on the home screen and the dock, [App info, Add to home screen] in
 *               the drawer, and the app's own shortcuts (its desktop actions) on a card of their own nearest the
 *               icon, with an arrow at the icon's centre. Above the icon when it fits (ABOVE_GAP_DP clear of it),
 *               else BELOW_GAP_DP below it; left-aligned a little in from the icon, else right-aligned, else
 *               centred and without the arrow.
 *   the home's  on the wallpaper: [Wallpaper & style, Apps list, Home settings], a card a row
 *               with round outer and small inner corners, centred on the finger, its bottom OPTIONS_GAP_DP above it.
 *
 * It opens scaling 0.5 → 1.02 in 200 ms (emphasized decelerate) and back to 1 in 140 ms about the arrow, its alpha
 * 0 → 1 in 80 ms; it closes shrinking for 230 ms, fading over the last 80. Widgets rows and the
 * shortcuts' "+" are left out: there are no widgets here, and a home cell holds an app, not one of its actions. */
#include <librsvg/rsvg.h>
#include <math.h>

#include "vibeos.h"
#include "popup_icons.h"

#define POP_W_DP          216
#define ROW_DP            52
#define CARD_GAP_DP       2
#define CARD_RADIUS_DP    28
#define INNER_RADIUS_DP   4
#define SYS_ICON_DP       20
#define SYS_ICON_X_DP     16
#define TEXT_X_DP         54
#define TEXT_END_DP       16
#define TEXT_SP           14
#define CAP_HEIGHT        0.711f        /* VibeOS Roboto: capHeight 1456 / 2048 */
#define APP_ICON_DP       32            /* a shortcut's row shows the app's icon */
#define APP_ICON_X_DP     11.5
#define ABOVE_GAP_DP      15
#define BELOW_GAP_DP      13
#define LEFT_IN_DP        (8 / 3.0)
#define RIGHT_IN_DP       (7 / 3.0)
#define OPTIONS_GAP_DP    35
#define OPTIONS_SIDE_DP   8
#define ARROW_W_DP        10            /* where it leaves the card */
#define ARROW_H_DP        6.7
#define MAX_SHORTCUTS     4
#define MAX_ROWS          (3 + MAX_SHORTCUTS)
#define TAP_SLOP_DP       8

#define CARD_RGB          0x131B1C
#define TEXT_RGB          0xDDE7E9
#define PRESSED_ALPHA     0.12f

#define OPEN_MS           200.0
#define OPEN_FROM         0.5f
#define OPEN_OVERSHOOT    1.02f
#define SETTLE_MS         140.0
#define FADE_IN_MS        80.0
#define CLOSE_MS          230.0
#define FADE_OUT_MS       80.0
#define CLOSE_TO          0.75f

typedef enum { ICON_INFO, ICON_REMOVE, ICON_PLUS, ICON_PALETTE, ICON_APPS, ICON_SETTING, N_ICONS } Icon;

typedef enum {
  ACT_INFO, ACT_REMOVE, ACT_ADD_HOME, ACT_SHORTCUT, ACT_WALLPAPER, ACT_APPS, ACT_HOME_SETTINGS
} Act;

typedef struct {
  Act         act;
  int         icon;               /* Icon, or -1: the app's own (a shortcut) */
  TextLayout *label;
  char       *action;             /* a shortcut's desktop action */
  float       y;                  /* its top, at rest */
} Row;

typedef struct {
  int   first, n;                 /* its rows */
  float y, h;
  float r[4];                     /* corners: top left, top right, bottom right, bottom left */
} Card;

static struct {
  bool        open, closing;
  double      t0;
  float       x, y, w, h;         /* every card's box, at rest */
  float       px, py;             /* what it scales about */
  bool        arrow, above;
  float       arrow_x;
  Row         rows[MAX_ROWS];
  int         n_rows;
  Card        cards[MAX_ROWS];
  int         n_cards;
  Tile       *tile;
  ModelPos    pos;
  int         pressed;            /* the row under the finger, or -1 */
  float       down_x, down_y;
  TextFont   *font;
  GfxTex     *icons[N_ICONS];
} P = { .pressed = -1 };

static const char *const SVGS[N_ICONS] = { INFO_SVG, REMOVE_SVG, PLUS_SVG, PALETTE_SVG, APPS_SVG, SETTING_SVG };

GfxTex *
svg_texture (const char *svg, int px)
{
  RsvgHandle *h = rsvg_handle_new_from_data ((const guint8 *) svg, strlen (svg), NULL);
  if (!h)
    return NULL;
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (s);
  RsvgRectangle vp = { 0, 0, px, px };
  rsvg_handle_render_document (h, cr, &vp, NULL);
  cairo_destroy (cr);
  cairo_surface_flush (s);
  GfxTex *t = gfx_tex_from_bgra (px, px, cairo_image_surface_get_data (s), cairo_image_surface_get_stride (s));
  cairo_surface_destroy (s);
  g_object_unref (h);
  return t;
}

static void
load_icons (void)
{
  if (P.font)
    return;
  P.font = text_font_axes ("VibeOS Roboto", 600, dp (TEXT_SP), "opsz=14");
  int px = (int) lroundf (dp (SYS_ICON_DP));
  for (int i = 0; i < N_ICONS; i++)
    P.icons[i] = svg_texture (SVGS[i], px);
}

static void
clear_rows (void)
{
  for (int i = 0; i < P.n_rows; i++) {
    text_layout_free (P.rows[i].label);
    g_free (P.rows[i].action);
  }
  P.n_rows = P.n_cards = 0;
  P.pressed = -1;
  P.tile = NULL;
}

static void
add_row (Act act, int icon, const char *label, const char *action)
{
  if (P.n_rows >= MAX_ROWS)
    return;
  float max = dp (POP_W_DP - TEXT_X_DP - TEXT_END_DP);
  P.rows[P.n_rows++] = (Row) { act, icon, text_layout (P.font, label, max, 1, TEXT_LEFT), g_strdup (action), 0 };
}

static void
add_card (int first, int n)
{
  if (n > 0)
    P.cards[P.n_cards++] = (Card) { first, n };
}

/* the cards top to bottom from y, every row's y with them, and their corners (the options' inner ones are small) */
static void
stack (float y, bool options)
{
  float r = dp (CARD_RADIUS_DP), inner = dp (INNER_RADIUS_DP);
  P.y = y;
  for (int c = 0; c < P.n_cards; c++) {
    Card *k = &P.cards[c];
    k->y = y;
    k->h = k->n * dp (ROW_DP);
    for (int i = 0; i < k->n; i++)
      P.rows[k->first + i].y = y + i * dp (ROW_DP);
    float top = options && c > 0 ? inner : r, bottom = options && c < P.n_cards - 1 ? inner : r;
    k->r[0] = k->r[1] = top;
    k->r[2] = k->r[3] = bottom;
    y += k->h + dp (CARD_GAP_DP);
  }
  P.h = y - dp (CARD_GAP_DP) - P.y;
}

static float
total_height (void)
{
  int rows = 0;
  for (int c = 0; c < P.n_cards; c++)
    rows += P.cards[c].n;
  return rows * dp (ROW_DP) + MAX (0, P.n_cards - 1) * dp (CARD_GAP_DP);
}

static void
start (void)
{
  P.open = true;
  P.closing = false;
  P.t0 = N.now;
  P.pressed = -1;
  wl_schedule (N.wl);
}

/* ---- opening ---------------------------------------------------------------------------------------------------- */

void
popup_open_icon (Tile *t, ModelPos pos, bool from_drawer, float ix, float iy, float isz)
{
  load_icons ();
  clear_rows ();
  P.tile = t;
  P.pos = pos;

  add_row (ACT_INFO, ICON_INFO, _("App info"), NULL);
  if (from_drawer)
    add_row (ACT_ADD_HOME, ICON_PLUS, _("Add to home screen"), NULL);
  else
    add_row (ACT_REMOVE, ICON_REMOVE, _("Remove"), NULL);
  int n_sys = P.n_rows;
  GDesktopAppInfo *d = G_IS_DESKTOP_APP_INFO (t->info) ? G_DESKTOP_APP_INFO (t->info) : NULL;
  const char *const *actions = d ? g_desktop_app_info_list_actions (d) : NULL;
  for (int i = 0; actions && actions[i] && P.n_rows < n_sys + MAX_SHORTCUTS; i++) {
    g_autofree char *name = g_desktop_app_info_get_action_name (d, actions[i]);
    if (name && *name)
      add_row (ACT_SHORTCUT, -1, name, actions[i]);
  }

  P.w = dp (POP_W_DP);
  float h = 0;
  /* the shortcuts' card goes nearest the icon */
  P.n_cards = 0;
  add_card (0, n_sys);
  add_card (n_sys, P.n_rows - n_sys);
  h = total_height ();
  float top = roundf (dp (N.status_dp));
  P.above = iy - dp (ABOVE_GAP_DP) - h >= top;
  if (!P.above && P.n_cards == 2) {
    Card sys = P.cards[0];
    P.cards[0] = P.cards[1];
    P.cards[1] = sys;
  }
  stack (P.above ? iy - dp (ABOVE_GAP_DP) - h : iy + isz + dp (BELOW_GAP_DP), false);

  float left = ix + dp (LEFT_IN_DP), right = ix + isz - dp (RIGHT_IN_DP) - P.w;
  P.arrow = true;
  if (left + P.w < N.W)
    P.x = roundf (left);
  else if (right > 0)
    P.x = roundf (right);
  else {
    P.x = roundf ((N.W - P.w) / 2);
    P.arrow = false;
  }
  P.arrow_x = ix + isz / 2;
  P.px = P.arrow_x;
  P.py = P.above ? P.y + P.h : P.y;
  start ();
}

void
popup_open_options (float x, float y)
{
  load_icons ();
  clear_rows ();
  add_row (ACT_WALLPAPER, ICON_PALETTE, _("Wallpaper & style"), NULL);
  add_row (ACT_APPS, ICON_APPS, _("Apps list"), NULL);
  add_row (ACT_HOME_SETTINGS, ICON_SETTING, _("Home settings"), NULL);
  for (int i = 0; i < P.n_rows; i++)
    add_card (i, 1);
  P.w = dp (POP_W_DP);
  float h = total_height ();
  float top = roundf (dp (N.status_dp));
  P.above = y - dp (OPTIONS_GAP_DP) - h >= top;
  stack (P.above ? y - dp (OPTIONS_GAP_DP) - h : y + dp (OPTIONS_GAP_DP), true);
  P.x = roundf (CLAMP (x - P.w / 2, dp (OPTIONS_SIDE_DP), N.W - dp (OPTIONS_SIDE_DP) - P.w));
  P.arrow = false;
  P.px = CLAMP (x, P.x, P.x + P.w);
  P.py = P.above ? P.y + P.h : P.y;
  start ();
}

/* ---- closing ---------------------------------------------------------------------------------------------------- */

bool
popup_is_open (void)
{
  return P.open && !P.closing;
}

bool
popup_showing (void)
{
  return P.open;
}

void
popup_close (void)
{
  if (!P.open || P.closing)
    return;
  /* caught opening: shrink from where it is (the open's own scale is near enough 1 after the fade) */
  P.closing = true;
  P.t0 = N.now;
  P.pressed = -1;
  wl_schedule (N.wl);
}

void
popup_close_now (void)
{
  P.open = P.closing = false;
  clear_rows ();
  wl_schedule (N.wl);
}

/* ---- animation -------------------------------------------------------------------------------------------------- */

static void
anim (float *scale, float *alpha)
{
  double ms = N.now - P.t0;
  if (P.closing) {
    float t = CLAMP (ms / CLOSE_MS, 0, 1);
    *scale = 1 + (CLOSE_TO - 1) * spongy_curve (SPONGY_FAST_OUT_SLOW_IN, t);
    *alpha = CLAMP ((CLOSE_MS - ms) / FADE_OUT_MS, 0, 1);
    return;
  }
  if (ms < OPEN_MS)
    *scale = OPEN_FROM + (OPEN_OVERSHOOT - OPEN_FROM) * spongy_curve (SPONGY_EMPHASIZED_DECELERATE,
                                                                       CLAMP (ms / OPEN_MS, 0, 1));
  else
    *scale = OPEN_OVERSHOOT + (1 - OPEN_OVERSHOOT) * spongy_curve (SPONGY_DECELERATE,
                                                                   CLAMP ((ms - OPEN_MS) / SETTLE_MS, 0, 1));
  *alpha = CLAMP (ms / FADE_IN_MS, 0, 1);
}

bool
popup_frame (void)
{
  if (!P.open)
    return false;
  double ms = N.now - P.t0;
  if (P.closing && ms >= CLOSE_MS) {
    popup_close_now ();
    return false;
  }
  return P.closing || ms < OPEN_MS + SETTLE_MS;
}

/* ---- drawing ---------------------------------------------------------------------------------------------------- */

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

/* the arrow: a rhombus whose near half hides under the card, its far half the triangle */
static void
draw_arrow (GfxColor c)
{
  float cx = P.arrow_x, cy = P.above ? P.y + P.h : P.y;
  float half = dp (ARROW_H_DP), side = half * (float) G_SQRT2;
  gfx_save ();
  gfx_scale (dp (ARROW_W_DP) / (2 * half), 1, cx, cy);
  gfx_rotate ((float) G_PI / 4, cx, cy);
  gfx_rect (cx - side / 2, cy - side / 2, side, side, dp (1.5), c);
  gfx_restore ();
}

void
popup_draw (void)
{
  if (!P.open)
    return;
  float s, a;
  anim (&s, &a);
  if (a <= 0)
    return;
  GfxColor card = GFX_RGBA (CARD_RGB, 1), text = GFX_RGBA (TEXT_RGB, 1);
  gfx_save ();
  gfx_opacity (a);
  gfx_scale (s, s, P.px, P.py);
  if (P.arrow)
    draw_arrow (card);
  for (int c = 0; c < P.n_cards; c++) {
    Card *k = &P.cards[c];
    gfx_rect_corners (P.x, k->y, P.w, k->h, k->r, card);
    if (P.pressed >= k->first && P.pressed < k->first + k->n) {
      /* the pressed row, with the card's corners where it meets them */
      int i = P.pressed - k->first;
      float r[4] = { i == 0 ? k->r[0] : 0, i == 0 ? k->r[1] : 0, i == k->n - 1 ? k->r[2] : 0,
                     i == k->n - 1 ? k->r[3] : 0 };
      gfx_rect_corners (P.x, P.rows[P.pressed].y, P.w, dp (ROW_DP), r, (GfxColor) { 1, 1, 1, PRESSED_ALPHA });
    }
  }
  float cap = dp (TEXT_SP) * CAP_HEIGHT;
  for (int i = 0; i < P.n_rows; i++) {
    Row *r = &P.rows[i];
    float mid = r->y + dp (ROW_DP) / 2;
    if (r->icon >= 0) {
      float sz = dp (SYS_ICON_DP);
      tinted (P.icons[r->icon], P.x + dp (SYS_ICON_X_DP), roundf (mid - sz / 2), sz, text);
    } else if (P.tile) {
      float sz = roundf (dp (APP_ICON_DP));
      if (P.tile->icon)
        gfx_image (P.tile->icon, roundf (P.x + dp (APP_ICON_X_DP)), roundf (mid - sz / 2), sz, sz, 0, 1);
    }
    text_draw (r->label, P.x + dp (TEXT_X_DP), roundf (mid + cap / 2 - text_ascent (P.font)), text);
  }
  gfx_restore ();
}

/* ---- the actions ------------------------------------------------------------------------------------------------ */

static void
spawn (const char *a0, const char *a1, const char *a2)
{
  const char *argv[] = { a0, a1, a2, NULL };
  g_autoptr (GError) err = NULL;
  if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err))
    g_warning ("%s: %s", a0, err->message);
}

/* the app off the home screen (into hidden-from-home, still in the drawer), or back on it at the first free cell */
static void
set_hidden (const char *id, bool hidden)
{
  g_auto (GStrv) old = g_settings_get_strv (N.settings, "hidden-from-home");
  g_autoptr (GPtrArray) list = g_ptr_array_new ();
  for (char **h = old; *h; h++)
    if (g_strcmp0 (*h, id))
      g_ptr_array_add (list, *h);
  if (hidden)
    g_ptr_array_add (list, (gpointer) id);
  g_ptr_array_add (list, NULL);
  g_settings_set_strv (N.settings, "hidden-from-home", (const char *const *) list->pdata);
  model_set_hidden (N.model, (char **) list->pdata);
  g_autoptr (GHashTable) apps = tiles_apps ();
  model_reconcile (N.model, apps);
}

void
home_remove_icon (const char *id, ModelPos pos)
{
  /* one of several copies goes; the last one takes the app off the home screen; its cell stays empty, and a page
   * it leaves empty goes (not while a drag is on: its end tidies) */
  if (!model_remove_one (N.model, id, pos))
    set_hidden (id, true);
  if (drag_holding () || drag_busy ())
    home_model_changed ();
  else
    home_tidy_pages ();
}

/* a drawer icon dropped on the home screen while the app was off it: back on, where it was dropped */
void
home_unhide (const char *id)
{
  if (g_hash_table_contains (N.model->hidden, id))
    set_hidden (id, false);
}

static void
activate (Row *r)
{
  Tile *t = P.tile;
  switch (r->act) {
  case ACT_INFO:
    spawn ("vibeos-settings", "--app", t->id);
    break;
  case ACT_REMOVE:
    home_remove_icon (t->id, P.pos);
    break;
  case ACT_ADD_HOME:
    if (g_hash_table_contains (N.model->hidden, t->id))
      set_hidden (t->id, false);
    else
      model_add_shortcut (N.model, t->id);
    home_model_changed ();
    break;
  case ACT_SHORTCUT:
    if (G_IS_DESKTOP_APP_INFO (t->info))
      g_desktop_app_info_launch_action (G_DESKTOP_APP_INFO (t->info), r->action, NULL);
    break;
  case ACT_WALLPAPER:
    spawn ("vibeos-settings", "--page", "wallpaper");
    break;
  case ACT_APPS:
    drawer_open ();
    break;
  case ACT_HOME_SETTINGS:
    spawn ("vibeos-settings", "--page", "home");
    break;
  }
}

/* ---- touch ------------------------------------------------------------------------------------------------------ */

static int
row_at (float x, float y)
{
  if (x < P.x || x >= P.x + P.w)
    return -1;
  for (int i = 0; i < P.n_rows; i++)
    if (y >= P.rows[i].y && y < P.rows[i].y + dp (ROW_DP))
      return i;
  return -1;
}

/* a finger down while it is open: on a row it presses it; anywhere else it closes the menu and is spent */
void
popup_touch_down (float x, float y)
{
  P.down_x = x;
  P.down_y = y;
  P.pressed = row_at (x, y);
  if (P.pressed < 0)
    popup_close ();
  wl_schedule (N.wl);
}

void
popup_touch_move (float x, float y)
{
  if (P.pressed >= 0 && hypotf (x - P.down_x, y - P.down_y) > dp (TAP_SLOP_DP)) {
    P.pressed = -1;
    wl_schedule (N.wl);
  }
}

void
popup_touch_up (float x, float y)
{
  int i = P.pressed;
  P.pressed = -1;
  if (i < 0 || row_at (x, y) != i)
    return;
  Row r = P.rows[i];
  r.action = g_strdup (r.action);
  popup_close ();
  activate (&r);
  g_free (r.action);
}

void
popup_touch_cancel (void)
{
  P.pressed = -1;
  wl_schedule (N.wl);
}
