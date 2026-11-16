#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// One GfxInst per instance
layout(location = 0) in vec4 i_rect;    // x y w h, the rect in its own space
layout(location = 1) in vec4 i_uv;      // u0 v0 u1 v1
layout(location = 2) in vec4 i_color;   // premultiplied
layout(location = 3) in vec4 i_m;       // screen = [m0 m1; m2 m3] p + t
layout(location = 4) in vec4 i_misc;    // tx ty radius mode
layout(location = 5) in vec4 i_clip;    // screen x0 y0 x1 y1

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_local;          // from the rect's centre, in its own units
layout(location = 2) flat out vec4 v_color;
layout(location = 3) flat out vec4 v_shape;     // half w, half h, radius, mode
layout(location = 4) flat out vec4 v_clip;
layout(location = 5) flat out vec4 v_extra;     // the corners' radii (mode 3), the ring's width (mode 4)

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));

void main()
{
  vec2 c = corners[gl_VertexIndex];
  float mode = i_misc.w;

  // Grow the quad by a pixel on every side so the edge can be antialiased; glyph masks are pixel-aligned and come
  // from an atlas whose neighbours must not bleed in, so they keep their exact size
  vec2 grow = vec2(0.0);
  if (mode != 2.0) {
    float sx = length(vec2(i_m.x, i_m.z));
    float sy = length(vec2(i_m.y, i_m.w));
    grow = vec2(1.0 / max(sx, 1e-4), 1.0 / max(sy, 1e-4));
  }
  vec2 f = mix(-grow / i_rect.zw, 1.0 + grow / i_rect.zw, c);   // rect-relative, may run past 0..1
  vec2 p = i_rect.xy + f * i_rect.zw;

  vec2 s = vec2(i_m.x * p.x + i_m.y * p.y, i_m.z * p.x + i_m.w * p.y) + i_misc.xy;
  gl_Position = vec4(s / pc.vp * 2.0 - 1.0, 0.0, 1.0);

  v_uv = mix(i_uv.xy, i_uv.zw, f);
  v_local = (f - 0.5) * i_rect.zw;
  v_color = i_color;
  v_shape = vec4(i_rect.zw * 0.5, i_misc.z, mode);
  v_clip = i_clip;
  v_extra = i_uv;
}
