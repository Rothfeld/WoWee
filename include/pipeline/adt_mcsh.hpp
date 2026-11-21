#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace wowee::pipeline {

/// Native TBC 2.4.3 baked ADT MCSH terrain shadow attenuation (30% darkening,
/// i.e. 0.7x brightness in full shadow, matching LightIntBand-independent
/// shadow overlay measured across Ratchet, Crossroads, and Everlook).
inline constexpr float kMcshTerrainShadowFloor = 0.7f;

/// Unpack a 512-byte ADT MCNK MCSH 64x64 1-bit baked shadow map into a
/// 4096-byte R8_UNORM row-major texture (row = Y in [0,63], col = X in [0,63]).
/// Bit 1 = shadowed (255), bit 0 = lit (0).
inline std::array<uint8_t, 4096> unpackMcshShadowMap(const std::array<uint8_t, 512>& shadowMap) {
    std::array<uint8_t, 4096> out{};
    for (size_t row = 0; row < 64; ++row) {
        const size_t srcRow = row * 8;
        const size_t dstRow = row * 64;
        for (size_t col = 0; col < 64; ++col) {
            const bool shadowed = ((shadowMap[srcRow + (col >> 3)] >> (col & 7)) & 1u) != 0;
            out[dstRow + col] = shadowed ? 255u : 0u;
        }
    }
    return out;
}

/// Apply a separable 5-tap binomial blur ([1, 4, 6, 4, 1] / 16) with
/// clamp-to-edge to an unpacked 64x64 MCSH shadow map so 1-bit shadow edges
/// match Native TBC 2.4.3's soft penumbra instead of hard 1-texel steps.
inline std::array<uint8_t, 4096> softenMcshShadowMap(const std::array<uint8_t, 4096>& raw) {
    constexpr int kSize = 64;
    constexpr int kWeights[5] = {1, 4, 6, 4, 1};
    std::array<uint16_t, 4096> horiz{};
    for (int row = 0; row < kSize; ++row) {
        const int rowOff = row * kSize;
        for (int col = 0; col < kSize; ++col) {
            int sum = 0;
            for (int k = -2; k <= 2; ++k) {
                const int c = std::clamp(col + k, 0, kSize - 1);
                sum += static_cast<int>(raw[static_cast<size_t>(rowOff + c)]) * kWeights[k + 2];
            }
            horiz[static_cast<size_t>(rowOff + col)] = static_cast<uint16_t>(sum);
        }
    }
    std::array<uint8_t, 4096> out{};
    for (int row = 0; row < kSize; ++row) {
        const int rowOff = row * kSize;
        for (int col = 0; col < kSize; ++col) {
            int sum = 0;
            for (int k = -2; k <= 2; ++k) {
                const int r = std::clamp(row + k, 0, kSize - 1);
                sum += static_cast<int>(horiz[static_cast<size_t>(r * kSize + col)]) * kWeights[k + 2];
            }
            // Sum of weights is 16 * 16 = 256; round to nearest uint8_t.
            out[static_cast<size_t>(rowOff + col)] = static_cast<uint8_t>(std::clamp((sum + 128) >> 8, 0, 255));
        }
    }
    return out;
}

/// Terrain MCSH multiplier matching assets/shaders/terrain.frag.glsl.
/// When dynamic shadows are active, prevents double-darkening fragments that
/// are already darkened by the dynamic shadow map.
inline float computeTerrainMcshFactor(float mcsh, bool dynamicShadowsEnabled, float dynamicShadow) {
    const float clampedMcsh = std::clamp(mcsh, 0.0f, 1.0f);
    const float mcshFactor = 1.0f + (kMcshTerrainShadowFloor - 1.0f) * clampedMcsh;
    if (!dynamicShadowsEnabled) {
        return mcshFactor;
    }
    const float clampedDyn = std::clamp(dynamicShadow, 0.0f, 1.0f);
    const float dynDarken = kMcshTerrainShadowFloor + (1.0f - kMcshTerrainShadowFloor) * clampedDyn;
    return std::min(1.0f, mcshFactor / std::max(dynDarken, kMcshTerrainShadowFloor));
}

}  // namespace wowee::pipeline
