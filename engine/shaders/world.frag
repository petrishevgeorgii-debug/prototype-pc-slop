#version 450
// Manhattan Engine: a first lighting model for the city - the colour texture lit by a sun and
// the sky, then fog. To be replaced by physically based materials (docs/ENGINE.md).

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec4 v_colour;

layout(set = 0, binding = 0) uniform sampler2D colour_texture;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 camera;
    vec4 sun;
} pc;

layout(location = 0) out vec4 out_colour;

void main() {
    vec3 albedo = pc.sun.w > 0.5 ? texture(colour_texture, v_uv).rgb : vec3(0.42, 0.40, 0.37);
    // Normals: the vertex's, else the face's (some of the city's geometry has none).
    vec3 n = dot(v_normal, v_normal) > 0.01 ? normalize(v_normal) : normalize(cross(dFdx(v_world), dFdy(v_world)));
    vec3 to_camera = pc.camera.xyz - v_world;
    if (dot(n, to_camera) < 0.0) n = -n; // two-sided until the game's winding is known
    vec3 sun = normalize(pc.sun.xyz);
    float direct = max(dot(n, sun), 0.0);
    vec3 sky = vec3(0.30, 0.38, 0.48);
    vec3 ground = vec3(0.10, 0.09, 0.08);
    vec3 ambient = mix(ground, sky, n.y * 0.5 + 0.5) * 1.1;
    vec3 lit = albedo * (ambient + vec3(1.0, 0.92, 0.80) * direct * 1.6);
    float fog = 1.0 - exp(-length(to_camera) * pc.camera.w);
    out_colour = vec4(mix(lit, sky, clamp(fog, 0.0, 1.0)), 1.0);
}
