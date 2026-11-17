/* libspongy (see spongy.h): the edge, the fling, the velocity, the springs, the curves and the list, in that order. */
#include "spongy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_E
#define M_E 2.71828182845904523536
#endif

static double density = 3.0;

void
spongy_set_density (double d)
{
  if (d > 0)
    density = d;
}

double
spongy_density (void)
{
  return density;
}

static int
sgn (double x)
{
  return (x > 0) - (x < 0);
}

/* ---- the stretch edge ------------------------------------------------------------------------------------------ */

#define NATURAL_FREQUENCY            24.657
#define DAMPING_RATIO                0.98
#define ABSORB_VELOCITY_ADJUST       13.0
#define LINEAR_STRETCH_INTENSITY     0.016f
#define EXP_STRETCH_INTENSITY        0.016f
#define SCROLL_DIST_AFFECTED_BY_EXP  0.33f
#define LINEAR_VELOCITY_TAKE_OVER    200.0
#define LINEAR_DISTANCE_TAKE_OVER    8.0
#define VELOCITY_THRESHOLD           0.01
#define VALUE_THRESHOLD              0.001
#define NON_ZERO_EPSILON             0.00004f
#define INTERPOLATION_STRENGTH       0.7f

void
spongy_edge_init (SpongyEdge *e, float size)
{
  memset (e, 0, sizeof *e);
  e->size = size;
}

void
spongy_edge_set_size (SpongyEdge *e, float size)
{
  e->size = size;
}

void
spongy_edge_finish (SpongyEdge *e)
{
  e->state = SPONGY_EDGE_IDLE;
  e->distance = 0;
  e->velocity = 0;
}

bool
spongy_edge_is_finished (const SpongyEdge *e)
{
  return e->state == SPONGY_EDGE_IDLE;
}

void
spongy_edge_pull (SpongyEdge *e, float delta, double now)
{
  if (e->state != SPONGY_EDGE_PULL)
    e->pull_distance = e->distance;      /* catches a receding stretch where it is */
  e->state = SPONGY_EDGE_PULL;
  e->t_ms = now;
  e->pull_distance += delta;
  if (e->pull_distance > 1.f)
    e->pull_distance = 1.f;
  e->distance = fmaxf (0.f, e->pull_distance);
  e->velocity = 0;
  if (e->distance == 0.f)
    e->state = SPONGY_EDGE_IDLE;
}

float
spongy_edge_pull_distance (SpongyEdge *e, float delta, double now)
{
  float fin = fmaxf (0.f, delta + e->distance), d = fin - e->distance;
  if (d == 0.f && e->distance == 0.f)
    return 0.f;
  spongy_edge_pull (e, d, now);
  return d;
}

void
spongy_edge_release (SpongyEdge *e, double now)
{
  e->pull_distance = 0;
  if (e->state != SPONGY_EDGE_PULL)
    return;
  e->state = SPONGY_EDGE_RECEDE;
  e->velocity = 0;
  e->t_ms = now;
}

void
spongy_edge_absorb (SpongyEdge *e, int v, double now)
{
  e->state = SPONGY_EDGE_RECEDE;
  e->velocity = (float) (v * ABSORB_VELOCITY_ADJUST);
  e->t_ms = now;
}

static void
edge_spring (SpongyEdge *e, double now)
{
  double dt = (now - e->t_ms) / 1000.0;
  if (dt < 0.001)
    return;
  e->t_ms = now;
  double H = e->size, d = e->distance, v = e->velocity;
  if (H <= 0) {
    spongy_edge_finish (e);
    return;
  }
  /* the last few pixels at a straight 200 px/s, so it lands instead of creeping */
  if (fabs (v) <= LINEAR_VELOCITY_TAKE_OVER && fabs (d * H) < LINEAR_DISTANCE_TAKE_OVER && sgn (v) == -sgn (d)) {
    v = sgn (v) * LINEAR_VELOCITY_TAKE_OVER;
    double td = d + v * dt / H;
    if (sgn (td) != sgn (d)) {
      e->distance = 0;
      e->velocity = 0;
    } else {
      e->distance = (float) td;
      e->velocity = (float) v;
    }
    return;
  }
  const double wn = NATURAL_FREQUENCY, z = DAMPING_RATIO, wd = wn * sqrt (1 - z * z);
  double x0 = d * H, s = (z * wn * x0 + v) / wd, E = exp (-z * wn * dt);
  double x = E * (x0 * cos (wd * dt) + s * sin (wd * dt));
  double vv = x * -wn * z + E * (-wd * x0 * sin (wd * dt) + wd * s * cos (wd * dt));
  e->distance = (float) (x / H);
  e->velocity = (float) vv;
  if (e->distance > 1.f) {
    e->distance = 1.f;
    e->velocity = 0;
  }
  double xp = e->distance * H;
  if (xp < 0 || (fabs (e->velocity) < VELOCITY_THRESHOLD && xp < VALUE_THRESHOLD)) {
    e->distance = 0;
    e->velocity = 0;
  }
}

