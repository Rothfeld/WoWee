#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "core/coordinates.hpp"
#include "pipeline/adt_alpha.hpp"
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

/// Minimum terrain normal Z (cos(slope)) that holds ground-effect clutter.
/// Steep rocky slopes and cliff faces (~>38.7 deg) stay bare in TBC 2.4.3 even
/// when their dominant paint layer carries a non-zero GroundEffectTexture id.
inline constexpr float kGroundClutterMinNormalZ = 0.78f;

/// Quad-center terrain normal Z from MCNR ([-127, 127] -> [-1, 1]).
/// Returns 1.0 when normals are absent/unpopulated (e.g. synthetic flat chunks).
inline float chunkQuadNormalZ(const MapChunk& chunk, int qy, int qx) {
    const size_t idx = static_cast<size_t>(9 + qy * 17 + qx);
    if (idx * 3 + 2 >= chunk.normals.size()) return 1.0f;
    const int8_t nx = chunk.normals[idx * 3 + 0];
    const int8_t ny = chunk.normals[idx * 3 + 1];
    const int8_t nz = chunk.normals[idx * 3 + 2];
    if (nx == 0 && ny == 0 && nz == 0) return 1.0f;
    return static_cast<float>(nz) / 127.0f;
}

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

/// Decompose `R_tilt((0,0,1) -> surfaceNormal) * Rz(yawRadians)` into exact
/// `(ex, ey, ez)` radians for `rendering::placementModelMatrix` (`Rz(ez)*Ry(ey)*Rx(ex)`).
/// Native TBC 2.4.3 tilts all `GroundEffectDoodad` instances perpendicular to the
/// terrain triangle normal so flat rock domes (`BstRoc01/02`) and slope shrubs sit
/// flush on hillsides rather than having their uphill half buried underground.
inline glm::vec3 groundClutterSlopeEuler(const glm::vec3& surfaceNormal, float yawRadians) {
    const float lenSq = glm::dot(surfaceNormal, surfaceNormal);
    if (lenSq <= 1e-8f) {
        return glm::vec3(0.0f, 0.0f, yawRadians);
    }
    const glm::vec3 n = surfaceNormal * (1.0f / std::sqrt(lenSq));
    if (n.z <= -0.9999f) {
        return glm::vec3(core::coords::PI, 0.0f, yawRadians);
    }
    const float k = 1.0f / (1.0f + n.z);
    const float cyw = std::cos(yawRadians);
    const float syw = std::sin(yawRadians);
    const float d0 = n.x * cyw + n.y * syw;
    const float c0x = cyw - n.x * d0 * k;
    const float c0y = syw - n.y * d0 * k;
    const float c0z = -d0;
    const float c1z = n.x * syw - n.y * cyw;
    const float ey = std::asin(std::clamp(-c0z, -1.0f, 1.0f));
    const float ez = std::atan2(c0y, c0x);
    const float ex = std::atan2(c1z, n.z);
    return glm::vec3(ex, ey, ez);
}

