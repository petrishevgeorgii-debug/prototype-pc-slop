#version 450
// Manhattan Engine: the city's geometry (renderer.cpp). Positions are world space, metres, y up.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec4 in_colour;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 camera; // xyz, fog density
    vec4 sun;    // xyz, flags (1: textured)
} pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec4 v_colour;

void main() {
    v_world = in_position;
    v_uv = in_uv;
    v_normal = in_normal;
    v_colour = in_colour;
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
}