float
spongy_edge_damp (float x)
{
  float o = fabsf (x), s = x > 0 ? 1.f : -1.f;
  return s * (LINEAR_STRETCH_INTENSITY * o +
              EXP_STRETCH_INTENSITY * (1.f - expf (-o * (float) (M_E / SCROLL_DIST_AFFECTED_BY_EXP))));
}

float
spongy_edge_frame (SpongyEdge *e, double now, bool far_edge, bool *animating)
{
  if (e->state == SPONGY_EDGE_RECEDE)
    edge_spring (e, now);
  float a = 0.f;
  if (e->distance != 0.f)
    a = spongy_edge_damp (fminf (1.f, e->distance)) * (far_edge ? -1.f : 1.f);
  bool last = false;
  if (e->state == SPONGY_EDGE_RECEDE && e->distance == 0.f && e->velocity == 0.f) {
    e->state = SPONGY_EDGE_IDLE;
    last = true;          /* one more frame, to draw it unstretched */
  }
  if (animating)
    *animating = e->state != SPONGY_EDGE_IDLE || last;
  return fabsf (a) <= NON_ZERO_EPSILON ? 0.f : a;
}

/* the stretch shader's mapping, the affected distance being the whole size */
float
spongy_stretch_map (float p, float o, float dim)
{
  if (dim <= 0)
    return p;
  float stretched = dim / (1 + fabsf (o));
  if (o > 0) {
    float off = dim - p;
    float var = 1.f + (off / dim - 1.f) * INTERPOLATION_STRENGTH;
    return stretched - off / (1.f + o * var);
  }
  if (o < 0) {
    float off = p;
    float var = 1.f + (off / dim - 1.f) * INTERPOLATION_STRENGTH;
    return dim - (stretched - off / (1.f - o * var));
  }
  return p;
}

float
spongy_stretch_unmap (float src, float o, float dim)
{
  if (o == 0 || dim <= 0)
    return src;
  /* monotonic in p: bisect */
  float lo = -dim, hi = 2 * dim;
  for (int i = 0; i < 40; i++) {
    float mid = (lo + hi) / 2;
    if (spongy_stretch_map (mid, o, dim) < src)
      lo = mid;
    else
      hi = mid;
  }
  return (lo + hi) / 2;
}

/* ---- fling: a spline that slows down as friction would --------------------------------------------------------- */

#define SCROLL_FRICTION   0.015                    /* the scroll friction */
#define INFLEXION         0.35f
#define START_TENSION     0.5f
#define END_TENSION       1.0f
#define NB_SAMPLES        100
#define GRAVITY_EARTH     9.80665

static float spline_position[NB_SAMPLES + 1], spline_time[NB_SAMPLES + 1];
static bool spline_ready;

static double
deceleration_rate (void)
{
  return log (0.78) / log (0.9);
}

static void
spline_init (void)
{
  if (spline_ready)
    return;
  const float P1 = START_TENSION * INFLEXION, P2 = 1.0f - END_TENSION * (1.0f - INFLEXION);
  float x_min = 0.0f, y_min = 0.0f;
  for (int i = 0; i < NB_SAMPLES; i++) {
    const float alpha = (float) i / NB_SAMPLES;
    float x_max = 1.0f, x, tx, coef;
    for (;;) {
      x = x_min + (x_max - x_min) / 2.0f;
      coef = 3.0f * x * (1.0f - x);
      tx = coef * ((1.0f - x) * P1 + x * P2) + x * x * x;
      if (fabsf (tx - alpha) < 1E-5)
        break;
      if (tx > alpha)
        x_max = x;
      else
        x_min = x;
    }
    spline_position[i] = coef * ((1.0f - x) * START_TENSION + x) + x * x * x;
    float y_max = 1.0f, y, dy;
    for (;;) {
      y = y_min + (y_max - y_min) / 2.0f;
      coef = 3.0f * y * (1.0f - y);
      dy = coef * ((1.0f - y) * START_TENSION + y) + y * y * y;
      if (fabsf (dy - alpha) < 1E-5)
        break;
      if (dy > alpha)
        y_max = y;
      else
        y_min = y;
    }
    spline_time[i] = coef * ((1.0f - y) * P1 + y * P2) + y * y * y;
  }
  spline_position[NB_SAMPLES] = spline_time[NB_SAMPLES] = 1.0f;
  spline_ready = true;
}

