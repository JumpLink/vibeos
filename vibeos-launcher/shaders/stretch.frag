#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "clip.glsl"

// Android's overscroll stretch (frameworks/base libs/hwui/effects/StretchEffect.cpp), unchanged but for the GLSL:
// pc.p0 = overscroll x, overscroll y (normalised, -1..1), the content's width and height in pixels
TEX_BINDING uniform sampler2D tex;

LOC(0) in vec2 v_uv;
LOC(1) in vec2 v_local;
LOC(2) flat in vec4 v_color;
LOC(3) flat in vec4 v_shape;
LOC(4) flat in vec4 v_clip;

layout(location = 0) out vec4 o;

const float INTERPOLATION_STRENGTH = 0.7;

float overscroll_start(float pos, float overscroll, float affected, float inv_affected, float stretched)
{
  float offset = affected - pos;
  float variation = mix(1.0, offset * inv_affected, INTERPOLATION_STRENGTH);
  return stretched - offset / (1.0 + overscroll * variation);
}

float overscroll_end(float pos, float overscroll, float reverse, float inv_affected, float stretched, float size)
{
  float offset = pos - reverse;
  float variation = mix(1.0, offset * inv_affected, INTERPOLATION_STRENGTH);
  return size - (stretched - offset / (1.0 + -overscroll * variation));
}

float overscroll(float pos, float overscroll, float size)
{
  float stretched = size / (1.0 + abs(overscroll));
  float diff = stretched - size;
  float inv = 1.0 / size;
  if (overscroll > 0.0)
    return pos <= size ? overscroll_start(pos, overscroll, size, inv, stretched) : diff + pos;
  if (overscroll < 0.0) {
    float reverse = 0.0;   // size - the affected distance, which hwui sets to the whole size
    return pos >= reverse ? overscroll_end(pos, overscroll, reverse, inv, stretched, size) : -diff + pos;
  }
  return pos;
}

void main()
{
  vec2 size = pc.p0.zw;
  vec2 coord = v_uv * size;
  coord = vec2(overscroll(coord.x, pc.p0.x, size.x), overscroll(coord.y, pc.p0.y, size.y));
  float cov = clip_coverage(v_clip) * rrect_coverage(v_local, v_shape);
  o = v_color * texture(tex, coord / size) * cov;
}
