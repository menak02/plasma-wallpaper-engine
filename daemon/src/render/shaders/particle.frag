#version 450

// Particle fragment shader. Default particles are a feathered radial glow in
// the QPainter path (alpha 0.9 at center through 0.0 at the edge); the sprite
// texture path uses the same shader with a bound texture instead.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tex;

void main() {
    vec4 texColor = texture(tex, fragUv);
    // Soft feather: 1.0 at center, 0.0 at rim (matches QRadialGradient 0->0.5->1.0
    // alpha ramp used by ParticleEngine's default glow sprite).
    float feather = 1.0 - smoothstep(0.0, 0.5, length(fragUv - 0.5));
    float alpha = texColor.a * fragColor.a * feather;
    outColor = vec4(fragColor.rgb * texColor.rgb, alpha);
}