static double
physical_coeff (void)
{
  double ppi = density * 160.0;
  return GRAVITY_EARTH * 39.37 * ppi * 0.84;      /* 0.84: a tuning factor, the fling's feel */
}

static double
spline_deceleration (float velocity)
{
  return log (INFLEXION * fabs (velocity) / (SCROLL_FRICTION * physical_coeff ()));
}

double
spongy_fling_distance (float velocity)
{
  if (velocity == 0)
    return 0;
  double l = spline_deceleration (velocity), dr = deceleration_rate ();
  return SCROLL_FRICTION * physical_coeff () * exp (dr / (dr - 1.0) * l);
}

int
spongy_fling_duration (float velocity)
{
  if (velocity == 0)
    return 0;
  double l = spline_deceleration (velocity), dr = deceleration_rate ();
  return (int) (1000.0 * exp (l / (dr - 1.0)));
}

float
spongy_fling_velocity (double distance)
{
  if (distance <= 0)
    return 0;
  /* distance = F c e^(dr / (dr - 1) l), where l = ln (INFLEXION v / (F c)) */
  double dr = deceleration_rate (), fc = SCROLL_FRICTION * physical_coeff ();
  double l = log (distance / fc) * (dr - 1.0) / dr;
  return (float) (fc * exp (l) / INFLEXION);
}

static void
adjust_duration (SpongyFling *f, float start, float old_final, float new_final)
{
  float old_distance = old_final - start, new_distance = new_final - start;
  if (old_distance == 0)
    return;
  float x = fabsf (new_distance / old_distance);
  int index = (int) (NB_SAMPLES * x);
  if (index < NB_SAMPLES) {
    float x_inf = (float) index / NB_SAMPLES, x_sup = (float) (index + 1) / NB_SAMPLES;
    float t_inf = spline_time[index], t_sup = spline_time[index + 1];
    float time_coef = t_inf + (x - x_inf) / (x_sup - x_inf) * (t_sup - t_inf);
    f->duration = (int) (f->duration * time_coef);
  }
}

void
spongy_fling_start (SpongyFling *f, float start, float velocity, float min, float max, double now)
{
  spline_init ();
  memset (f, 0, sizeof *f);
  f->velocity = f->curr_velocity = velocity;
  f->start_ms = now;
  f->position = f->start = start;
  double total = 0;
  if (velocity != 0) {
    f->duration = f->spline_duration = spongy_fling_duration (velocity);
    total = spongy_fling_distance (velocity);
  }
  f->distance = (float) (total * sgn (velocity));
  f->final = start + f->distance;
  if (f->final < min) {
    adjust_duration (f, f->start, f->final, min);
    f->final = min;
  }
  if (f->final > max) {
    adjust_duration (f, f->start, f->final, max);
    f->final = max;
  }
  f->finished = f->duration <= 0;
}

bool
spongy_fling_update (SpongyFling *f, double now)
{
  if (f->finished)
    return false;
  double t_ms = now - f->start_ms;
  if (t_ms <= 0)
    return true;
  if (t_ms > f->duration) {
    f->position = f->final;
    f->curr_velocity = 0;
    f->finished = true;
    return false;
  }
  float t = (float) (t_ms / f->spline_duration);
  int index = (int) (NB_SAMPLES * t);
  float distance_coef = 1.f, velocity_coef = 0.f;
  if (index < NB_SAMPLES) {
    float t_inf = (float) index / NB_SAMPLES, t_sup = (float) (index + 1) / NB_SAMPLES;
    float d_inf = spline_position[index], d_sup = spline_position[index + 1];
    velocity_coef = (d_sup - d_inf) / (t_sup - t_inf);
    distance_coef = d_inf + (t - t_inf) * velocity_coef;
  }
  f->position = f->start + distance_coef * f->distance;
  f->curr_velocity = velocity_coef * f->distance / f->spline_duration * 1000.0f;
  return true;
}

void
spongy_fling_abort (SpongyFling *f)
{
  f->finished = true;
  f->curr_velocity = 0;
}

/* ---- velocity: a least-squares fit of degree 2, no weighting --------------------------------------------------- */

#define HORIZON_MS            100.0
#define ASSUME_STOPPED_MS     40.0

void
spongy_velocity_clear (SpongyVelocity *v)
{
  memset (v, 0, sizeof *v);
}

