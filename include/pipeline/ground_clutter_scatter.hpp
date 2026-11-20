#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"

namespace wowee {
namespace pipeline {

/// Minimal DBC effect record needed by the ground-clutter scatterer.
struct GroundClutterEffectSpec {
    std::array<uint32_t, 4> doodadIds{};
    std::array<uint32_t, 4> weights{};
    uint32_t density = 0;
};

/// One scattered detail doodad in chunk-local quad coordinates ([0, 8) x [0, 8)).
struct GroundClutterSample {
    int qx = 0;
    int qy = 0;
    float fracX = 0.0f;
    float fracY = 0.0f;
    uint32_t doodadId = UINT32_MAX;
    float yaw = 0.0f;
    float scale = 1.0f;
};

/// Convert WoWee's 0..1.5 ground-clutter density scale (0%..150% UI slider)
/// into TBC 2.4.3's `groundEffectDensity` (0..64 UI slider range, where
/// 24 is default = 56.25% and 64 is max slider = 150%).
inline float effectiveGroundEffectDensity(float groundClutterDensityScale) {
    return std::clamp(groundClutterDensityScale, 0.0f, 1.5f) * (64.0f / 1.5f);
}

inline uint32_t mixGroundClutterSeed(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/// Scatter ground-effect detail doodads across one 8x8 MCNK chunk.
///
/// TBC 2.4.3's `groundEffectDensity` (16..64 on the UI slider, up to 256 via
/// console) scales out of 256, not 64, and distributes tufts as chunk-level
/// clumps rather than uniform per-cell dots:
///   - A chunk performs `N = groundEffectDensity` deterministic cell picks
///     from the 64 quads (with replacement).
///   - Each pick landing on a valid growing quad spawns `ge.density / 4` tufts
///     (with stochastic rounding of any fractional remainder).
///   - Expected tufts per growing cell = (N / 64) * (density / 4) = density * N / 256.
///   - Active cell fraction = 1 - (63/64)^N (e.g. 31.5% at N=24, 63.5% at N=64),
///     matching native 2.4.3's patchy road/meadow clutter distribution.
///   - Each pick index derives its own sub-seed so raising density preserves
///     every lower-density clump identically.
template <typename LookupEffectFn, typename EmitFn>
size_t scatterChunkGroundClutter(const MapChunk& chunk,
                                 int tileX, int tileY, int cx, int cy,
                                 float groundClutterDensityScale,
                                 LookupEffectFn&& lookupEffect,
                                 EmitFn&& emit,
                                 size_t* outNoEffectMatch = nullptr) {
    if (!chunk.hasHeightMap() || chunk.layers.empty()) return 0;

    const float effectiveDensity = effectiveGroundEffectDensity(groundClutterDensityScale);
    if (effectiveDensity <= 0.0f) return 0;

    const uint32_t chunkKey =
        (static_cast<uint32_t>(tileX & 0x3F) << 20) |
        (static_cast<uint32_t>(tileY & 0x3F) << 14) |
        (static_cast<uint32_t>(cy & 0xF) << 10) |
        (static_cast<uint32_t>(cx & 0xF) << 6);
    const uint32_t chunkSeed = mixGroundClutterSeed(chunkKey ^ 0xA511E9B3u);

    uint32_t numPicks = static_cast<uint32_t>(effectiveDensity);
    const float fracPicks = effectiveDensity - static_cast<float>(numPicks);
    if (fracPicks > 0.0f) {
        const uint32_t roundRoll = mixGroundClutterSeed(chunkSeed ^ 0x9E3779B9u);
        if (static_cast<float>(roundRoll >> 8) / 16777216.0f < fracPicks) {
            ++numPicks;
        }
    }

    constexpr float kTwoPi = 2.0f * core::coords::PI;
    constexpr float kRand16Max = 65535.0f;
    size_t attemptsTotal = 0;

    for (uint32_t pick = 0; pick < numPicks; ++pick) {
        uint32_t seed = mixGroundClutterSeed(chunkSeed ^ (pick * 0x85EBCA6Bu + 0xC2B2AE35u));
        auto nextRand = [&seed]() -> uint32_t {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };

        const int cell = static_cast<int>((nextRand() >> 16) & 63u);
        const int qx = cell % 8;
        const int qy = cell / 8;

        if (chunk.isHole(qy, qx) || chunk.isEffectDisabled(qy, qx)) continue;
        const size_t layerIdx = chunk.effectLayerFor(qy, qx);
        if (layerIdx >= chunk.layers.size()) continue;
        const auto& layer = chunk.layers[layerIdx];
        if (layer.effectId == 0) continue;

        const GroundClutterEffectSpec* ge = lookupEffect(layer.effectId);
        if (!ge) {
            if (outNoEffectMatch) ++(*outNoEffectMatch);
            continue;
        }

        const uint32_t totalWeight = ge->weights[0] + ge->weights[1] + ge->weights[2] + ge->weights[3];
        if (ge->density == 0 || totalWeight == 0) continue;

        const float clumpCount = static_cast<float>(ge->density) * 0.25f;
        uint32_t attempts = static_cast<uint32_t>(clumpCount);
        if (static_cast<float>(nextRand() >> 8) / 16777216.0f < clumpCount - static_cast<float>(attempts)) {
            ++attempts;
        }
        attemptsTotal += attempts;

        for (uint32_t a = 0; a < attempts; ++a) {
            const float fracX = static_cast<float>(qx) + static_cast<float>(nextRand() >> 8) / 16777216.0f;
            const float fracY = static_cast<float>(qy) + static_cast<float>(nextRand() >> 8) / 16777216.0f;

            const uint32_t roll = (nextRand() >> 8) % totalWeight;
            int slot = 0;
            uint32_t acc = 0;
            for (int i = 0; i < 4; ++i) {
                acc += ge->weights[i];
                if (roll < acc) {
                    slot = i;
                    break;
                }
            }
            const uint32_t doodadId = ge->doodadIds[slot];
            if (doodadId == UINT32_MAX) continue;

            const float yaw = static_cast<float>(nextRand() & 0xFFFFu) / kRand16Max * kTwoPi;
            const float scale = 0.80f + (static_cast<float>(nextRand() & 0xFFFFu) / kRand16Max) * 0.35f;
            emit(GroundClutterSample{qx, qy, fracX, fracY, doodadId, yaw, scale});
        }
    }

    return attemptsTotal;
}

} // namespace pipeline
} // namespace wowee
