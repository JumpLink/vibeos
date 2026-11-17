/* libspongy: scrolling physics modelled on Android's, for anything that scrolls.
 *
 *   SpongyEdge      the stretch overscroll: pulled past an end, the content stretches like a sponge and springs back
 *                   (its state, its spring, the stretch amount)
 *   SpongyFling     the spline fling: how far and how long a flung list runs
 *   SpongyVelocity  the pointer's velocity: a least-squares quadratic over the last 100 ms
 *   SpongySpring    a damped spring, stepped exactly
 *   SpongyInterp    easing curves: any cubic path, and the stock ones
 *   SpongyList      a list along one axis (up and down, or sideways) wired to all of the above, and snapping
 *
 * Plain C, no allocation, no clock of its own: every call takes the frame or event time in milliseconds. Distances
 * are pixels. The stretch itself is drawn by whoever renders (see spongy_stretch_map); spongy-gtk.h is a GTK 4
 * widget that does all of it. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- device ---------------------------------------------------------------------------------------------------- */

/* density = pixels per dp (3 on a 480 dpi phone). Sets the fling physics' coefficient and the slop/fling
 * thresholds of SpongyList; 3 until it is called. */
void   spongy_set_density        (double density);
double spongy_density            (void);

/* ---- the stretch edge ------------------------------------------------------------------------------------------ */

typedef enum {
  SPONGY_EDGE_IDLE = 0,
  SPONGY_EDGE_PULL = 1,
  SPONGY_EDGE_RECEDE = 3,
} SpongyEdgeState;

typedef struct {
  SpongyEdgeState state;
  float  distance;        /* 0..1 of size */
  float  pull_distance;
  float  velocity;        /* px/s, while receding */
  double t_ms;            /* the last spring step */
  float  size;            /* the view's extent along the axis, px */
} SpongyEdge;

void   spongy_edge_init           (SpongyEdge *e, float size);
void   spongy_edge_set_size       (SpongyEdge *e, float size);
void   spongy_edge_finish         (SpongyEdge *e);
bool   spongy_edge_is_finished    (const SpongyEdge *e);
void   spongy_edge_pull           (SpongyEdge *e, float delta, double now_ms);      /* delta in units of size */
float  spongy_edge_pull_distance  (SpongyEdge *e, float delta, double now_ms);      /* returns what it took */
void   spongy_edge_release        (SpongyEdge *e, double now_ms);
void   spongy_edge_absorb         (SpongyEdge *e, int velocity_px_s, double now_ms);
/* One frame: steps the spring and returns the shader's overscroll amount (signed, |a| <= 0.032; negative for the
 * far edge, bottom/right). *animating: another frame is wanted. */
float  spongy_edge_frame          (SpongyEdge *e, double now_ms, bool far_edge, bool *animating);
float  spongy_edge_damp           (float distance);                                 /* a pull's distance, damped */

/* Where an output pixel of the stretched picture samples its source, along one axis: the stretch shader's mapping on
 * the CPU (hit-testing, tests, renderers without shaders). amount from spongy_edge_frame, size = the picture's extent.
 * */
float  spongy_stretch_map         (float pos, float amount, float size);
/* The inverse: where a source pixel ends up (to find what a touch under a stretch hits) */
float  spongy_stretch_unmap       (float pos, float amount, float size);

/* ---- fling ----------------------------------------------------------------------------------------------------- */

typedef struct {
  bool   finished;
  double start_ms;
  float  start, final;
  float  distance;        /* signed spline distance */
  int    duration, spline_duration;   /* ms */
  float  velocity;        /* the starting velocity, px/s */
  float  position, curr_velocity;
} SpongyFling;

/* A fling from `start` at `velocity` px/s, ending inside [min, max] (pass -/+INFINITY for an endless list: the
 * list itself finds its end) */