void
spongy_velocity_add (SpongyVelocity *v, double t, float x, float y)
{
  if (v->n) {
    int last = (v->head + SPONGY_VELOCITY_HISTORY - 1) % SPONGY_VELOCITY_HISTORY;
    if (t >= v->t[last] + ASSUME_STOPPED_MS)
      spongy_velocity_clear (v);       /* no movement for too long: every pointer stopped */
  }
  v->t[v->head] = t;
  v->x[v->head] = x;
  v->y[v->head] = y;
  v->head = (v->head + 1) % SPONGY_VELOCITY_HISTORY;
  if (v->n < SPONGY_VELOCITY_HISTORY)
    v->n++;
}

/* the slope at t = 0 of the least-squares fit of degree <= 2 to (t[i], x[i]) */
static float
lsq_slope (const double *t, const float *x, int m)
{
  if (m < 2)
    return 0;
  if (m == 2) {
    double dt = t[0] - t[1];
    return dt != 0 ? (float) ((x[0] - x[1]) / dt) : 0;
  }
  /* solveUnweightedLeastSquaresDeg2 */
  double sxi = 0, sxiyi = 0, syi = 0, sxi2 = 0, sxi3 = 0, sxi2yi = 0, sxi4 = 0;
  for (int i = 0; i < m; i++) {
    double xi = t[i], yi = x[i], xi2 = xi * xi, xi3 = xi2 * xi, xi4 = xi3 * xi;
    sxi += xi;
    sxi2 += xi2;
    sxiyi += xi * yi;
    sxi2yi += xi2 * yi;
    syi += yi;
    sxi3 += xi3;
    sxi4 += xi4;
  }
  double Sxx = sxi2 - sxi * sxi / m;
  double Sxy = sxiyi - sxi * syi / m;
  double Sxx2 = sxi3 - sxi * sxi2 / m;
  double Sx2y = sxi2yi - sxi2 * syi / m;
  double Sx2x2 = sxi4 - sxi2 * sxi2 / m;
  double denom = Sxx * Sx2x2 - Sxx2 * Sxx2;
  if (denom == 0) {
    double dt = t[0] - t[m - 1];
    return dt != 0 ? (float) ((x[0] - x[m - 1]) / dt) : 0;
  }
  return (float) ((Sxy * Sx2x2 - Sx2y * Sxx2) / denom);
}

void
spongy_velocity_get (const SpongyVelocity *v, double now, float *vx, float *vy)
{
  *vx = *vy = 0;
  if (!v->n)
    return;
  int newest = (v->head + SPONGY_VELOCITY_HISTORY - 1) % SPONGY_VELOCITY_HISTORY;
  if (now - v->t[newest] > ASSUME_STOPPED_MS)
    return;                          /* stopped before lifting: no fling */
  double t[SPONGY_VELOCITY_HISTORY];
  float xs[SPONGY_VELOCITY_HISTORY], ys[SPONGY_VELOCITY_HISTORY];
  int m = 0;
  for (int k = 0; k < v->n; k++) {
    int i = (newest - k + SPONGY_VELOCITY_HISTORY) % SPONGY_VELOCITY_HISTORY;
    double age = v->t[newest] - v->t[i];
    if (age > HORIZON_MS)
      break;
    t[m] = -age / 1000.0;
    xs[m] = v->x[i];
    ys[m] = v->y[i];
    m++;
  }
  *vx = lsq_slope (t, xs, m);
  *vy = lsq_slope (t, ys, m);
}

/* ---- springs --------------------------------------------------------------------------------------------------- */

#define THRESHOLD_MULTIPLIER           0.75
#define VELOCITY_THRESHOLD_MULTIPLIER  (1000.0 / 16.0)

void
spongy_spring_init (SpongySpring *s, double stiffness, double damping, double min_visible)
{
  memset (s, 0, sizeof *s);
  s->stiffness = stiffness;
  s->damping = damping;
  s->min = -INFINITY;
  s->max = INFINITY;
  s->value_threshold = min_visible * THRESHOLD_MULTIPLIER;
  s->velocity_threshold = s->value_threshold * VELOCITY_THRESHOLD_MULTIPLIER;
}

void
spongy_spring_animate (SpongySpring *s, double value, double velocity, double final, double now)
{
  s->value = value;
  s->velocity = velocity;
  s->final = final;
  s->last_ms = now;
  s->running = true;
}

void
spongy_spring_set_final (SpongySpring *s, double final)
{
  s->final = final;
}

