#pragma once

#include <algorithm>
#include <cassert>

namespace wowee::rendering {

/// Fraction of a mist puff's quad size over which it fades to zero as it
/// approaches opaque world geometry (sloped terrain, rocks, buildings).
/// A 36-yard puff represents a ~18-yard-radius cloud; fading over 40% of its
/// edge (~14.4 yards) feathers any terrain intersection smoothly without
/// attenuating open-air atmospheric haze.
inline constexpr float kMistSoftFadeFactor = 0.40f;

/// World-space soft-particle fade distance (yards) for a mist puff of `puffSize` yards.
[[nodiscard]] inline float mistSoftFadeRange(float puffSize) {
    assert(puffSize > 0.0f);
    return std::max(puffSize * kMistSoftFadeFactor, 1e-3f);
}

/// Convert Vulkan [0, 1] NDC depth to linear view-space depth in yards.
/// Matches `linearizeDepth` in `water.frag.glsl` and `weather_mist.frag.glsl`.
[[nodiscard]] inline float linearizeMistDepth(float ndcDepth, float nearPlane, float farPlane) {
    assert(nearPlane > 0.0f && farPlane > nearPlane);
    return nearPlane * farPlane / (farPlane - ndcDepth * (farPlane - nearPlane));
}

/// Smooth soft-particle alpha multiplier in [0, 1] given linear view-space
/// scene depth and mist fragment depth (both in yards) and the fade range.
/// Returns 1.0 when `fadeRange <= 0.0` (depth fade disabled).
[[nodiscard]] inline float mistSoftDepthFade(float sceneLinearDepth, float fragLinearDepth, float fadeRange) {
    if (fadeRange <= 0.0f) return 1.0f;
    const float depthDelta = sceneLinearDepth - fragLinearDepth;
    if (depthDelta <= 0.0f) return 0.0f;
    const float t = std::clamp(depthDelta / fadeRange, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Convenience wrapper operating directly on Vulkan [0, 1] NDC depth values.
[[nodiscard]] inline float mistSoftDepthFadeNdc(float sceneNdcDepth, float fragNdcDepth,
                                                float nearPlane, float farPlane, float fadeRange) {
    if (fadeRange <= 0.0f) return 1.0f;
    const float sceneLin = linearizeMistDepth(sceneNdcDepth, nearPlane, farPlane);
    const float fragLin = linearizeMistDepth(fragNdcDepth, nearPlane, farPlane);
    return mistSoftDepthFade(sceneLin, fragLin, fadeRange);
}

} // namespace wowee::rendering
