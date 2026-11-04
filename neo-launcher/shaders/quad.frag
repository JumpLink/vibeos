#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "clip.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_local;
layout(location = 2) flat in vec4 v_color;
layout(location = 3) flat in vec4 v_shape;
layout(location = 4) flat in vec4 v_clip;

layout(location = 0) out vec4 o;

void main()
{
  float cov = clip_coverage(v_clip);
  vec4 c;
  if (v_shape.w == 2.0) {                       // glyph mask
    c = v_color * texture(tex, v_uv).r;
  } else {
    cov *= rrect_coverage(v_local, v_shape);
    c = v_shape.w == 1.0 ? v_color : v_color * texture(tex, v_uv);
  }
  o = c * cov;
}