void
spongy_spring_step (double stiffness, double damping, double final, double *x, double *v, double dt_ms)
{
  double wn = sqrt (stiffness), dt = dt_ms / 1000.0;
  double d0 = *x - final, v0 = *v, disp, vel;
  if (damping > 1) {
    double gp = -damping * wn + wn * sqrt (damping * damping - 1);
    double gm = -damping * wn - wn * sqrt (damping * damping - 1);
    double a = d0 - (gm * d0 - v0) / (gm - gp);
    double b = (gm * d0 - v0) / (gm - gp);
    disp = a * exp (gm * dt) + b * exp (gp * dt);
    vel = a * gm * exp (gm * dt) + b * gp * exp (gp * dt);
  } else if (damping == 1) {
    double a = d0, b = v0 + wn * d0;
    disp = (a + b * dt) * exp (-wn * dt);
    vel = (a + b * dt) * exp (-wn * dt) * -wn + b * exp (-wn * dt);
  } else {
    double wd = wn * sqrt (1 - damping * damping);
    double c = d0, sn = (1 / wd) * (damping * wn * d0 + v0);
    disp = exp (-damping * wn * dt) * (c * cos (wd * dt) + sn * sin (wd * dt));
    vel = disp * -wn * damping + exp (-damping * wn * dt) * (-wd * c * sin (wd * dt) + wd * sn * cos (wd * dt));
  }
  *x = disp + final;
  *v = vel;
}

bool
spongy_spring_update (SpongySpring *s, double now)
{
  if (!s->running)
    return false;
  double dt = now - s->last_ms;
  s->last_ms = now;
  if (dt <= 0)
    return true;
  spongy_spring_step (s->stiffness, s->damping, s->final, &s->value, &s->velocity, dt);
  if (s->value < s->min)
    s->value = s->min;
  if (s->value > s->max)
    s->value = s->max;
  if (fabs (s->velocity) < s->velocity_threshold && fabs (s->value - s->final) < s->value_threshold) {
    s->value = s->final;
    s->velocity = 0;
    s->running = false;
    return false;
  }
  return true;
}

/* ---- easing curves: any cubic path ----------------------------------------------------------------------------- */

void
spongy_interp_cubic (SpongyInterp *i, float x1, float y1, float x2, float y2)
{
  i->n = 1;
  float *p = i->p[0];
  p[0] = x1; p[1] = y1; p[2] = x2; p[3] = y2; p[4] = 1; p[5] = 1;
}

bool
spongy_interp_path (SpongyInterp *i, const char *s)
{
  /* "M 0,0 C x1,y1 x2,y2 x,y C …": numbers separated by spaces or commas */
  i->n = 0;
  float px = 0, py = 0;
  char cmd = 0;
  const char *q = s;
  while (*q) {
    while (*q == ' ' || *q == ',' || *q == '\n' || *q == '\t')
      q++;
    if (!*q)
      break;
    if (*q == 'M' || *q == 'C' || *q == 'L') {
      cmd = *q++;
      continue;
    }
    float v[6];
    int need = cmd == 'C' ? 6 : 2;
    for (int k = 0; k < need; k++) {
      while (*q == ' ' || *q == ',')
        q++;
      char *end;
      v[k] = strtof (q, &end);
      if (end == q)
        return false;
      q = end;
    }
    if (cmd == 'M') {
      px = v[0];
      py = v[1];
    } else if (i->n < 8) {
      float *p = i->p[i->n++];
      if (cmd == 'L') {     /* a line as a cubic */
        p[0] = px + (v[0] - px) / 3; p[1] = py + (v[1] - py) / 3;
        p[2] = px + 2 * (v[0] - px) / 3; p[3] = py + 2 * (v[1] - py) / 3;
        p[4] = v[0]; p[5] = v[1];
      } else {
        memcpy (p, v, sizeof v);
      }
      px = p[4];
      py = p[5];
    }
  }
  return i->n > 0;
}

static float
bezier (float a, float b, float c, float d, float t)
{
  float u = 1 - t;
  return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d;
}

float
spongy_interp_get (const SpongyInterp *in, float t)
{
  if (t <= 0)
    return 0;
  if (t >= 1)
    return 1;
  float x0 = 0, y0 = 0;
  for (int k = 0; k < in->n; k++) {
    const float *p = in->p[k];
    if (t <= p[4] || k == in->n - 1) {
      /* solve x(u) = t on this segment: bisection, x is monotonic for a valid interpolator */
      float lo = 0, hi = 1, u = 0.5f;
      for (int it = 0; it < 32; it++) {
        u = (lo + hi) / 2;
        if (bezier (x0, p[0], p[2], p[4], u) < t)
          lo = u;
        else
          hi = u;
      }
      return bezier (y0, p[1], p[3], p[5], u);
    }
    x0 = p[4];
    y0 = p[5];
  }
  return t;
}