void   spongy_fling_start         (SpongyFling *f, float start, float velocity, float min, float max, double now_ms);
/* Steps to now: false once it has ended (position is then final) */
bool   spongy_fling_update        (SpongyFling *f, double now_ms);
void   spongy_fling_abort         (SpongyFling *f);
double spongy_fling_distance      (float velocity);     /* how far a fling at this speed goes, px */
int    spongy_fling_duration      (float velocity);     /* ms */
/* the inverse of spongy_fling_distance: the speed, px/s, of a fling that runs `distance` px (0 for none) */
float  spongy_fling_velocity      (double distance);

/* ---- velocity -------------------------------------------------------------------------------------------------- */

#define SPONGY_VELOCITY_HISTORY 20

typedef struct {
  double t[SPONGY_VELOCITY_HISTORY];
  float  x[SPONGY_VELOCITY_HISTORY], y[SPONGY_VELOCITY_HISTORY];
  int    n, head;
} SpongyVelocity;

void   spongy_velocity_clear      (SpongyVelocity *v);
void   spongy_velocity_add        (SpongyVelocity *v, double t_ms, float x, float y);
/* px/s at the latest sample; 0 when the pointer stopped (no move in the last 40 ms before `now`) */
void   spongy_velocity_get        (const SpongyVelocity *v, double now_ms, float *vx, float *vy);

/* ---- springs --------------------------------------------------------------------------------------------------- */

#define SPONGY_STIFFNESS_HIGH            10000.0
#define SPONGY_STIFFNESS_MEDIUM          1500.0
#define SPONGY_STIFFNESS_LOW             200.0
#define SPONGY_STIFFNESS_VERY_LOW        50.0
#define SPONGY_DAMPING_HIGH_BOUNCY       0.2
#define SPONGY_DAMPING_MEDIUM_BOUNCY     0.5
#define SPONGY_DAMPING_LOW_BOUNCY        0.75
#define SPONGY_DAMPING_NO_BOUNCY         1.0
#define SPONGY_MIN_VISIBLE_PIXELS        1.0       /* the minimum visible change, by what a spring moves */
#define SPONGY_MIN_VISIBLE_ALPHA         (1.0 / 256)
#define SPONGY_MIN_VISIBLE_SCALE         (1.0 / 500)
#define SPONGY_MIN_VISIBLE_ROTATION      0.1

typedef struct {
  double stiffness, damping;
  double final;
  double value, velocity;          /* per second */
  double min, max;
  double value_threshold, velocity_threshold;
  double last_ms;
  bool   running;
} SpongySpring;

void   spongy_spring_init         (SpongySpring *s, double stiffness, double damping, double min_visible_change);
/* start (or retarget a running spring, keeping its velocity) from value toward final */
void   spongy_spring_animate      (SpongySpring *s, double value, double velocity, double final, double now_ms);
void   spongy_spring_set_final    (SpongySpring *s, double final);
/* steps to now; false once at rest (value == final) */
bool   spongy_spring_update       (SpongySpring *s, double now_ms);
/* the closed form alone: value and velocity `dt` ms after (x, v) */
void   spongy_spring_step         (double stiffness, double damping, double final, double *x, double *v, double dt_ms);

/* ---- interpolators --------------------------------------------------------------------------------------------- */

typedef struct {
  int   n;                         /* cubic segments */
  float p[8][6];                   /* per segment: c1x c1y c2x c2y endx endy (from the previous end) */
} SpongyInterp;

void   spongy_interp_cubic        (SpongyInterp *i, float x1, float y1, float x2, float y2);
/* an SVG path of M/C commands in the unit square ("M 0,0 C …"), from (0, 0) to (1, 1) */
bool   spongy_interp_path         (SpongyInterp *i, const char *path);
float  spongy_interp_get          (const SpongyInterp *i, float t);

