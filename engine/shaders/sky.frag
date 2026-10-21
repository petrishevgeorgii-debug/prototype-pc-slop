#version 450
#extension GL_GOOGLE_include_directive : require
// Manhattan Engine: the sky - a gradient from the haze at the horizon to the zenith, the sun.

layout(location = 0) in vec2 v_ndc;

#include "common.glsl"

layout(location = 0) out vec4 out_colour;

void main() {
    // Screen y grows downwards (the projection flips it).
    vec3 dir = normalize(frame.camera_forward.xyz + v_ndc.x * frame.camera_right.xyz - v_ndc.y * frame.camera_up.xyz);
    out_colour = vec4(ToneMap(SkyColour(dir) + SunDisc(dir)), 1.0);
}
