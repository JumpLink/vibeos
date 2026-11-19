/* The OpenGL ES 3.0 backend (see gfx-backend.h), on EGL and a wl_egl_window: for a GPU that has OpenGL ES but no
 * Vulkan driver. It draws what the Vulkan backend draws: the same instances through the same shaders (shaders/,
 * compiled as GLSL ES 3.00: *_gles.h), premultiplied blending, linear clamped sampling without mipmaps, 8-bit UNORM
 * textures and targets, no dithering.
 *
 * What differs is kept out of the pictures:
 *  - OpenGL ES counts the window's rows from the bottom. A pass to the window is drawn upside down (the vertex shader
 *    flips y when the push constants' pad.x is 1) and the clip reads gl_FragCoord the same way, so the window shows
 *    the picture upright. Offscreen targets are not flipped: their first row is texture row 0, as in Vulkan.
 *  - A BGRA texture is stored as RGBA bytes as they come (no conversion) and sampled through a swizzle that swaps red
 *    and blue back.
 *  - Instances are written to memory of our own and copied into the frame's vertex buffer batch by batch, each batch
 *    into its own range, mapped unsynchronized: no draw of the frame reads that range yet, and the buffer's previous
 *    frame is done with it (a ring of RING buffers, each reused once the fence after its frame has signalled, as the
 *    Vulkan backend waits for its frame's fence). OpenGL ES 3.0 has no base instance, so each draw points the
 *    attributes at its batch.
 *  - Frames: eglSwapBuffers presents with swap interval 0, as the Vulkan swapchain's mailbox does: the compositor's
 *    frame callbacks (wl.c) pace them, and the swap never waits for one of its own. VIBEOS_FIFO asks for interval 1. */
#include "gfx-backend.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <glib.h>
#include <string.h>
#include <wayland-egl.h>

#include "quad_vert_gles.h"
#include "quad_frag_gles.h"
#include "blur_down_frag_gles.h"
#include "blur_up_frag_gles.h"
#include "stretch_frag_gles.h"

#define MAX_UPLOAD (4u << 20)        /* as the Vulkan backend: a larger write goes at once */
#define RING       3                 /* instance buffers, a frame each */

typedef struct {
  GfxTex base;
  GLuint tex;
  GLuint fbo;                /* render targets only */
} GlTex;

typedef struct {
  GLuint  prog;
  bool    blend;
  GLint   vp, pad, p0, p1;   /* the uniform struct pc's members */
  GfxPush set;               /* what they hold */
  bool    set_valid;
} Pipe;

static struct {
  EGLDisplay            dpy;
  EGLContext            ctx;
  EGLSurface            surf;
  struct wl_egl_window *win;
  int                   w, h;
  GLint                 max_size;

  Pipe                  pipes[GFX_N_PIPES];
  GLuint                vao, sampler;
  GLuint                vbo[RING];
  GLsync                fence[RING];  /* after the last frame that used vbo[i] */
  int                   ring;         /* this frame's vbo */
  GfxInst              *inst;       /* the frame's instances, copied into vbo[ring] batch by batch */

  /* the frame being drawn */
  bool                  screen;     /* the pass draws to the window */
  GLuint                fbo;        /* the pass's framebuffer */
  GfxPipe               bound_pipe;
  GLuint                bound_tex;
} L;

static bool
has_ext (const char *list, const char *ext)
{
  if (!list)
    return false;
  size_t n = strlen (ext);
  for (const char *p = list; (p = strstr (p, ext)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
      return true;
  return false;
}

static GLuint
compile (GLenum type, const char *src, const char *name)
{
  GLuint s = glCreateShader (type);
  glShaderSource (s, 1, &src, NULL);
  glCompileShader (s);
  GLint ok = 0;
  glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096] = "";
    glGetShaderInfoLog (s, sizeof log, NULL, log);
    g_warning ("gl: %s: %s", name, log);
    glDeleteShader (s);
    return 0;
  }
  return s;
}

static bool
make_pipe (Pipe *p, GLuint vert, const char *frag_src, const char *name, bool blend)
{
  GLuint frag = compile (GL_FRAGMENT_SHADER, frag_src, name);
  if (!frag)
    return false;
  p->prog = glCreateProgram ();
  glAttachShader (p->prog, vert);
  glAttachShader (p->prog, frag);
  glLinkProgram (p->prog);
  glDeleteShader (frag);
  GLint ok = 0;
  glGetProgramiv (p->prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096] = "";
    glGetProgramInfoLog (p->prog, sizeof log, NULL, log);
    g_warning ("gl: %s: %s", name, log);
    return false;
  }
  p->blend = blend;
  p->vp = glGetUniformLocation (p->prog, "pc.vp");
  p->pad = glGetUniformLocation (p->prog, "pc.pad");
  p->p0 = glGetUniformLocation (p->prog, "pc.p0");
  p->p1 = glGetUniformLocation (p->prog, "pc.p1");
  glUseProgram (p->prog);
  glUniform1i (glGetUniformLocation (p->prog, "tex"), 0);
  return true;
}

