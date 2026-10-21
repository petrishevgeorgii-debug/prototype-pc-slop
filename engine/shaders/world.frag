#version 450
#extension GL_GOOGLE_include_directive : require
// Manhattan Engine: the city's surfaces - the game's colour and normal maps and vertex tint, lit by
// its light rig (manhattan_tod: the sun, the sky's ambient light, two fill lights) with the sun's
// shadows, then haze and tone mapping.

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec4 v_tangent;
layout(location = 4) in vec4 v_colour;

#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2D colour_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_texture;
layout(set = 1, binding = 1) uniform sampler2DShadow shadow_map;

layout(location = 0) out vec4 out_colour;

float Shadow(vec3 world, vec3 n) {
    if (frame.sun_direction.w < 0.5) return 1.0;
    float depth = dot(world - frame.camera_position.xyz, frame.camera_forward.xyz);
    int i = depth < frame.splits.x ? 0 : depth < frame.splits.y ? 1 : depth < frame.splits.z ? 2 : 3;
    if (depth > frame.splits.w) return 1.0;
    // Pushed off the surface by a texel and a half, so it does not shadow itself.
    float texel = frame.cascade_texel[i];
    vec3 p = world + n * texel * 1.5 + frame.sun_direction.xyz * texel;
    vec3 c = (frame.cascades[i] * vec4(p, 1.0)).xyz;
    vec2 uv = c.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || c.z > 1.0) return 1.0;
    uv = (uv + vec2(i & 1, i >> 1)) * 0.5;
    const float t = 1.0 / 4096.0;
    float s = 0.0;
    s += texture(shadow_map, vec3(uv + vec2(-0.75, -0.25) * t, c.z));
    s += texture(shadow_map, vec3(uv + vec2(0.25, -0.75) * t, c.z));
    s += texture(shadow_map, vec3(uv + vec2(0.75, 0.25) * t, c.z));
    s += texture(shadow_map, vec3(uv + vec2(-0.25, 0.75) * t, c.z));
    s *= 0.25;
    // The last cascade fades out at its far end.
    if (i == 3) s = mix(s, 1.0, smoothstep(frame.splits.w * 0.8, frame.splits.w, depth));
    return s;
}

void main() {
    uint flags = pc.material.x;
    vec4 albedo = (flags & kTextured) != 0u ? texture(colour_texture, v_uv) : vec4(0.35, 0.34, 0.33, 1.0);
    if ((flags & kAlphaTest) != 0u && albedo.a < 0.5) discard;
    // The city's "CBV" effects darken and colour each building by its vertices (black: none).
    if ((flags & kTint) != 0u && max(v_colour.r, max(v_colour.g, v_colour.b)) > 0.01) albedo.rgb *= v_colour.rgb;

    // The surface's normal: the vertex's (else the face's), turned to the camera, then the map's.
    vec3 to_camera = frame.camera_position.xyz - v_world;
    vec3 face = normalize(cross(dFdx(v_world), dFdy(v_world)));
    vec3 n = dot(v_normal, v_normal) > 0.01 ? normalize(v_normal) : face;
    if (dot(n, to_camera) < 0.0) n = -n; // two-sided until the game's winding is known
    vec3 geometric = n;
    if ((flags & kNormalMap) != 0u && dot(v_tangent.xyz, v_tangent.xyz) > 0.01) {
        vec2 xy = texture(normal_texture, v_uv).rg * 2.0 - 1.0;
        vec3 m = vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
        vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
        vec3 b = cross(n, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        n = normalize(t * m.x + b * m.y + n * m.z);
    }

    vec3 l = frame.sun_direction.xyz;
    float sun = max(dot(n, l), 0.0) * (dot(geometric, l) > 0.0 ? Shadow(v_world, geometric) : 0.0);
    // The game's rig is balanced for its own (gamma space) shading: here the sun is made stronger
    // and the sky and fill lights weaker, for the contrast of linear lighting with shadows.
    vec3 light = frame.sun_colour.rgb * sun * 1.6;
    // The sky's light from above, less from the street.
    light += frame.ambient.rgb * mix(0.25, 0.6, n.y * 0.5 + 0.5);
    for (int k = 0; k < 2; ++k) light += frame.fill_colour[k].rgb * max(dot(n, frame.fill_direction[k].xyz), 0.0) * 0.3;

    vec3 lit = albedo.rgb * light;
    lit = Fog(lit, v_world);
    float alpha = (flags & kDecal) != 0u ? albedo.a : 1.0;
    out_colour = vec4(ToneMap(lit), alpha);
}
