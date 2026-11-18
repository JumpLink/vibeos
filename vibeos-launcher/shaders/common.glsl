// Shared by every stage: the push constants (gfx-backend.h's GfxPush). OpenGL ES has no push constants: a uniform
// struct of the same members stands in for them there, and the macros below cover what else the two dialects spell
// differently (glslang defines VULKAN when it compiles for Vulkan; the OpenGL ES driver does not).
#ifdef VULKAN
layout(push_constant) uniform Push {
  vec2 vp;      // the pass's size in pixels
  vec2 pad;
  vec4 p0;      // effect parameters
  vec4 p1;
} pc;
#define LOC(n) layout(location = n)
#define TEX_BINDING layout(set = 0, binding = 0)
#define VERTEX_INDEX gl_VertexIndex
#else
struct Push {
  vec2 vp;      // the pass's size in pixels
  vec2 pad;     // x: 1 when the pass draws to the window, whose rows OpenGL ES counts from the bottom
  vec4 p0;      // effect parameters
  vec4 p1;
};
uniform Push pc;
#define LOC(n)
#define TEX_BINDING
#define VERTEX_INDEX gl_VertexID
#endif
