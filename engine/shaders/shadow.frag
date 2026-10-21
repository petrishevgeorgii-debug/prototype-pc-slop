#version 450
#extension GL_GOOGLE_include_directive : require
// Manhattan Engine: cut-out surfaces (leaves, fences) cast the shadow of what is left of them.

layout(location = 0) in vec2 v_uv;

#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D colour_texture;

void main() {
    if ((pc.material.x & kAlphaTest) != 0u && texture(colour_texture, v_uv).a < 0.5) discard;
}
