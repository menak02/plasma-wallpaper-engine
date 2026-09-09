#version 450

// One billboard per particle. Same Y-down screen space as the quad path.

layout(location = 0) in vec2 corner;          // (0,0)..(1,1)

layout(location = 1) in vec4 inPosSize;       // xy = center px, z = size px, w = rotation radians
layout(location = 2) in vec4 inColorOpacity;  // rgb = color, a = alpha

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform Frame {
    vec2 viewport;
    vec2 pad;
} frame;

void main() {
    fragUv = corner;
    fragColor = inColorOpacity;

    vec2 local = (corner - 0.5) * vec2(inPosSize.z);
    float c = cos(inPosSize.w);
    float s = sin(inPosSize.w);
    vec2 rotated = vec2(local.x * c - local.y * s,
                        local.x * s + local.y * c);

    vec2 px = inPosSize.xy + rotated;
    vec2 ndc = vec2(px.x / frame.viewport.x * 2.0 - 1.0,
                    1.0 - px.y / frame.viewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