/* The stock curves: the platform's, Material 3's easing and the launcher's */
typedef enum {
  SPONGY_LINEAR,
  SPONGY_FAST_OUT_SLOW_IN,         /* (0.4, 0, 0.2, 1) */
  SPONGY_FAST_OUT_LINEAR_IN,       /* (0.4, 0, 1, 1) */
  SPONGY_LINEAR_OUT_SLOW_IN,       /* (0, 0, 0.2, 1) */
  SPONGY_EMPHASIZED,               /* the two-part Material 3 curve */
  SPONGY_EMPHASIZED_ACCELERATE,    /* (0.3, 0, 0.8, 0.15) */
  SPONGY_EMPHASIZED_DECELERATE,    /* (0.05, 0.7, 0.1, 1) */
  SPONGY_STANDARD,                 /* (0.2, 0, 0, 1) */
  SPONGY_STANDARD_ACCELERATE,      /* (0.3, 0, 1, 1) */
  SPONGY_STANDARD_DECELERATE,      /* (0, 0, 0, 1) */
  SPONGY_DECELERATE_QUINT,         /* 1 - (1 - t)^5 */
  SPONGY_DECELERATE_CUBIC,         /* 1 - (1 - t)^3 */
  SPONGY_DECELERATE,               /* 1 - (1 - t)^2 */
  SPONGY_ACCELERATE,               /* t^2 */
  SPONGY_ACCELERATE_DECELERATE,    /* cos((t + 1)π)/2 + 0.5 */
  SPONGY_N_CURVES
} SpongyCurve;

float  spongy_curve               (SpongyCurve c, float t);

/* ---- a list ---------------------------------------------------------------------------------------------------- */

/* A list along one axis: drag with touch slop, the stretch at both ends (pulling, catching a receding stretch, a drag
 * back shrinking it), the fling with a stretch absorbing its leftover speed at the end, and flinging out of a
 * stretch. offset 0 is the top (the left, for a sideways list, whose "top" edge is the left one); max = content
 * height - view height (widths, sideways). */
typedef struct {
  float  offset, max_offset;
  float  view_height;
  SpongyEdge top, bottom;
  SpongyFling fling;
  SpongyVelocity vel;
  bool   down, dragging, flinging;
  float  last_y, down_y;
  int    slop, min_fling, max_fling;   /* px; from the density unless set */
} SpongyList;

void   spongy_list_init           (SpongyList *l, float view_height, float content_height);
void   spongy_list_set_size       (SpongyList *l, float view_height, float content_height);
void   spongy_list_scroll_to      (SpongyList *l, float offset);
void   spongy_list_down           (SpongyList *l, float y, double t_ms);
/* true once the drag has passed the slop and scrolls the list (a parent should then let go of it) */
bool   spongy_list_move           (SpongyList *l, float y, double t_ms);
void   spongy_list_up             (SpongyList *l, double t_ms);
void   spongy_list_cancel         (SpongyList *l, double t_ms);
/* A fling that no drag started (a touchpad's kinetic scroll): px/s, toward the end positive */
void   spongy_list_fling          (SpongyList *l, float velocity, double t_ms);
/* One frame: advances the fling and the springs. *top_amount / *bottom_amount are the stretch amounts to draw
 * with (the top one anchored at the top, positive; the bottom one at the bottom, negative). Returns true while
 * anything still moves. */
bool   spongy_list_frame          (SpongyList *l, double now_ms, float *top_amount, float *bottom_amount);

/* Snapping: where a list let go comes to rest (a grid's columns, a pager's pages) */
typedef enum {
  SPONGY_SNAP_NONE,      /* where the fling stops */
  SPONGY_SNAP_ITEMS,     /* the multiple of the interval nearest to where the fling would have stopped */
  SPONGY_SNAP_PAGES,     /* the next multiple in the fling's direction, at most; the nearest without a fling */
} SpongySnap;

/* Called right after spongy_list_up or spongy_list_fling: aims the list at a multiple of `interval` px (the end
 * counts as one, so the last column or page can be partly one), by starting its fling again with the speed that
 * stops it exactly there, along the same curve. Nothing while a stretch shows: it springs back to an end, which is
 * a snap point. Returns where the list will rest. */
float  spongy_list_snap           (SpongyList *l, SpongySnap mode, float interval, double t_ms);

#ifdef __cplusplus
}
#endif
