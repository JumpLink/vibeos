// Shared by every stage: the push constants (gfx.c's Push)
layout(push_constant) uniform Push {
  vec2 vp;      // the pass's size in pixels
  vec2 pad;
  vec4 p0;      // effect parameters
  vec4 p1;
} pc;