float
spongy_curve (SpongyCurve c, float t)
{
  static SpongyInterp cubic[SPONGY_N_CURVES];
  static bool ready;
  if (!ready) {
    spongy_interp_cubic (&cubic[SPONGY_FAST_OUT_SLOW_IN], 0.4f, 0, 0.2f, 1);
    spongy_interp_cubic (&cubic[SPONGY_FAST_OUT_LINEAR_IN], 0.4f, 0, 1, 1);
    spongy_interp_cubic (&cubic[SPONGY_LINEAR_OUT_SLOW_IN], 0, 0, 0.2f, 1);
    spongy_interp_path (&cubic[SPONGY_EMPHASIZED],
                        "M 0,0 C 0.05, 0, 0.133333, 0.06, 0.166666, 0.4 C 0.208333, 0.82, 0.25, 1, 1, 1");
    spongy_interp_cubic (&cubic[SPONGY_EMPHASIZED_ACCELERATE], 0.3f, 0, 0.8f, 0.15f);
    spongy_interp_cubic (&cubic[SPONGY_EMPHASIZED_DECELERATE], 0.05f, 0.7f, 0.1f, 1);
    spongy_interp_cubic (&cubic[SPONGY_STANDARD], 0.2f, 0, 0, 1);
    spongy_interp_cubic (&cubic[SPONGY_STANDARD_ACCELERATE], 0.3f, 0, 1, 1);
    spongy_interp_cubic (&cubic[SPONGY_STANDARD_DECELERATE], 0, 0, 0, 1);
    ready = true;
  }
  if (t <= 0)
    return 0;
  if (t >= 1)
    return 1;
  switch (c) {
  case SPONGY_LINEAR: return t;
  case SPONGY_DECELERATE_QUINT: return 1 - powf (1 - t, 5);
  case SPONGY_DECELERATE_CUBIC: return 1 - powf (1 - t, 3);
  case SPONGY_DECELERATE: return 1 - (1 - t) * (1 - t);
  case SPONGY_ACCELERATE: return t * t;
  case SPONGY_ACCELERATE_DECELERATE: return (float) (cos ((t + 1) * M_PI) / 2.0 + 0.5);
  default: return spongy_interp_get (&cubic[c], t);
  }
}

/* ---- a list: drag, fling and stretch together ------------------------------------------------------------------ */

#define FLING_DESTRETCH_FACTOR 4.0f

static bool can_scroll_up (SpongyList *l) { return l->offset > 0; }
static bool can_scroll_down (SpongyList *l) { return l->offset < l->max_offset; }

void
spongy_list_init (SpongyList *l, float view_height, float content_height)
{
  memset (l, 0, sizeof *l);
  l->fling.finished = true;
  l->slop = (int) lround (8 * density);
  l->min_fling = (int) lround (50 * density);
  l->max_fling = (int) lround (8000 * density);
  spongy_edge_init (&l->top, view_height);
  spongy_edge_init (&l->bottom, view_height);
  spongy_list_set_size (l, view_height, content_height);
}

void
spongy_list_set_size (SpongyList *l, float view_height, float content_height)
{
  l->view_height = view_height;
  l->max_offset = fmaxf (0, content_height - view_height);
  if (l->offset > l->max_offset)
    l->offset = l->max_offset;
  spongy_edge_set_size (&l->top, view_height);
  spongy_edge_set_size (&l->bottom, view_height);
}

void
spongy_list_scroll_to (SpongyList *l, float offset)
{
  l->offset = fminf (fmaxf (0, offset), l->max_offset);
  spongy_fling_abort (&l->fling);
  l->flinging = false;
}

/* scrolls by dy, returns what was not taken: how far past an end it would have gone, exactly 0 inside (dy − what
 * the offset moved is float noise that reads as an end reached, for fractional moves) */
static float
scroll_by (SpongyList *l, float dy)
{
  float to = l->offset + dy;
  if (to < 0) {
    l->offset = 0;
    return to;
  }
  if (to > l->max_offset) {
    l->offset = l->max_offset;
    return to - l->max_offset;
  }
  l->offset = to;
  return 0;
}

void
spongy_list_down (SpongyList *l, float y, double t)
{
  spongy_velocity_clear (&l->vel);
  spongy_velocity_add (&l->vel, t, 0, y);
  l->down = true;
  l->dragging = false;
  l->last_y = l->down_y = y;
  if (l->flinging) {
    /* a touch stops a fling and drags at once, no slop */
    spongy_fling_abort (&l->fling);
    l->flinging = false;
    l->dragging = true;
  }
  /* a touch catches a receding stretch where it is, and drags it without slop */
  if (l->top.distance != 0 && !can_scroll_up (l)) {
    spongy_edge_pull_distance (&l->top, 0, t);
    l->dragging = true;
  }
  if (l->bottom.distance != 0 && !can_scroll_down (l)) {
    spongy_edge_pull_distance (&l->bottom, 0, t);
    l->dragging = true;
  }
}

