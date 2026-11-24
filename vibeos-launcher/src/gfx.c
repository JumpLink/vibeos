/* The 2D renderer (see gfx.h): the drawing state, the instances and their batches, the uploads' queue and the
 * textures' end of life, the same for both backends (gfx-backend.h), and the choice between them. */
#include "gfx-backend.h"

#include <glib.h>
#include <math.h>
#include <string.h>

typedef struct {
  float m[4], t[2];
  float alpha;
  float clip[4];   /* screen space, x1 <= x0 = none */
} State;

typedef struct {
  GfxTex *tex;
  int     x, y, w, h;
  guint8 *data;      /* tightly packed */
} Upload;

typedef struct {
  GfxTex  *tex;
  uint32_t frame;    /* destroyed once frame passes this + the backend's frames in flight */
} Dead;

static struct {
  const GfxBackend     *b;
  uint32_t              frame;      /* frames ended so far */

  GArray               *uploads;    /* Upload */
  GArray               *dead;       /* Dead */
  GfxTex               *white;
  GfxTex               *blur_targets[4][6];
  int                   blur_size[4][2];

  /* the frame being recorded */
  GfxInst              *inst;       /* its instance memory */
  uint32_t              inst_used;
  bool                  in_pass;
  int                   pass_w, pass_h;
  GfxPush               push;
  GfxTex               *batch_tex;
  GfxPipe               batch_pipe;
  uint32_t              batch_first, batch_count;

  State                 state;
  GArray               *stack;      /* State */
  float                 hole[4];    /* gfx_occlude's, screen x0 y0 x1 y1; x1 <= x0 = none */
  bool                  count_fill;
  double                fill_blend, fill_opaque;
} G;

static void
state_identity (State *s)
{
  *s = (State) { .m = { 1, 0, 0, 1 }, .alpha = 1, .clip = { 0, 0, 0, 0 } };
}

/* VIBEOS_LAUNCHER_RENDERER=vulkan|gl takes that backend or none. Otherwise Vulkan on a GPU, then OpenGL ES (a GPU
 * with no Vulkan driver of its own, where the loader would only offer its software rasterizer), then Vulkan on
 * whatever the loader has. */
static bool
backend_init (struct wl_display *display, struct wl_surface *surface, int w, int h)
{
  const char *env = g_getenv ("VIBEOS_LAUNCHER_RENDERER");
  if (env && *env) {
    bool vk = !g_ascii_strcasecmp (env, "vulkan"), gl = !g_ascii_strcasecmp (env, "gl");
    if (!vk && !gl) {
      g_warning ("VIBEOS_LAUNCHER_RENDERER=%s: neither vulkan nor gl", env);
      return false;
    }
    G.b = vk ? &gfx_vk_backend : &gfx_gl_backend;
    return G.b->init (display, surface, w, h, true);
  }
  static const struct { const GfxBackend *b; bool allow_cpu; } order[] = {
    { &gfx_vk_backend, false }, { &gfx_gl_backend, true }, { &gfx_vk_backend, true },
  };
  for (guint i = 0; i < G_N_ELEMENTS (order); i++) {
    G.b = order[i].b;
    if (G.b->init (display, surface, w, h, order[i].allow_cpu))
      return true;
  }
  G.b = NULL;
  return false;
}

bool
gfx_init (struct wl_display *display, struct wl_surface *surface, int w, int h)
{
  if (!backend_init (display, surface, w, h))
    return false;
  g_message ("gfx: drawing with %s", G.b->name);
  G.uploads = g_array_new (FALSE, FALSE, sizeof (Upload));
  G.dead = g_array_new (FALSE, FALSE, sizeof (Dead));
  G.stack = g_array_new (FALSE, FALSE, sizeof (State));
  state_identity (&G.state);
  guint32 white = 0xffffffff;
  G.white = gfx_tex_from_bgra (1, 1, &white, 4);
  return G.white != NULL;
}

void
gfx_resize (int w, int h)
{
  G.b->resize (w, h);
}

void
gfx_finish (void)
{
  if (G.b)
    G.b->finish ();
}

const char *
gfx_backend_name (void)
{
  return G.b ? G.b->name : NULL;
}

GfxTex *
gfx_white (void)
{
  return G.white;
}

/* ---- textures ------------------------------------------------------------------------------------------------- */

GfxTex *
gfx_tex_new (int w, int h, GfxFormat format)
{
  GfxTex *t = G.b->tex_new (w, h, format, false);
  if (!t)
    g_warning ("gfx: cannot make a %dx%d texture", w, h);
  return t;
}

