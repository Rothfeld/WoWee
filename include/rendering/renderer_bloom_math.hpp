#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Reference vertical resolution for TBC 2.4.3 ffxGlow spatial calibration.
/// Empirical black-box captures on Native TBC 2.4.3 (build 8606) at 1280x720
/// across Stratholme Alonsus Chapel (LP 336, glow=0.50), Teldrassil (LP 208,
/// glow=1.00), Molten Core (LP 12, glow=0.65), and The Stormspire (LP 552,
/// glow=0.30) match:
///   out = clamp(scene + LP.glow * blur(scene)^2, 0.0, 1.0)
/// with total Gaussian sigma = 5.75 px at 720p (MAE = 0.40/255), corresponding
/// to two 2x2 box downsamples (ffxbox4.bls -> 1/4 resolution) followed by
/// separable Gaussian passes (ffxgauss4.bls) and squared additive composite
/// (ffxglow.bls) ahead of the 2D UI pass.
inline constexpr float kTbcFfxGlowReferenceHeight = 720.0f;
inline constexpr float kTbcFfxGlowTotalSigmaAt720p = 5.75f;
inline constexpr float kTbcFfxGlowQuarterSigmaAt720p = 5.35f / 4.0f;

struct BloomTargetExtents {
    uint32_t halfWidth = 1;
    uint32_t halfHeight = 1;
    uint32_t quarterWidth = 1;
    uint32_t quarterHeight = 1;
};

inline constexpr BloomTargetExtents computeBloomTargetExtents(uint32_t width, uint32_t height) {
    const uint32_t hw = std::max(1u, width / 2u);
    const uint32_t hh = std::max(1u, height / 2u);
    return {
        .halfWidth = hw,
        .halfHeight = hh,
        .quarterWidth = std::max(1u, hw / 2u),
        .quarterHeight = std::max(1u, hh / 2u),
    };
}

/// Blur sigma in quarter-resolution texels, scaled proportionally with screen
/// height so bloom halo screen coverage stays constant across 720p/1080p/4K.
inline float computeQuarterBlurSigmaTexels(uint32_t quarterHeight) {
    const float refQuarterHeight = kTbcFfxGlowReferenceHeight * 0.25f;
    return kTbcFfxGlowQuarterSigmaAt720p *
           (static_cast<float>(std::max(1u, quarterHeight)) / refQuarterHeight);
}

/// Spacing in quarter-resolution texels between adjacent taps of the 9-tap
/// separable Gaussian kernel. Keeping step >= sigma / 1.3375 ensures [-4..+4]
/// taps always span at least +-3.0 sigma (99.7% of Gaussian energy) at high
/// display resolutions while step == 1.0 texel at 720p and below.
inline float computeQuarterBlurTapStepTexels(float sigmaTexels) {
    if (sigmaTexels <= kTbcFfxGlowQuarterSigmaAt720p) {
        return 1.0f;
    }
    return sigmaTexels / kTbcFfxGlowQuarterSigmaAt720p;
}

/// Normalized 9-tap symmetric 1D Gaussian weights for offsets k = -4..+4.
inline std::array<float, 9> computeGaussianTapWeights9(float sigmaTexels, float tapStepTexels) {
    std::array<float, 9> weights{};
    const float safeSigma = std::max(1e-4f, sigmaTexels);
    float sum = 0.0f;
    for (int k = -4; k <= 4; ++k) {
        const float d = (static_cast<float>(k) * tapStepTexels) / safeSigma;
        const float w = std::exp(-0.5f * d * d);
        weights[static_cast<size_t>(k + 4)] = w;
        sum += w;
    }
    if (sum > 0.0f) {
        const float inv = 1.0f / sum;
        for (float& w : weights) w *= inv;
    }
    return weights;
}

/// Effective TBC 2.4.3 ffxGlow strength for the current frame.
/// - Requires TBC expansion, an active 3D world frame, renderer/CVar enables
///   (`ffx != 0` and `ffxGlow != 0`), and `netherWorldActive == false` (in TBC
///   2.4.3, Nether World invisibility replaces ffxGlow).
inline constexpr float effectiveTbcFfxGlowStrength(bool expansionIsTbc,
                                                   bool worldDrawn,
                                                   bool rendererEnabled,
                                                   bool ffxMasterEnabled,
                                                   bool ffxGlowEnabled,
                                                   bool netherWorldActive,
                                                   float dbcGlow) {
    if (!expansionIsTbc || !worldDrawn || !rendererEnabled ||
        !ffxMasterEnabled || !ffxGlowEnabled || netherWorldActive) {
        return 0.0f;
    }
    if (dbcGlow <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(dbcGlow, 0.0f, 1.0f);
}

/// Exact TBC 2.4.3 ffxglow.bls composite transfer curve:
///   result.rgb = clamp(scene.rgb + glow * (blurred.rgb * blurred.rgb), 0.0, 1.0)
inline glm::vec3 evaluateTbcFfxGlowComposite(const glm::vec3& scene,
                                             const glm::vec3& blurred,
                                             float glow) {
    const float g = std::clamp(glow, 0.0f, 1.0f);
    return glm::clamp(scene + g * (blurred * blurred), glm::vec3(0.0f), glm::vec3(1.0f));
}

}  // namespace wowee::rendering
