#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Dual Kawase, up: eight taps on a diamond; pc.p0.xy = half a source texel × offset
TEX_BINDING uniform sampler2D tex;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main()
{
  vec2 h = pc.p0.xy;
  vec4 s = texture(tex, v_uv + vec2(-h.x * 2.0, 0.0));
  s += texture(tex, v_uv + vec2(-h.x, h.y)) * 2.0;
  s += texture(tex, v_uv + vec2(0.0, h.y * 2.0));
  s += texture(tex, v_uv + vec2(h.x, h.y)) * 2.0;
  s += texture(tex, v_uv + vec2(h.x * 2.0, 0.0));
  s += texture(tex, v_uv + vec2(h.x, -h.y)) * 2.0;
  s += texture(tex, v_uv + vec2(0.0, -h.y * 2.0));
  s += texture(tex, v_uv + vec2(-h.x, -h.y)) * 2.0;
  o = s / 12.0;
}
