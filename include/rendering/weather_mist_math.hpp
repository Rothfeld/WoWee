#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::rendering {

/// Maximum fraction of a mist puff's quad size over which it fades to zero at
/// grazing terrain intersections on distant slopes (36 yd * 0.25 = 9.0 yd).
inline constexpr float kMistSoftFadeFactor = 0.25f;

/// Perspective-proportional slope for soft-particle depth feathering so close
/// foreground puffs (8-16 yd in downward camera views) feather terrain contact
/// locally (~2-3 yd) without losing open-air opacity over cobblestones/ground.
inline constexpr float kMistDepthFadeSlope = 0.30f;
inline constexpr float kMistMinSoftFadeRange = 1.5f;

/// 3D camera-eye near fade range (yards): puffs closer than 22 yd fade smoothly
/// to zero at 4 yd from the camera lens (~0.62 at 14.5 yd in steep downward
/// views, ~0.19 at 9 yd in immediate horizon foreground, 1.0 past 22 yd).
inline constexpr float kMistNearEyeFadeStart = 4.0f;
inline constexpr float kMistNearEyeFadeEnd = 22.0f;

/// Planar view-space depth fade range (yards) so peripheral billboards fade
/// smoothly to zero before their center crosses behind the camera plane during
/// camera turns, eliminating hard view-plane pop-in/pop-out.
inline constexpr float kMistViewDepthFadeStart = 1.0f;
inline constexpr float kMistViewDepthFadeEnd = 8.0f;

/// Horizontal radial fade margin (yards) at the rim of the yaw-invariant mist
/// cylinder so puffs fade smoothly to zero before reaching the outer boundary.
inline constexpr float kMistOuterFadeMargin = 16.0f;

/// Native TBC 2.4.3 (build 8606) rain mist grade onset curve: light/moderate
/// rain (grade <= 0.50) draws rain streaks without rolling mist puffs; heavier
/// rain ramps mist smoothly to full intensity at grade 1.00.
inline constexpr float kRainMistGradeStart = 0.50f;
inline constexpr float kRainMistGradeFull = 1.00f;

/// Radius (yards) of the yaw-invariant horizontal mist cylinder whose footprint
/// area matches the configured slab width and along-wind span.
[[nodiscard]] inline float mistCylinderRadius(float halfWidth, float nearDepth, float farDepth) {
    assert(halfWidth > 0.0f && farDepth > nearDepth);
    constexpr float kPi = 3.14159265358979323846f;
    const float slabArea = (2.0f * halfWidth) * (farDepth - nearDepth);
    return std::sqrt(slabArea / kPi);
}

/// Smooth alpha multiplier in [0, 1] fading a puff to zero as its horizontal
/// distance from the camera approaches `outerRadius`.
[[nodiscard]] inline float mistOuterRadiusFade(float horizDist, float outerRadius,
                                               float margin = kMistOuterFadeMargin) {
    assert(outerRadius > 0.0f && margin > 0.0f);
    const float effMargin = std::min(margin, outerRadius);
    const float t = std::clamp((outerRadius - horizDist) / effMargin, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Smooth alpha multiplier in [0, 1] fading a puff to zero before its center
/// crosses the camera plane (`viewDepth <= kMistViewDepthFadeStart`).
[[nodiscard]] inline float mistViewDepthFade(float viewDepth) {
    const float t = std::clamp(
        (viewDepth - kMistViewDepthFadeStart) / (kMistViewDepthFadeEnd - kMistViewDepthFadeStart),
        0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Effective rain mist intensity in [0, 1] for a given weather `amount`,
/// matching Native TBC 2.4.3's rain mist grade onset.
[[nodiscard]] inline float rainMistAmount(float weatherAmount) {
    const float t = std::clamp(
        (weatherAmount - kRainMistGradeStart) / (kRainMistGradeFull - kRainMistGradeStart),
        0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// World-space maximum soft-particle fade distance (yards) for `puffSize`.
[[nodiscard]] inline float mistSoftFadeRange(float puffSize) {
    assert(puffSize > 0.0f);
    return std::max(puffSize * kMistSoftFadeFactor, 1e-3f);
}

/// Effective soft-depth feather range (yards) at linear fragment depth `fragLinearDepth`.
[[nodiscard]] inline float mistEffectiveSoftFadeRange(float fragLinearDepth, float maxFadeRange) {
    if (maxFadeRange <= 0.0f) return 0.0f;
    const float minRange = std::min(kMistMinSoftFadeRange, maxFadeRange);
    return std::clamp(fragLinearDepth * kMistDepthFadeSlope, minRange, maxFadeRange);
}

/// Smooth 3D near-camera alpha multiplier in [0, 1] as a function of distance from eye.
[[nodiscard]] inline float mistNearCameraFade(float distanceFromEye) {
    const float t = std::clamp(
        (distanceFromEye - kMistNearEyeFadeStart) / (kMistNearEyeFadeEnd - kMistNearEyeFadeStart),
        0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Convert Vulkan [0, 1] NDC depth to linear view-space depth in yards.
/// Matches `linearizeDepth` in `water.frag.glsl` and `weather_mist.frag.glsl`.
[[nodiscard]] inline float linearizeMistDepth(float ndcDepth, float nearPlane, float farPlane) {
    assert(nearPlane > 0.0f && farPlane > nearPlane);
    return nearPlane * farPlane / (farPlane - ndcDepth * (farPlane - nearPlane));
}

/// Smooth soft-particle alpha multiplier in [0, 1] given linear view-space
/// scene depth and mist fragment depth (both in yards) and the max fade range.
/// Returns 1.0 when `fadeRange <= 0.0` (depth fade disabled).
[[nodiscard]] inline float mistSoftDepthFade(float sceneLinearDepth, float fragLinearDepth, float fadeRange) {
    if (fadeRange <= 0.0f) return 1.0f;
    const float depthDelta = sceneLinearDepth - fragLinearDepth;
    if (depthDelta <= 0.0f) return 0.0f;
    const float effRange = mistEffectiveSoftFadeRange(fragLinearDepth, fadeRange);
    const float t = std::clamp(depthDelta / effRange, 0.0f, 1.0f);
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

/// Decide whether water, swim spray, and depth-softened weather mist can leave
/// the main scene pass and draw in the single-sampled continuation pass after
/// opaque scene depth is captured.
/// Under MSAA, an off-screen post-processing target (ffxGlow, FXAA, FSR, FSR2,
/// death/nether compositing) can still use the 1x continuation pass whenever it
/// resolves both color and depth into 1x attachments.
[[nodiscard]] inline bool canUseWaterContinuePass(bool msaaEnabled,
                                                  bool hasWater1xPass,
                                                  bool offscreenScene,
                                                  bool hasResolvedOffscreenAttachments,
                                                  bool hasSceneContinuePass) {
    if (msaaEnabled) {
        if (!hasWater1xPass) return false;
        return !offscreenScene || hasResolvedOffscreenAttachments;
    }
    return hasSceneContinuePass;
}

} // namespace wowee::rendering
