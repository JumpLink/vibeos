#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Dual Kawase, down: the centre and four diagonal taps; pc.p0.xy = half a source texel × offset
TEX_BINDING uniform sampler2D tex;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main()
{
  vec2 h = pc.p0.xy;
  vec4 s = texture(tex, v_uv) * 4.0;
  s += texture(tex, v_uv - h);
  s += texture(tex, v_uv + h);
  s += texture(tex, v_uv + vec2(h.x, -h.y));
  s += texture(tex, v_uv - vec2(h.x, -h.y));
  o = s / 8.0;
}
