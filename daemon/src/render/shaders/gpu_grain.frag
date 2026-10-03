#version 450

// Film grain post-process as a fragment pass over the composited frame.
// Same interleaved gradient noise (Jimenez 2014) as the compute shader and
// the CPU fallback, parameterized identically (power, scale, frame index).

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D frameTex;

layout(push_constant) uniform Grain {
    vec2 viewport;  // framebuffer size in pixels
    float power;    // grain intensity
    float scale;    // noise cell size in pixels
    float frame;    // animation frame index for temporal variation
} grain;

float grainNoise(vec2 p, float frame) {
    p += frame * 137.0;
    float d = dot(p, vec2(0.06711056, 0.00583715));
    return fract(d + 52.9829189 * fract(d));
}

void main() {
    vec4 color = texture(frameTex, fragUv);
    vec2 px = fragUv * grain.viewport;
    vec2 grainCoord = px / max(grain.scale, 0.001);
    float noise = grainNoise(grainCoord, grain.frame);
    float g = (noise - 0.5) * grain.power;
    color.rgb = clamp(color.rgb + g, vec3(0.0), vec3(1.0));
    outColor = color;
}
