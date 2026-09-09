#version 450

// Textured quad fragment shader. The three blend modes actually used by the
// wallpaper library (normal/translucent/additive) share this shader; blending
// is applied through fixed-function blend factors keyed off the fragment
// alpha, so per-layer opacity must be folded in here.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragOpacity;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Frame {
    vec2 viewport;
    vec2 parallax;
} frame;

void main() {
    vec4 c = texture(tex, fragUv);
    outColor = vec4(c.rgb, c.a * fragOpacity);
}