/* A list that moves in whole pixels can round what a stretch took to whole pixels. Ours are
 * fractional: an edge that took all of a move took exactly it, or the rounding's leftover scrolls the list off its
 * end and the stretch is let go. EPS: float noise in a pull, in units of the size. */
#define PULL_EPS 1e-6f

/* A drag while stretched goes to the stretch first, and only what is left of it (once a drag
 * back has undone the stretch) scrolls; what it took, px */
static float
release_glow (SpongyList *l, float dy, double t)
{
  float H = l->view_height, consumed = 0;
  if (H <= 0)
    return 0;
  if (l->top.distance != 0) {
    if (can_scroll_up (l))
      spongy_edge_release (&l->top, t);
    else {
      float want = -dy / H, got = spongy_edge_pull_distance (&l->top, want, t);
      consumed = fabsf (got - want) < PULL_EPS ? dy : -got * H;
      if (l->top.distance == 0)
        spongy_edge_release (&l->top, t);
    }
  } else if (l->bottom.distance != 0) {
    if (can_scroll_down (l))
      spongy_edge_release (&l->bottom, t);
    else {
      float want = dy / H, got = spongy_edge_pull_distance (&l->bottom, want, t);
      consumed = fabsf (got - want) < PULL_EPS ? dy : got * H;
      if (l->bottom.distance == 0)
        spongy_edge_release (&l->bottom, t);
    }
  }
  return consumed;
}

bool
spongy_list_move (SpongyList *l, float y, double t)
{
  if (!l->down)
    return false;
  spongy_velocity_add (&l->vel, t, 0, y);
  float dy = l->last_y - y;               /* > 0: the finger moved up, toward the end */
  if (!l->dragging) {
    dy = dy > 0 ? fmaxf (0, dy - l->slop) : fminf (0, dy + l->slop);
    if (dy == 0)
      return false;
    l->dragging = true;
  }
  l->last_y = y;
  dy -= release_glow (l, dy, t);
  float rest = scroll_by (l, dy);
  float H = l->view_height;
  /* what the list could not take stretches the edge */
  if (H > 0) {
    if (rest < 0)
      spongy_edge_pull_distance (&l->top, -rest / H, t);
    else if (rest > 0)
      spongy_edge_pull_distance (&l->bottom, rest / H, t);
  }
  /* a scroll away from a stretched edge lets the stretch go */
  if (!spongy_edge_is_finished (&l->top) && dy > 0)
    spongy_edge_release (&l->top, t);
  if (!spongy_edge_is_finished (&l->bottom) && dy < 0)
    spongy_edge_release (&l->bottom, t);
  return true;
}

static bool
should_absorb (SpongyEdge *e, float velocity, float size)
{
  if (velocity > 0)
    return true;
  return spongy_fling_distance (-velocity) < e->distance * size;
}

static void
release_glows (SpongyList *l, double t)
{
  spongy_edge_release (&l->top, t);
  spongy_edge_release (&l->bottom, t);
}

void
spongy_list_up (SpongyList *l, double t)
{
  if (!l->down)
    return;
  l->down = false;
  float vx, vy;
  spongy_velocity_get (&l->vel, t, &vx, &vy);
  float velocity = -vy;                   /* toward the end positive */
  velocity = fmaxf (-l->max_fling, fminf (velocity, l->max_fling));
  if (!l->dragging || fabsf (velocity) < l->min_fling)
    velocity = 0;
  if (velocity != 0) {
    /* flinging while stretched moves the stretch, not the list */
    float fling = velocity;
    bool absorbed = false;
    if (l->top.distance != 0) {
      if (should_absorb (&l->top, -velocity, l->view_height)) {
        spongy_edge_absorb (&l->top, (int) -velocity, t);
        absorbed = true;
      }
    } else if (l->bottom.distance != 0) {
      if (should_absorb (&l->bottom, velocity, l->view_height)) {
        spongy_edge_absorb (&l->bottom, (int) velocity, t);
        absorbed = true;
      }
    }
    if (!absorbed) {
      spongy_fling_start (&l->fling, 0, fling, -INFINITY, INFINITY, t);
      l->flinging = !l->fling.finished;
    }
  }
  l->dragging = false;
  release_glows (l, t);
}

void
spongy_list_cancel (SpongyList *l, double t)
{
  l->down = false;
  l->dragging = false;
  release_glows (l, t);
}