/* everything init made, so that another backend can have the surface */
static void
teardown (void)
{
  if (L.ctx && eglGetCurrentContext () == L.ctx) {
    for (int i = 0; i < GFX_N_PIPES; i++)
      if (L.pipes[i].prog)
        glDeleteProgram (L.pipes[i].prog);
    for (int i = 0; i < RING; i++) {
      if (L.fence[i]) glDeleteSync (L.fence[i]);
      if (L.vbo[i]) glDeleteBuffers (1, &L.vbo[i]);
    }
    if (L.vao) glDeleteVertexArrays (1, &L.vao);
    if (L.sampler) glDeleteSamplers (1, &L.sampler);
  }
  if (L.dpy != EGL_NO_DISPLAY) {
    eglMakeCurrent (L.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (L.surf != EGL_NO_SURFACE)
      eglDestroySurface (L.dpy, L.surf);
    if (L.ctx != EGL_NO_CONTEXT)
      eglDestroyContext (L.dpy, L.ctx);
    eglTerminate (L.dpy);
  }
  if (L.win)
    wl_egl_window_destroy (L.win);
  g_free (L.inst);
  memset (&L, 0, sizeof L);
}

/* 8-bit red, green and blue, no alpha (an opaque window, as the Vulkan swapchain's), else with alpha */
static bool
choose_config (EGLConfig *out)
{
  static const EGLint attrs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
    EGL_NONE,
  };
  EGLConfig configs[64];
  EGLint n = 0;
  if (!eglChooseConfig (L.dpy, attrs, configs, G_N_ELEMENTS (configs), &n) || n < 1)
    return false;
  int best = -1;
  for (int i = 0; i < n; i++) {
    EGLint r, g, b, a;
    eglGetConfigAttrib (L.dpy, configs[i], EGL_RED_SIZE, &r);
    eglGetConfigAttrib (L.dpy, configs[i], EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib (L.dpy, configs[i], EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib (L.dpy, configs[i], EGL_ALPHA_SIZE, &a);
    if (r != 8 || g != 8 || b != 8)
      continue;
    if (a == 0) {
      best = i;
      break;
    }
    if (best < 0)
      best = i;
  }
  if (best < 0)
    return false;
  *out = configs[best];
  return true;
}

static bool
setup (struct wl_display *display, struct wl_surface *surface, int w, int h)
{
  const char *client = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
  PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window = NULL;
  if (has_ext (client, "EGL_EXT_platform_base") &&
      (has_ext (client, "EGL_EXT_platform_wayland") || has_ext (client, "EGL_KHR_platform_wayland"))) {
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    create_platform_window =
      (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC) eglGetProcAddress ("eglCreatePlatformWindowSurfaceEXT");
  }
  L.dpy = get_platform_display ? get_platform_display (EGL_PLATFORM_WAYLAND_EXT, display, NULL)
                               : eglGetDisplay ((EGLNativeDisplayType) display);
  EGLint major, minor;
  if (L.dpy == EGL_NO_DISPLAY || !eglInitialize (L.dpy, &major, &minor)) {
    g_message ("gl: no EGL display");
    L.dpy = EGL_NO_DISPLAY;
    return false;
  }
  EGLConfig config;
  if (!eglBindAPI (EGL_OPENGL_ES_API) || !choose_config (&config)) {
    g_message ("gl: no OpenGL ES 3 window config");
    return false;
  }
  static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  L.ctx = eglCreateContext (L.dpy, config, EGL_NO_CONTEXT, ctx_attrs);
  if (L.ctx == EGL_NO_CONTEXT) {
    g_message ("gl: no OpenGL ES 3 context");
    return false;
  }
  L.win = wl_egl_window_create (surface, w, h);
  L.w = w;
  L.h = h;
  L.surf = create_platform_window ? create_platform_window (L.dpy, config, L.win, NULL)
                                  : eglCreateWindowSurface (L.dpy, config, (EGLNativeWindowType) L.win, NULL);
  if (L.surf == EGL_NO_SURFACE || !eglMakeCurrent (L.dpy, L.surf, L.surf, L.ctx)) {
    g_message ("gl: cannot draw to the window");
    return false;
  }
  eglSwapInterval (L.dpy, g_getenv ("VIBEOS_FIFO") ? 1 : 0);

  GLuint vert = compile (GL_VERTEX_SHADER, quad_vert_gles, "quad.vert");
  if (!vert)
    return false;
  bool ok = make_pipe (&L.pipes[GFX_PIPE_QUAD], vert, quad_frag_gles, "quad.frag", true) &&
            make_pipe (&L.pipes[GFX_PIPE_BLUR_DOWN], vert, blur_down_frag_gles, "blur_down.frag", false) &&
            make_pipe (&L.pipes[GFX_PIPE_BLUR_UP], vert, blur_up_frag_gles, "blur_up.frag", false) &&
            make_pipe (&L.pipes[GFX_PIPE_STRETCH], vert, stretch_frag_gles, "stretch.frag", true);
  glDeleteShader (vert);
  if (!ok)
    return false;

  glGenVertexArrays (1, &L.vao);
  glBindVertexArray (L.vao);
  glGenBuffers (RING, L.vbo);
  for (int i = 0; i < RING; i++) {
    glBindBuffer (GL_ARRAY_BUFFER, L.vbo[i]);
    glBufferData (GL_ARRAY_BUFFER, GFX_INST_BYTES, NULL, GL_STREAM_DRAW);
  }
  for (GLuint i = 0; i < 6; i++) {
    glEnableVertexAttribArray (i);
    glVertexAttribDivisor (i, 1);
  }
  glGenSamplers (1, &L.sampler);
  glSamplerParameteri (L.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri (L.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri (L.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri (L.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glActiveTexture (GL_TEXTURE0);
  glBindSampler (0, L.sampler);
  glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation (GL_FUNC_ADD);
  glDisable (GL_DITHER);
  glDisable (GL_DEPTH_TEST);
  glDisable (GL_STENCIL_TEST);
  glDisable (GL_SCISSOR_TEST);
  glDisable (GL_CULL_FACE);
  glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
  glGetIntegerv (GL_MAX_TEXTURE_SIZE, &L.max_size);
  L.inst = g_malloc (GFX_INST_BYTES);
  L.bound_pipe = GFX_N_PIPES;

  g_message ("gl: %s, %s (EGL %d.%d)", glGetString (GL_RENDERER), glGetString (GL_VERSION), major, minor);
  return glGetError () == GL_NO_ERROR;
}

static bool
glb_init (struct wl_display *display, struct wl_surface *surface, int w, int h, bool allow_cpu)
{
  L.dpy = EGL_NO_DISPLAY;
  L.ctx = EGL_NO_CONTEXT;
  L.surf = EGL_NO_SURFACE;
  if (setup (display, surface, w, h))
    return true;
  teardown ();
  return false;
}

static void
glb_resize (int w, int h)
{
  if (w == L.w && h == L.h)
    return;
  wl_egl_window_resize (L.win, w, h, 0, 0);
  L.w = w;
  L.h = h;
}

static void
glb_screen_size (int *w, int *h)
{
  *w = L.w;
  *h = L.h;
}

static void
glb_finish (void)
{
  glFinish ();
}

/* ---- textures ------------------------------------------------------------------------------------------------- */

static void
bind_tex (GLuint tex)
{
  if (L.bound_tex != tex) {
    glBindTexture (GL_TEXTURE_2D, tex);
    L.bound_tex = tex;
  }
}

static void glb_tex_destroy (GfxTex *gt);

static GfxTex *
glb_tex_new (int w, int h, GfxFormat format, bool target)
{
  if (w < 1 || h < 1 || w > L.max_size || h > L.max_size)
    return NULL;
  GlTex *t = g_new0 (GlTex, 1);
  t->base = (GfxTex) { w, h, format, target };
  for (int i = 0; i < 16 && glGetError () != GL_NO_ERROR; i++)
    ;
  glGenTextures (1, &t->tex);
  bind_tex (t->tex);
  glTexStorage2D (GL_TEXTURE_2D, 1, format == GFX_FORMAT_R8 ? GL_R8 : GL_RGBA8, w, h);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  if (format == GFX_FORMAT_BGRA8 && !target) {
    /* the bytes are B G R A: stored as they come, read back with red and blue swapped */
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
  }
  if (glGetError () != GL_NO_ERROR)
    goto fail;
  if (target) {
    glGenFramebuffers (1, &t->fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    glBindFramebuffer (GL_FRAMEBUFFER, L.fbo);      /* the pass's again, if one is on */
    if (status != GL_FRAMEBUFFER_COMPLETE)
      goto fail;
  }
  return &t->base;
fail:
  glb_tex_destroy (&t->base);
  return NULL;
}

static void
glb_tex_destroy (GfxTex *gt)
{
  GlTex *t = (GlTex *) gt;
  if (t->fbo)
    glDeleteFramebuffers (1, &t->fbo);
  if (t->tex) {
    if (L.bound_tex == t->tex)
      L.bound_tex = 0;
    glDeleteTextures (1, &t->tex);
  }
  g_free (t);
}

static void
write_tex (GlTex *t, int x, int y, int w, int h, const void *pixels, int row_length)
{
  bind_tex (t->tex);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, row_length);
  glTexSubImage2D (GL_TEXTURE_2D, 0, x, y, w, h, t->base.format == GFX_FORMAT_R8 ? GL_RED : GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
}

static bool
glb_upload (GfxTex *gt, int x, int y, int w, int h, const void *pixels)
{
  write_tex ((GlTex *) gt, x, y, w, h, pixels, 0);
  return true;
}

static void
glb_upload_now (GfxTex *gt, int x, int y, int w, int h, const void *pixels, int stride)
{
  int bpp = gt->format == GFX_FORMAT_R8 ? 1 : 4;
  if (stride % bpp == 0) {
    write_tex ((GlTex *) gt, x, y, w, h, pixels, stride / bpp);
    return;
  }
  guint8 *packed = g_malloc ((gsize) w * h * bpp);
  for (int r = 0; r < h; r++)
    memcpy (packed + (gsize) r * w * bpp, (const guint8 *) pixels + (gsize) r * stride, (gsize) w * bpp);
  write_tex ((GlTex *) gt, x, y, w, h, packed, 0);
  g_free (packed);
}

/* ---- frames and passes ---------------------------------------------------------------------------------------- */

static GfxInst *
glb_frame_begin (void)
{
  /* the next instance buffer, once the GPU is done with the frame that used it last */
  L.ring = (L.ring + 1) % RING;
  GLsync f = L.fence[L.ring];
  if (f) {
    GLenum r;
    do
      r = glClientWaitSync (f, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
    while (r == GL_TIMEOUT_EXPIRED);
    glDeleteSync (f);
    L.fence[L.ring] = NULL;
  }
  glBindBuffer (GL_ARRAY_BUFFER, L.vbo[L.ring]);
  return L.inst;
}

static void
glb_frame_end (void)
{
  L.fence[L.ring] = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!eglSwapBuffers (L.dpy, L.surf))
    g_warning ("gl: eglSwapBuffers failed (0x%x)", eglGetError ());
}

static void
glb_pass_begin (GfxTex *gt, int w, int h, const float clear[4])
{
  GlTex *target = (GlTex *) gt;
  L.fbo = target ? target->fbo : 0;
  glBindFramebuffer (GL_FRAMEBUFFER, L.fbo);
  glViewport (0, 0, w, h);
  glClearColor (clear[0], clear[1], clear[2], clear[3]);
  glClear (GL_COLOR_BUFFER_BIT);
  L.screen = !target;
}

static void
glb_pass_end (void)
{
}

static void
glb_draw (GfxPipe pipe, GfxTex *gt, const GfxPush *push, uint32_t first, uint32_t count)
{
  Pipe *p = &L.pipes[pipe];
  if (L.bound_pipe != pipe) {
    glUseProgram (p->prog);
    if (p->blend)
      glEnable (GL_BLEND);
    else
      glDisable (GL_BLEND);
    L.bound_pipe = pipe;
  }
  GfxPush u = *push;
  u.pad[0] = L.screen ? 1 : 0;
  if (!p->set_valid || memcmp (&p->set, &u, sizeof u)) {
    glUniform2fv (p->vp, 1, u.vp);
    glUniform2fv (p->pad, 1, u.pad);
    glUniform4fv (p->p0, 1, u.p);
    glUniform4fv (p->p1, 1, u.p + 4);
    p->set = u;
    p->set_valid = true;
  }
  bind_tex (((GlTex *) gt)->tex);
  GLintptr off = (GLintptr) first * sizeof (GfxInst);
  GLsizeiptr size = (GLsizeiptr) count * sizeof (GfxInst);
  void *dst = glMapBufferRange (GL_ARRAY_BUFFER, off, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  if (!dst)
    return;
  memcpy (dst, L.inst + first, size);
  glUnmapBuffer (GL_ARRAY_BUFFER);
  for (GLuint i = 0; i < 6; i++)
    glVertexAttribPointer (i, 4, GL_FLOAT, GL_FALSE, sizeof (GfxInst), (const void *) (off + i * 16));
  glDrawArraysInstanced (GL_TRIANGLES, 0, 6, count);
}

const GfxBackend gfx_gl_backend = {
  .name = "gl",
  .frames_in_flight = 0,       /* OpenGL keeps a deleted texture until the draws that use it are done */
  .max_upload = MAX_UPLOAD,
  .init = glb_init,
  .resize = glb_resize,
  .screen_size = glb_screen_size,
  .finish = glb_finish,
  .tex_new = glb_tex_new,
  .tex_destroy = glb_tex_destroy,
  .upload = glb_upload,
  .upload_now = glb_upload_now,
  .frame_begin = glb_frame_begin,
  .frame_end = glb_frame_end,
  .pass_begin = glb_pass_begin,
  .pass_end = glb_pass_end,
  .draw = glb_draw,
};
