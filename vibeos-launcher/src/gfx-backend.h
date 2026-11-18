/* What gfx.c asks of a backend (gfx-vk.c: Vulkan, gfx-gl.c: OpenGL ES): the device and the swapchain, textures,
 * uploads, passes and instanced draws. gfx.c keeps the rest, the same whichever backend runs: the drawing state, the
 * instances and their batches, the uploads' queue, when a freed texture goes, and the blur's passes. */
#pragma once

#include "gfx.h"

/* The shaders' push constants (shaders/common.glsl): the same 48 bytes for every pipeline */
typedef struct {
  float vp[2];       /* the pass's size in pixels */
  float pad[2];      /* left to the backend (OpenGL ES: whether the pass draws to the window) */
  float p[8];        /* effect parameters */
} GfxPush;

#define GFX_INST_BYTES (4u << 20)     /* instances per frame: ~43000 quads */

typedef struct {
  const char *name;
  int         frames_in_flight;   /* a texture freed in frame n is destroyed when frame n + this + 1 begins */
  uint32_t    max_upload;         /* bytes: a larger write is uploaded at once (upload_now), not before the pass */

  /* allow_cpu: a software rasterizer will do (Vulkan's lavapipe), else only a GPU */
  bool     (*init)        (struct wl_display *display, struct wl_surface *surface, int w, int h, bool allow_cpu);
  void     (*resize)      (int w, int h);
  void     (*screen_size) (int *w, int *h);
  void     (*finish)      (void);

  GfxTex  *(*tex_new)     (int w, int h, GfxFormat format, bool target);
  void     (*tex_destroy) (GfxTex *t);
  /* tightly packed pixels into t, ahead of the pass about to begin; false: no room left in this frame, again in the
   * next one */
  bool     (*upload)      (GfxTex *t, int x, int y, int w, int h, const void *pixels);
  /* the same, now, outside any frame (and waiting for it) */
  void     (*upload_now)  (GfxTex *t, int x, int y, int w, int h, const void *pixels, int stride);

  /* the frame's instance memory (GFX_INST_BYTES), or NULL when there is nothing to draw into now */
  GfxInst *(*frame_begin) (void);
  void     (*frame_end)   (void);
  /* target NULL: the screen; clear is premultiplied */
  void     (*pass_begin)  (GfxTex *target, int w, int h, const float clear[4]);
  void     (*pass_end)    (void);
  /* this frame's instances first .. first + count - 1, through one pipeline with one texture */
  void     (*draw)        (GfxPipe pipe, GfxTex *tex, const GfxPush *push, uint32_t first, uint32_t count);
} GfxBackend;

extern const GfxBackend gfx_vk_backend;
extern const GfxBackend gfx_gl_backend;