void
spongy_list_fling (SpongyList *l, float velocity, double t)
{
  velocity = fmaxf (-l->max_fling, fminf (velocity, l->max_fling));
  if (fabsf (velocity) < l->min_fling)
    return;
  spongy_fling_start (&l->fling, 0, velocity, -INFINITY, INFINITY, t);
  l->flinging = !l->fling.finished;
}

/* A fling out of a stretch first undoes it, four times as fast as it scrolls */
static float
consume_fling_in_stretch (SpongyList *l, float unconsumed, double t)
{
  float size = l->view_height;
  if (size <= 0)
    return unconsumed;
  /* the stretch takes all of the step until it is undone (exactly all: see PULL_EPS); the rest scrolls */
  if (unconsumed > 0 && l->top.distance != 0) {
    float delta = -unconsumed * FLING_DESTRETCH_FACTOR / size, got = spongy_edge_pull_distance (&l->top, delta, t);
    if (fabsf (got - delta) < PULL_EPS)
      return 0;
    spongy_edge_finish (&l->top);
    return unconsumed + size / FLING_DESTRETCH_FACTOR * got;
  }
  if (unconsumed < 0 && l->bottom.distance != 0) {
    float delta = unconsumed * FLING_DESTRETCH_FACTOR / size, got = spongy_edge_pull_distance (&l->bottom, delta, t);
    if (fabsf (got - delta) < PULL_EPS)
      return 0;
    spongy_edge_finish (&l->bottom);
    return unconsumed - size / FLING_DESTRETCH_FACTOR * got;
  }
  return unconsumed;
}

bool
spongy_list_frame (SpongyList *l, double now, float *top_amount, float *bottom_amount)
{
  bool more = false;
  if (l->flinging) {
    float before = l->fling.position;
    bool going = spongy_fling_update (&l->fling, now);
    float dy = l->fling.position - before;
    dy = consume_fling_in_stretch (l, dy, now);
    float rest = scroll_by (l, dy);
    bool done = !going || rest != 0;
    if (done) {
      if (rest != 0) {
        /* the end reached with speed left: the stretch takes it */
        int v = (int) fabsf (l->fling.curr_velocity);
        if (rest < 0 && spongy_edge_is_finished (&l->top))
          spongy_edge_absorb (&l->top, v, now);
        else if (rest > 0 && spongy_edge_is_finished (&l->bottom))
          spongy_edge_absorb (&l->bottom, v, now);
      }
      spongy_fling_abort (&l->fling);
      l->flinging = false;
    } else {
      more = true;
    }
  }
  bool at = false, ab = false;
  float a = spongy_edge_frame (&l->top, now, false, &at);
  float b = spongy_edge_frame (&l->bottom, now, true, &ab);
  if (top_amount)
    *top_amount = a;
  if (bottom_amount)
    *bottom_amount = b;
  return more || at || ab || l->down;
}

/* ---- snapping -------------------------------------------------------------------------------------------------- */

float
spongy_list_snap (SpongyList *l, SpongySnap mode, float interval, double t)
{
  if (mode == SPONGY_SNAP_NONE || interval <= 0 || l->down || l->max_offset <= 0)
    return l->offset;
  /* a stretch springs back to its end */
  if (l->top.distance != 0 || l->bottom.distance != 0)
    return l->offset;
  float velocity = l->flinging ? l->fling.velocity : 0;
  float rest = l->offset + (l->flinging ? l->fling.final - l->fling.position : 0);
  float target;
  if (mode == SPONGY_SNAP_PAGES && velocity != 0) {
    /* one page on in the fling's direction from where it is (a list let go exactly on a page goes to the next) */
    float page = l->offset / interval;
    target = (velocity > 0 ? ceilf (page + 1e-4f) : floorf (page - 1e-4f)) * interval;
  } else if (rest >= l->max_offset) {
    target = l->max_offset;
  } else if (rest <= 0) {
    target = 0;
  } else {
    /* the nearer of the multiples either side; the end is the last one */
    float lo = floorf (rest / interval) * interval, hi = fminf (lo + interval, l->max_offset);
    target = rest - lo <= hi - rest ? lo : hi;
  }
  target = fminf (fmaxf (target, 0), l->max_offset);
  float d = target - l->offset;
  spongy_fling_abort (&l->fling);
  l->flinging = false;
  if (fabsf (d) < 0.5f) {
    l->offset = target;
    return target;
  }
  spongy_fling_start (&l->fling, 0, d > 0 ? spongy_fling_velocity (d) : -spongy_fling_velocity (-d), -INFINITY,
                      INFINITY, t);
  /* exactly there: the speed was found for this distance, the curve keeps its shape */
  l->fling.distance = l->fling.final = d;
  l->flinging = !l->fling.finished;
  if (!l->flinging)
    l->offset = target;
  return target;
}
