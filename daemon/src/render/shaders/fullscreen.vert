#version 450

// Fullscreen triangle (no vertex buffer): covers the viewport with an
// interpolated 0..1 uv for post-process fragment passes.

layout(location = 0) out vec2 fragUv;

void main() {
    // (0,0), (2,0), (0,2) — oversizes the triangle; interpolation keeps uv
    // within 0..1 across the visible viewport.
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    fragUv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