/// Scatter ground-effect detail doodads across one 8x8 MCNK chunk.
///
/// TBC 2.4.3's `groundEffectDensity` (16..64 on the UI slider, 24 default,
/// up to 256 via console) scales out of 64 per quad:
///   - Each valid growing quad (qx, qy) spawns `ge.density * N / 64` tufts
///     (with deterministic stochastic rounding of any fractional remainder):
///     3.0 tufts/quad at N=24 / density=8, 8.0 tufts/quad at N=64 / density=8.
///   - When MCAL alpha maps are present, each sample point (fracX, fracY) checks
///     the dominant painted terrain layer so tufts on mixed road-edge quads stay
///     off bare road/snow texels whose layer carries no ground-effect doodads.
///   - Each tuft index within a quad derives its own sub-seed so raising density
///     preserves every lower-density tuft identically.
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

    // Decode MCAL alpha maps once per chunk when multi-layer alpha data is
    // present so individual tufts respect painted road/snow boundaries inside
    // mixed quads.
    const size_t nLayers = std::min<size_t>(chunk.layers.size(), 4);
    std::array<std::vector<uint8_t>, 4> layerAlphas;
    bool hasAlphaMaps = false;
    if (nLayers > 1 && !chunk.alphaMap.empty()) {
        for (size_t i = 1; i < nLayers; ++i) {
            if (decodeLayerAlpha(chunk, i, layerAlphas[i], 0)) {
                hasAlphaMaps = true;
            }
        }
    }

    auto dominantPaintedLayerAt = [&](float fracX, float fracY) -> size_t {
        const float u = std::clamp(fracX / 8.0f, 0.0f, 1.0f);
        const float v = std::clamp(fracY / 8.0f, 0.0f, 1.0f);
        float rem = 1.0f;
        size_t domLayer = 0;
        float domWeight = -1.0f;
        for (size_t i = nLayers; i-- > 1;) {
            const float a = layerAlphas[i].empty() ? 0.0f : sampleAlpha(layerAlphas[i], u, v);
            const float w = rem * a;
            if (w > domWeight) {
                domWeight = w;
                domLayer = i;
            }
            rem -= w;
        }
        if (std::max(rem, 0.0f) > domWeight) {
            domLayer = 0;
        }
        return domLayer;
    };

    constexpr float kTwoPi = 2.0f * core::coords::PI;
    constexpr float kRand16Max = 65535.0f;
    size_t attemptsTotal = 0;

    for (int cell = 0; cell < 64; ++cell) {
        const int qx = cell % 8;
        const int qy = cell / 8;

        if (chunk.isHole(qy, qx) || chunk.isEffectDisabled(qy, qx)) continue;
        if (chunkQuadNormalZ(chunk, qy, qx) < kGroundClutterMinNormalZ) continue;
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

        const uint32_t cellSeed = mixGroundClutterSeed((chunkKey | static_cast<uint32_t>(cell)) ^ 0xA511E9B3u);
        const float exactCount = static_cast<float>(ge->density) * effectiveDensity / 64.0f;
        uint32_t attempts = static_cast<uint32_t>(exactCount);
        const float fracCount = exactCount - static_cast<float>(attempts);
        if (fracCount > 0.0f) {
            const uint32_t roundRoll = mixGroundClutterSeed(cellSeed ^ 0x9E3779B9u);
            if (static_cast<float>(roundRoll >> 8) / 16777216.0f < fracCount) {
                ++attempts;
            }
        }
        attemptsTotal += attempts;

        for (uint32_t a = 0; a < attempts; ++a) {
            uint32_t seed = mixGroundClutterSeed(cellSeed ^ (a * 0x85EBCA6Bu + 0xC2B2AE35u));
            auto nextRand = [&seed]() -> uint32_t {
                seed = seed * 1664525u + 1013904223u;
                return seed;
            };

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
            const float yaw = static_cast<float>(nextRand() & 0xFFFFu) / kRand16Max * kTwoPi;
            const float scale = 0.80f + (static_cast<float>(nextRand() & 0xFFFFu) / kRand16Max) * 0.35f;
            if (doodadId == UINT32_MAX) continue;

            if (hasAlphaMaps) {
                const size_t paintedLayer = dominantPaintedLayerAt(fracX, fracY);
                if (paintedLayer != layerIdx && paintedLayer < chunk.layers.size()) {
                    const uint32_t paintedEffectId = chunk.layers[paintedLayer].effectId;
                    if (paintedEffectId == 0) continue;
                    const GroundClutterEffectSpec* paintedGe = lookupEffect(paintedEffectId);
                    if (!paintedGe || paintedGe->density == 0) continue;
                    const uint32_t paintedWeight =
                        paintedGe->weights[0] + paintedGe->weights[1] +
                        paintedGe->weights[2] + paintedGe->weights[3];
                    if (paintedWeight == 0) continue;
                }
            }

            emit(GroundClutterSample{qx, qy, fracX, fracY, doodadId, yaw, scale});
        }
    }

    return attemptsTotal;
}

} // namespace pipeline
} // namespace wowee
