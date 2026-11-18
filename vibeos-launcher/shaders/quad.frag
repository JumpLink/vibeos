#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "clip.glsl"

TEX_BINDING uniform sampler2D tex;

LOC(0) in vec2 v_uv;
LOC(1) in vec2 v_local;
LOC(2) flat in vec4 v_color;
LOC(3) flat in vec4 v_shape;
LOC(4) flat in vec4 v_clip;
LOC(5) flat in vec4 v_extra;

layout(location = 0) out vec4 o;

void main()
{
  float cov = clip_coverage(v_clip);
  vec4 c;
  if (v_shape.w == 2.0) {                       // glyph mask
    c = v_color * texture(tex, v_uv).r;
  } else if (v_shape.w == 3.0) {                // a colour, each corner its own radius (tl tr br bl)
    float r = v_local.x < 0.0 ? (v_local.y < 0.0 ? v_extra.x : v_extra.w) : (v_local.y < 0.0 ? v_extra.y : v_extra.z);
    cov *= rrect_coverage(v_local, vec4(v_shape.xy, r, 0.0));
    c = v_color;
  } else if (v_shape.w == 4.0) {                // an outline v_extra.x wide, inside the rect
    float w = v_extra.x;
    cov *= rrect_coverage(v_local, v_shape) -
           rrect_coverage(v_local, vec4(v_shape.xy - w, max(v_shape.z - w, 0.0), 0.0));
    c = v_color;
  } else {
    cov *= rrect_coverage(v_local, v_shape);
    c = v_shape.w == 1.0 ? v_color : v_color * texture(tex, v_uv);
  }
  o = c * cov;
}
