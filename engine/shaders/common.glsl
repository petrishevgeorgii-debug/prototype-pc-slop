// Manhattan Engine: what the shaders share - the frame's uniforms (renderer.cpp, FrameUniforms),
// the sky and the tone mapping.

layout(set = 1, binding = 0) uniform Frame {
    mat4 cascades[4];      // world -> the shadow map's cascades
    vec4 splits;           // where each cascade ends (view depth, metres)
    vec4 cascade_texel;    // a shadow texel's size in metres, per cascade
    vec4 camera_forward;   // xyz; w: exposure
    vec4 camera_right;     // xyz scaled by tan(fov x / 2), negative when mirrored
    vec4 camera_up;        // xyz scaled by tan(fov y / 2)
    vec4 camera_position;  // xyz; w: fog density
    vec4 sun_direction;    // towards the sun; w: shadows on (1)
    vec4 sun_colour;       // rgb, times its intensity
    vec4 ambient;          // the sky's light
    vec4 fill_direction[2]; // towards each fill light
    vec4 fill_colour[2];
    vec4 sky_zenith;
    vec4 sky_horizon;
} frame;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 camera;    // xyz, fog density
    uvec4 material; // x: flags (renderer.h, Material)
} pc;

const uint kTextured = 1u, kNormalMap = 2u, kTint = 4u, kAlphaTest = 8u, kDecal = 16u, kGlass = 32u, kPaint = 64u;

vec3 SkyColour(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    vec3 sky = mix(frame.sky_horizon.rgb, frame.sky_zenith.rgb, pow(max(up, 0.0), 0.45));
    sky = mix(sky, frame.sky_horizon.rgb * 0.55, clamp(-up * 4.0, 0.0, 1.0)); // below the horizon
    float s = max(dot(dir, frame.sun_direction.xyz), 0.0);
    sky += frame.sun_colour.rgb * (pow(s, 8.0) * 0.25 + pow(s, 64.0) * 0.6); // the haze around the sun
    return sky;
}

vec3 SunDisc(vec3 dir) {
    float s = dot(dir, frame.sun_direction.xyz);
    return frame.sun_colour.rgb * 40.0 * smoothstep(0.99985, 0.99995, s);
}

// Krzysztof Narkowicz's fit of the ACES film curve.
vec3 ToneMap(vec3 c) {
    c *= frame.camera_forward.w;
    return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
}

vec3 Fog(vec3 lit, vec3 world) {
    vec3 to = world - frame.camera_position.xyz;
    float d = length(to);
    vec3 dir = to / max(d, 1e-4);
    // Haze that thins with height above the street.
    float h = max(world.y, 0.0);
    float density = frame.camera_position.w * exp(-h * 0.004);
    float fog = 1.0 - exp(-d * density);
    return mix(lit, SkyColour(dir), clamp(fog, 0.0, 1.0));
}
