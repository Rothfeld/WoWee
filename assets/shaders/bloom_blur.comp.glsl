#version 450

// Separable 9-tap Gaussian blur on the quarter-resolution scene copy for TBC
// 2.4.3 ffxGlow (`ffxgauss4.bls`). Executed horizontally then vertically.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uInput;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uOutput;

layout(push_constant) uniform Push {
    vec2 axisStepUV;
    float invSigmaNorm;
    float pad;
} push;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uOutput);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec3 accum = vec3(0.0);
    float weightSum = 0.0;
    for (int k = -4; k <= 4; ++k) {
        float d = float(k) * push.invSigmaNorm;
        float w = exp(-0.5 * d * d);
        accum += textureLod(uInput, uv + float(k) * push.axisStepUV, 0.0).rgb * w;
        weightSum += w;
    }
    imageStore(uOutput, pixel, vec4(accum / max(weightSum, 1e-5), 1.0));
}
