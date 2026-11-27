#version 450

// TBC 2.4.3 `ffxglow.bls` additive bloom composite:
//   result.rgb = clamp(scene.rgb + glow * (blur.rgb * blur.rgb), 0.0, 1.0)
// Drawn in the 1x overlay pass with ONE + ONE additive color blending ahead of
// SunShafts and all 2D UI.

layout(set = 0, binding = 0) uniform sampler2D uBloom;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    float glow;
} push;

void main() {
    vec3 b = texture(uBloom, TexCoord).rgb;
    outColor = vec4(push.glow * (b * b), 0.0);
}