GfxTex *
gfx_target_new (int w, int h)
{
  GfxTex *t = G.b->tex_new (w, h, GFX_FORMAT_BGRA8, true);
  if (!t)
    g_warning ("gfx: cannot make a %dx%d target", w, h);
  return t;
}

void
gfx_tex_free (GfxTex *t)
{
  if (!t)
    return;
  for (guint i = 0; i < G.uploads->len; i++) {       /* a pending upload into it goes too */
    Upload *u = &g_array_index (G.uploads, Upload, i);
    if (u->tex == t) {
      g_free (u->data);
      g_array_remove_index (G.uploads, i--);
    }
  }
  Dead d = { t, G.frame };
  g_array_append_val (G.dead, d);
}

void
gfx_tex_write (GfxTex *t, int x, int y, int w, int h, const void *pixels, int stride)
{
  int bpp = t->format == GFX_FORMAT_R8 ? 1 : 4;
  gsize size = (gsize) w * h * bpp;
  if (size > G.b->max_upload) {
    /* too big to wait for a frame (a wallpaper): now */
    G.b->upload_now (t, x, y, w, h, pixels, stride);
    return;
  }
  Upload u = { t, x, y, w, h, g_malloc (size) };
  for (int r = 0; r < h; r++)
    memcpy (u.data + (gsize) r * w * bpp, (const guint8 *) pixels + (gsize) r * stride, (gsize) w * bpp);
  g_array_append_val (G.uploads, u);
}

GfxTex *
gfx_tex_from_bgra (int w, int h, const void *pixels, int stride)
{
  GfxTex *t = gfx_tex_new (w, h, GFX_FORMAT_BGRA8);
  if (t)
    gfx_tex_write (t, 0, 0, w, h, pixels, stride);
  return t;
}

static void
flush_uploads (void)
{
  guint done = 0;
  for (; done < G.uploads->len; done++) {
    Upload *u = &g_array_index (G.uploads, Upload, done);
    if (!G.b->upload (u->tex, u->x, u->y, u->w, u->h, u->data))
      break;                                  /* the rest next frame */
    g_free (u->data);
  }
  g_array_remove_range (G.uploads, 0, done);
}

static void
reap (void)
{
  for (guint i = 0; i < G.dead->len; i++) {
    Dead *d = &g_array_index (G.dead, Dead, i);
    if (G.frame >= d->frame + G.b->frames_in_flight + 1) {
      G.b->tex_destroy (d->tex);
      g_array_remove_index (G.dead, i--);
    }
  }
}

/* ---- frames and passes ---------------------------------------------------------------------------------------- */

bool
gfx_frame_begin (void)
{
  G.inst = G.b->frame_begin ();
  if (!G.inst)
    return false;
  reap ();
  G.inst_used = 0;
  return true;
}

/* the px of the pass a quad's fragments are shaded for (its clip is the shader's, not the rasterizer's: no less) */
static double
shaded (const GfxInst *q)
{
  bool edges = q->mode == GFX_MODE_PIECE;
  float w = edges ? q->w - q->x : q->w, h = edges ? q->h - q->y : q->h;
  if (q->m[1] != 0 || q->m[2] != 0)
    return fabs (q->m[0] * q->m[3] - q->m[1] * q->m[2]) * w * h;
  float x0 = q->m[0] * q->x + q->tx, x1 = q->m[0] * (q->x + w) + q->tx;
  float y0 = q->m[3] * q->y + q->ty, y1 = q->m[3] * (q->y + h) + q->ty;
  float sw = MIN (MAX (x0, x1), G.pass_w) - MAX (MIN (x0, x1), 0);
  float sh = MIN (MAX (y0, y1), G.pass_h) - MAX (MIN (y0, y1), 0);
  return sw > 0 && sh > 0 ? (double) sw * sh : 0;
}

static void
flush_batch (void)
{
  if (!G.batch_count)
    return;
  if (G.count_fill) {
    const GfxInst *q = &G.inst[G.batch_first];
    double px = 0;
    for (uint32_t i = 0; i < G.batch_count; i++)
      px += shaded (&q[i]);
    if (G.batch_pipe == GFX_PIPE_OPAQUE)
      G.fill_opaque += px;
    else
      G.fill_blend += px;
  }
  G.b->draw (G.batch_pipe, G.batch_tex, &G.push, G.batch_first, G.batch_count);
  G.batch_first += G.batch_count;
  G.batch_count = 0;
}

