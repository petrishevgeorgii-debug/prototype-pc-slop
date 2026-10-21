#version 450
#extension GL_GOOGLE_include_directive : require
// Manhattan Engine: the sun's shadow map; pc.view_projection is a cascade's matrix.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

#include "common.glsl"

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = in_uv;
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
}
