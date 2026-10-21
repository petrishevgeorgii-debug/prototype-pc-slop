#version 450
// Manhattan Engine: the sky, a triangle over the whole screen behind everything (depth 0 with
// reverse Z).

layout(location = 0) out vec2 v_ndc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