void
gfx_pass_begin (GfxTex *target, GfxColor c)
{
  flush_uploads ();          /* textures written since the last pass, glyphs among them, are ready for this one */
  int sw, sh;
  G.b->screen_size (&sw, &sh);
  G.pass_w = target ? target->w : sw;
  G.pass_h = target ? target->h : sh;
  float clear[4] = { c.r * c.a, c.g * c.a, c.b * c.a, c.a };
  G.b->pass_begin (target, G.pass_w, G.pass_h, clear);
  G.push.vp[0] = G.pass_w;
  G.push.vp[1] = G.pass_h;
  G.batch_tex = NULL;
  G.batch_first = G.inst_used;
  G.batch_count = 0;
  G.in_pass = true;
  state_identity (&G.state);
  g_array_set_size (G.stack, 0);
  memset (G.hole, 0, sizeof G.hole);
}

void
gfx_pass_end (void)
{
  flush_batch ();
  G.b->pass_end ();
  G.in_pass = false;
}

void
gfx_frame_end (void)
{
  G.b->frame_end ();
  G.frame++;
  G.inst = NULL;
}

int gfx_pass_width (void) { return G.pass_w; }
int gfx_pass_height (void) { return G.pass_h; }

/* ---- state ---------------------------------------------------------------------------------------------------- */

void
gfx_save (void)
{
  g_array_append_val (G.stack, G.state);
}

void
gfx_restore (void)
{
  if (G.stack->len) {
    G.state = g_array_index (G.stack, State, G.stack->len - 1);
    g_array_set_size (G.stack, G.stack->len - 1);
  }
}

void
gfx_translate (float x, float y)
{
  State *s = &G.state;
  s->t[0] += s->m[0] * x + s->m[1] * y;
  s->t[1] += s->m[2] * x + s->m[3] * y;
}

void
gfx_scale (float sx, float sy, float px, float py)
{
  gfx_translate (px, py);
  State *s = &G.state;
  s->m[0] *= sx; s->m[2] *= sx;
  s->m[1] *= sy; s->m[3] *= sy;
  gfx_translate (-px, -py);
}

void
gfx_rotate (float radians, float px, float py)
{
  gfx_translate (px, py);
  State *s = &G.state;
  float c = cosf (radians), n = sinf (radians);
  float m0 = s->m[0], m1 = s->m[1], m2 = s->m[2], m3 = s->m[3];
  s->m[0] = m0 * c + m1 * n; s->m[1] = -m0 * n + m1 * c;
  s->m[2] = m2 * c + m3 * n; s->m[3] = -m2 * n + m3 * c;
  gfx_translate (-px, -py);
}

void
gfx_opacity (float a)
{
  G.state.alpha *= a;
}

void
gfx_clip (float x0, float y0, float x1, float y1)
{
  State *s = &G.state;
  /* to screen space (axis-aligned transforms only) */
  float sx0 = s->m[0] * x0 + s->t[0], sy0 = s->m[3] * y0 + s->t[1];
  float sx1 = s->m[0] * x1 + s->t[0], sy1 = s->m[3] * y1 + s->t[1];
  if (sx1 < sx0) { float t = sx0; sx0 = sx1; sx1 = t; }
  if (sy1 < sy0) { float t = sy0; sy0 = sy1; sy1 = t; }
  if (s->clip[2] > s->clip[0]) {
    sx0 = MAX (sx0, s->clip[0]); sy0 = MAX (sy0, s->clip[1]);
    sx1 = MIN (sx1, s->clip[2]); sy1 = MIN (sy1, s->clip[3]);
    if (sx1 <= sx0) sx1 = sx0 + 0.001f;      /* empty, but still "a clip" */
    if (sy1 <= sy0) sy1 = sy0 + 0.001f;
  }
  s->clip[0] = sx0; s->clip[1] = sy0; s->clip[2] = sx1; s->clip[3] = sy1;
}

/* ---- fill (VIBEOS_STATS) -------------------------------------------------------------------------------------- */

void
gfx_count_fill (bool on)
{
  G.count_fill = on;
}

void
gfx_fill (double *blended, double *opaque)
{
  *blended = G.fill_blend;
  *opaque = G.fill_opaque;
  G.fill_blend = G.fill_opaque = 0;
}

/* ---- what will be covered ------------------------------------------------------------------------------------ */

void
gfx_occlude (float x0, float y0, float x1, float y1)
{
  G.hole[0] = x0; G.hole[1] = y0; G.hole[2] = x1; G.hole[3] = y1;
}

/* The parts of a rect of the current space outside the hole: the band above it, the one below and the two beside,
 * as many as there are (0: all of it is in the hole). -1: the hole leaves it whole, or the transform is no plain
 * scale and move. A part's edges along the hole lie on its whole pixels, so the parts meet without a seam. */
