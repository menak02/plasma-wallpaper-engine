#version 450

// Deformable scene layer (waterwaves / waterripple / wind / foliagesway).
// The layer is rendered as a GRID_SIZE x GRID_SIZE grid of cell quads; the
// cell index comes from gl_InstanceIndex, so one draw call renders the whole
// deformed layer. Cell-corner UVs are continuous across cell boundaries and
// the deformation is evaluated per corner, so neighbouring cells share edge
// positions exactly — a watertight deformed mesh, no cracks.
//
// Deformation math mirrors the CPU path (MeshDeformer::deformVertices):
//   phase      = time * speed * 2.5
//   offset     = sin(phase + u * 2pi) * strength * 12 * sin(v * pi)
//   position  += (cos(direction), sin(direction)) * offset
// With strength == 0 the mesh collapses to the undeformed image.

layout(constant_id = 0) const uint GRID_SIZE = 16;

layout(location = 0) in vec2 corner;         // (0,0)..(1,1) within the cell

layout(location = 1) in vec4 inPosSize;      // xy = layer center px, zw = layer size px
layout(location = 2) in vec4 inRotOpacity;   // x = rotation radians, y = opacity
layout(location = 3) in vec4 inDeform;       // x = speed, y = strength, z = direction

layout(location = 0) out vec2 fragUv;
layout(location = 1) out float fragOpacity;

// Offset-compatible with quad.vert/quad.frag: viewport/parallax stay at
// bytes 0..15, time lands at byte 16 where only the deform pipeline reads.
layout(push_constant) uniform Frame {
    vec2 viewport;   // output size in pixels
    vec2 parallax;   // per-frame mouse parallax offset px (pre-multiplied CPU-side)
    float time;      // seconds since engine start (matches CPU deform clock)
    float pad;
} frame;

void main() {
    uint g = GRID_SIZE;
    vec2 cell = vec2(float(gl_InstanceIndex % g), float(gl_InstanceIndex / g));

    // Continuous UV across the whole deformed layer (shared edges match
    // between neighbouring cells, so the deform offset is seamless).
    vec2 uv = (cell + corner) / float(g);
    fragUv = uv;
    fragOpacity = inRotOpacity.y;

    // Full-layer local corner position for this cell corner
    vec2 local = (uv - 0.5) * inPosSize.zw;

    // Deform in local space, before rotation — same order as the CPU path,
    // where MeshDeformer displaces grid vertices under the painter rotation.
    float phase = frame.time * inDeform.x * 2.5;
    float distFactor = sin(uv.y * 3.14159);
    float offset = sin(phase + uv.x * 6.28318) * inDeform.y * 12.0 * distFactor;
    vec2 displaced = local + vec2(cos(inDeform.z), sin(inDeform.z)) * offset;

    float c = cos(inRotOpacity.x);
    float s = sin(inRotOpacity.x);
    // Vulkan NDC is Y-down (+1 = bottom), scene/screen space is top-left
    // origin Y-down: no flip needed (see quad.vert).
    vec2 rotated = vec2(displaced.x * c - displaced.y * s,
                        displaced.x * s + displaced.y * c);

    vec2 px = inPosSize.xy + frame.parallax + rotated;
    vec2 ndc = vec2(px.x / frame.viewport.x * 2.0 - 1.0,
                    px.y / frame.viewport.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
