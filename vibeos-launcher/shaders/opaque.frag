#version 450

// An opaque picture written over what is there (gfx_image_opaque): no corners, no clip and no blending, so a
// full-screen one costs a texture read per pixel and nothing else
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 o;

void main()
{
  o = vec4(texture(tex, v_uv).rgb, 1.0);
}