static int
around_hole (float x, float y, float w, float h, float out[4][4])
{
  const State *s = &G.state;
  if (G.hole[2] <= G.hole[0] || s->m[1] != 0 || s->m[2] != 0 || s->m[0] <= 0 || s->m[3] <= 0)
    return -1;
  float hx0 = (G.hole[0] - s->t[0]) / s->m[0], hx1 = (G.hole[2] - s->t[0]) / s->m[0];
  float hy0 = (G.hole[1] - s->t[1]) / s->m[3], hy1 = (G.hole[3] - s->t[1]) / s->m[3];
  float ix0 = MAX (x, hx0), ix1 = MIN (x + w, hx1), iy0 = MAX (y, hy0), iy1 = MIN (y + h, hy1);
  if (ix1 <= ix0 || iy1 <= iy0)
    return -1;
  int n = 0;
  if (iy0 > y)
    memcpy (out[n++], (float[4]) { x, y, w, iy0 - y }, sizeof out[0]);
  if (iy1 < y + h)
    memcpy (out[n++], (float[4]) { x, iy1, w, y + h - iy1 }, sizeof out[0]);
  if (ix0 > x)
    memcpy (out[n++], (float[4]) { x, iy0, ix0 - x, iy1 - iy0 }, sizeof out[0]);
  if (ix1 < x + w)
    memcpy (out[n++], (float[4]) { ix1, iy0, x + w - ix1, iy1 - iy0 }, sizeof out[0]);
  return n;
}

/* all of a rect of the current space in the hole, its antialiased edge too */
static bool
hidden (float x, float y, float w, float h)
{
  const State *s = &G.state;
  if (G.hole[2] <= G.hole[0] || s->m[1] != 0 || s->m[2] != 0)
    return false;
  float x0 = s->m[0] * x + s->t[0], x1 = s->m[0] * (x + w) + s->t[0];
  float y0 = s->m[3] * y + s->t[1], y1 = s->m[3] * (y + h) + s->t[1];
  return MIN (x0, x1) - 1 >= G.hole[0] && MAX (x0, x1) + 1 <= G.hole[2] &&
         MIN (y0, y1) - 1 >= G.hole[1] && MAX (y0, y1) + 1 <= G.hole[3];
}

/* ---- drawing -------------------------------------------------------------------------------------------------- */

GfxInst *
gfx_push (GfxTex *tex, GfxPipe pipe)
{
  if (!tex)
    tex = G.white;
  if (tex != G.batch_tex || pipe != G.batch_pipe) {
    flush_batch ();
    G.batch_tex = tex;
    G.batch_pipe = pipe;
  }
  if ((G.inst_used + 1) * sizeof (GfxInst) > GFX_INST_BYTES) {
    static GfxInst scratch;                   /* out of room: draw nothing rather than crash */
    return &scratch;
  }
  GfxInst *q = &G.inst[G.inst_used++];
  G.batch_count++;
  memset (q, 0, sizeof *q);
  memcpy (q->m, G.state.m, sizeof q->m);
  q->tx = G.state.t[0];
  q->ty = G.state.t[1];
  memcpy (q->clip, G.state.clip, sizeof q->clip);
  q->a = G.state.alpha;   /* callers multiply their colour by this */
  return q;
}

void
gfx_effect_params (const float p[8])
{
  if (memcmp (G.push.p, p, sizeof G.push.p)) {
    flush_batch ();
    memcpy (G.push.p, p, sizeof G.push.p);
  }
}

static GfxInst *
solid (float x, float y, float w, float h, float radius, GfxColor c)
{
  if (hidden (x, y, w, h)) {
    static GfxInst scratch;                   /* to be covered: not drawn */
    return &scratch;
  }
  GfxInst *q = gfx_push (NULL, GFX_PIPE_QUAD);
  float a = c.a * q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->r = c.r * a; q->g = c.g * a; q->b = c.b * a; q->a = a;
  q->u1 = q->v1 = 1;
  q->radius = radius;
  q->mode = GFX_MODE_SOLID;
  return q;
}

void
gfx_rect (float x, float y, float w, float h, float radius, GfxColor c)
{
  float parts[4][4];
  int n = radius == 0 ? around_hole (x, y, w, h, parts) : -1;
  if (n < 0) {
    solid (x, y, w, h, radius, c);
    return;
  }
  for (int i = 0; i < n; i++)
    solid (parts[i][0], parts[i][1], parts[i][2], parts[i][3], 0, c);
}

void
gfx_rect_corners (float x, float y, float w, float h, const float r[4], GfxColor c)
{
  GfxInst *q = solid (x, y, w, h, 0, c);
  q->u0 = r[0]; q->v0 = r[1]; q->u1 = r[2]; q->v1 = r[3];
  q->mode = GFX_MODE_CORNERS;
}

