/* Neo Launcher 2: what the home screen, the dock and the app drawer share. Everything is laid out in device pixels
 * from Android's dp values (Launcher3 on the S10e: 5 × 5 "Large Phone" grid, 56 dp icons, 1 dp = 3 px there). */
#pragma once

#include <gio/gdesktopappinfo.h>
#include <stdbool.h>

#include "gfx.h"
#include "model.h"
#include "spongy.h"
#include "text.h"
#include "wl.h"

/* ---- geometry (dp unless named _PX) ----------------------------------------------------------------------------- */

#define STATUS_BAR_DP            (116 / 3.0)   /* the shell's status bar: the cutout sets it, not the 24 dp default */
#define NAV_GESTURE_DP           32            /* the bottom strip the shell's swipe up starts in */
#define GRID_COLS                5
#define GRID_ROWS                5
#define ICON_DP                  56
#define WS_SIDE_DP               8             /* cell box x: 8 dp in from each side */
#define WS_TOP_DP                (32 / 3.0)    /* cell box y: below the status bar */
#define WS_CONTENT_DP            85            /* icon + gap + label box: (cell − this)/2 is the icon's top in a cell */
#define LABEL_BASELINE_DP        74.5          /* below the icon's top */
#define LABEL_SP                 12
#define LABEL_HOME_MAX_DP        52            /* "Play St…" fits, "Play Sto…" does not */
#define LABEL_DRAWER_MAX_DP      54.6          /* "Calculat…" fits, "Messagi…" does not */
#define HOTSEAT_DP               104           /* the dock: icon + 48 dp below */
#define HOTSEAT_CELL_DP          63
#define PAGE_IND_DP              24
#define PAGE_DOT_DP              6
#define PAGE_DOT_GAP_DP          4

#define WALLPAPER_ZOOM           1.10f         /* the wallpaper at home, about the screen centre */
#define CONTENT_SCALE            0.97f         /* workspace + dock behind the drawer and behind a launch */

/* the drawer sheet, its handle, search pill and list (px from the sheet's top, measured on the S10e, as dp) */
#define SHEET_RADIUS_DP          28
#define HANDLE_W_DP              32
#define HANDLE_H_DP              4
#define HANDLE_TOP_DP            16
#define SEARCH_TOP_DP            34            /* 8 dp margin + 26 dp overlap */
#define SEARCH_H_DP              52
#define SEARCH_SIDE_DP           4.5
#define SEARCH_TEXT_SP           20
#define LIST_TOP_DP              90            /* the list's window starts here (y 386 on the S10e) */
#define LIST_ICON_TOP_DP         (77 / 3.0)    /* first icon below the list's top */
#define LIST_ROW_DP              120           /* 104 dp cell + 16 dp border space */
#define LIST_LABEL_BASELINE_DP   (231.5 / 3.0)
#define FASTSCROLL_W_DP          (20 / 3.0)
#define FASTSCROLL_H_DP          52
#define FASTSCROLL_END_DP        (1 / 3.0)
#define FASTSCROLL_TOP_DP        (37 / 3.0)    /* thumb at rest, below the list's top */

typedef struct {
  char        *id;
  GAppInfo    *info;
  GfxTex      *icon;              /* NULL until the icon thread drew it */
  TextLayout  *label;             /* the home label (52 dp) */
  TextLayout  *dlabel;            /* the drawer label (54.6 dp) */
  bool         hidden;            /* its window is growing out of it */
  char        *splash_icon;       /* the icon drawn at the splash's 160 dp (a cached PNG), or NULL: the theme's */
} Tile;

/* the icon pressed under a finger: 1 → 1.1 in 200 ms (t²) on down, back with 1 − (1 − t)² */
typedef struct {
  Tile   *tile;
  double  t0;
  float   from, to;
  bool    up;
} Press;

typedef struct {
  Wl          *wl;
  int          W, H;
  double       dp;
  double       now;               /* ms, the frame being drawn */
  GHashTable  *tiles;             /* id -> Tile* */
  GPtrArray   *sorted;            /* Tile*, the drawer's A→Z order */
  Model       *model;
  GSettings   *settings;
  TextFont    *label_font;
  Press        press;
  GDBusConnection *bus;

  /* launch: the content shrinks to 0.97 while the window grows out of an icon */
  double       launch_t0;         /* 0: none */
  bool         launch_from_drawer;
} Neo;

extern Neo N;

static inline float dp (double v) { return (float) (v * N.dp); }

/* tiles.c */
void   tiles_load        (void);                       /* the apps, their labels, and the icon thread */
Tile  *tiles_get         (const char *id);
void   tile_draw_icon    (Tile *t, float x, float y, float size, float alpha);
float  press_scale       (Tile *t);                    /* this tile's press scale now */
bool   press_animating   (void);
void   press_down        (Tile *t);
void   press_up          (void);
void   tile_launch       (Tile *t, float x, float y, float size, bool from_drawer);
float  launch_content_scale (void);                    /* the home content's scale while a launch runs */
void   label_draw        (TextLayout *l, float x, float y, GfxColor c, bool shadow);

/* home.c */
void   home_layout       (void);
void   home_draw         (float content_scale);          /* pages + clock + dock + page dots */
bool   home_frame        (void);                         /* animations; true while moving */
bool   home_touch_down   (float x, float y, double t);
void   home_touch_move   (float x, float y, double t, bool *vertical);
void   home_touch_up     (float x, float y, double t);
void   home_touch_cancel (double t);
float  home_stretch      (void);                         /* the horizontal edge stretch amount */
Tile  *home_tile_at      (float x, float y, float *ix, float *iy);
void   home_clock_tick   (void);
bool   home_icon_rect    (const char *id, float r[4]);

/* nav.c: gesture navigation, the launcher's half (the shell drives it over D-Bus) */
void   nav_init          (void);
bool   nav_frame         (void);                         /* true while the home reveal runs */
float  nav_depth         (void);                         /* the depth an app over the launcher puts on it */
bool   nav_blurs         (void);                         /* that depth blurs (not during the reveal) */
float  nav_home_scale    (void);                         /* the workspace and dock's scale and alpha */
float  nav_home_alpha    (void);
float  nav_home_ty       (void);                         /* … and how far down it is moved */
float  nav_scrim_alpha   (void);                         /* back's scrim over it all: black at this alpha */
float  nav_scrim_blur    (void);                         /* … over a blur of this radius (px) */
void   nav_drawer        (bool open);                    /* tell the shell the drawer opened or closed */
void   nav_icons_changed (void);                         /* the page or the dock's icons moved */

/* drawer.c */
void   drawer_layout     (void);
float  drawer_progress   (void);                         /* f: 0 home, 1 all apps */
bool   drawer_frame      (void);
void   drawer_draw       (void);
bool   drawer_is_open    (void);
/* a drag handed over from home (finger moved up past the slop); y_down is where it went down */
void   drawer_drag_begin (float y_down, float y, double t);
bool   drawer_touch_down (float x, float y, double t);       /* false: not the drawer's */
void   drawer_touch_move (float x, float y, double t);
void   drawer_touch_up   (float x, float y, double t);
void   drawer_touch_cancel (double t);
void   drawer_close_now  (void);
void   drawer_back       (void);                         /* close on back: 200 ms ACCELERATE */
bool   drawer_dragging   (void);
