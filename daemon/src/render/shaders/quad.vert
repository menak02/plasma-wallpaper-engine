#version 450

// One billboard quad per scene layer. Vertex data is a 4-corner strip;
// everything else (screen position/size in pixels, rotation, opacity) comes
// per-instance so a whole scene is one draw per layer texture.

layout(location = 0) in vec2 corner;       // (0,0)..(1,1)

layout(location = 1) in vec4 inPosSize;    // xy = center px, zw = size px
layout(location = 2) in vec4 inRotOpacity; // x = rotation radians, y = opacity

layout(location = 0) out vec2 fragUv;
layout(location = 1) out float fragOpacity;

layout(push_constant) uniform Frame {
    vec2 viewport;   // output size in pixels
    vec2 parallax;   // per-frame mouse parallax offset px (pre-multiplied CPU-side)
} frame;

void main() {
    fragUv = corner;
    fragOpacity = inRotOpacity.y;

    // Corner offsets centered on origin
    vec2 local = (corner - 0.5) * inPosSize.zw;

    float c = cos(inRotOpacity.x);
    float s = sin(inRotOpacity.x);
    // Y flipped: scene/screen space is top-left origin, Y down
    vec2 rotated = vec2(local.x * c - local.y * s,
                        local.x * s + local.y * c);

    vec2 px = inPosSize.xy + frame.parallax + rotated;
    vec2 ndc = vec2(px.x / frame.viewport.x * 2.0 - 1.0,
                    1.0 - px.y / frame.viewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