void
gfx_ring (float x, float y, float w, float h, float radius, float width, GfxColor c)
{
  GfxInst *q = solid (x, y, w, h, radius, c);
  q->u0 = width; q->v0 = q->u1 = q->v1 = 0;
  q->mode = GFX_MODE_RING;
}

void
gfx_image_uv (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              float radius, float alpha)
{
  if (hidden (x, y, w, h))
    return;
  GfxInst *q = gfx_push (t, GFX_PIPE_QUAD);
  float a = alpha * q->a;
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
  q->r = q->g = q->b = q->a = a;
  q->radius = radius;
  q->mode = GFX_MODE_TEX;
}

void
gfx_image (GfxTex *t, float x, float y, float w, float h, float radius, float alpha)
{
  gfx_image_uv (t, x, y, w, h, 0, 0, 1, 1, radius, alpha);
}

void
gfx_image_piece (GfxTex *t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
{
  if (hidden (x0, y0, x1 - x0, y1 - y0))
    return;
  GfxInst *q = gfx_push (t, GFX_PIPE_QUAD);
  float a = q->a;
  q->x = x0; q->y = y0; q->w = x1; q->h = y1;
  q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
  q->r = q->g = q->b = q->a = a;
  q->mode = GFX_MODE_PIECE;
}

static void
opaque (GfxTex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
  GfxInst *q = gfx_push (t, GFX_PIPE_OPAQUE);
  q->x = x; q->y = y; q->w = w; q->h = h;
  q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
  q->r = q->g = q->b = q->a = 1;
  q->mode = GFX_MODE_TEX;
}

void
gfx_image_opaque (GfxTex *t, float x, float y, float w, float h)
{
  if (G.state.alpha < 1 || G.state.clip[2] > G.state.clip[0]) {
    gfx_image (t, x, y, w, h, 0, 1);
    return;
  }
  float parts[4][4];
  int n = around_hole (x, y, w, h, parts);
  if (n < 0) {
    opaque (t, x, y, w, h, 0, 0, 1, 1);
    return;
  }
  for (int i = 0; i < n; i++) {
    const float *r = parts[i];
    opaque (t, r[0], r[1], r[2], r[3], (r[0] - x) / w, (r[1] - y) / h, (r[0] + r[2] - x) / w, (r[1] + r[3] - y) / h);
  }
}

/* ---- blur ----------------------------------------------------------------------------------------------------- */

GfxTex *
gfx_blur (GfxTex *src, int levels, float offset, int slot)
{
  levels = CLAMP (levels, 1, 5);
  slot = CLAMP (slot, 0, 3);
  if (G.blur_size[slot][0] != src->w || G.blur_size[slot][1] != src->h) {
    for (int i = 0; i < 6; i++) {
      gfx_tex_free (G.blur_targets[slot][i]);
      G.blur_targets[slot][i] = NULL;
    }
    G.blur_size[slot][0] = src->w;
    G.blur_size[slot][1] = src->h;
  }
  GfxTex **tg = G.blur_targets[slot];
  for (int i = 0; i <= levels; i++)
    if (!tg[i] && i > 0)
      tg[i] = gfx_target_new (MAX (1, src->w >> i), MAX (1, src->h >> i));
  GfxTex *from = src;
  /* down: src -> 1/2 -> ... -> 1/2^levels */
  for (int i = 1; i <= levels; i++) {
    gfx_pass_begin (tg[i], (GfxColor) { 0, 0, 0, 0 });
    float p[8] = { 0.5f / from->w * offset, 0.5f / from->h * offset };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (from, GFX_PIPE_BLUR_DOWN);
    q->w = tg[i]->w; q->h = tg[i]->h; q->u1 = q->v1 = 1; q->a = 1;
    gfx_pass_end ();
    from = tg[i];
  }
  /* up: back to 1/2 (the last level up is drawn by the caller, scaled, which is close enough and saves a pass) */
  for (int i = levels - 1; i >= 1; i--) {
    gfx_pass_begin (tg[i], (GfxColor) { 0, 0, 0, 0 });
    float p[8] = { 0.5f / from->w * offset, 0.5f / from->h * offset };
    gfx_effect_params (p);
    GfxInst *q = gfx_push (from, GFX_PIPE_BLUR_UP);
    q->w = tg[i]->w; q->h = tg[i]->h; q->u1 = q->v1 = 1; q->a = 1;
    gfx_pass_end ();
    from = tg[i];
  }
  return from;
}
