#pragma once

/// Which light volume comes first when two of them weigh the same.
///
/// Light.dbc's volumes overlap, and a volume whose inner radius contains the
/// player weighs exactly 1.0 - not nearly, exactly, because that branch
/// assigns the constant. On the shipped maps 166 points sit inside two such
/// volumes and 23 inside three.
///
/// So weight alone does not order them. Left to std::sort - which is unstable,
/// and whose treatment of equal elements depends on the length and contents of
/// the whole sequence - the pair chosen changed as the player walked, because
/// distant volumes crossing their outer radius kept changing that sequence.
/// The blended colours are smoothed over time and survived it. The skybox is
/// not: it is whichever volume comes first in this order and names one, and a
/// sky model is swapped outright. That was the sky flickering while moving.
///
/// The tiebreak is the tighter volume, which is the right answer and not
/// merely a fixed one - a small light sits inside a big one because it
/// describes somewhere more specific, so it is the one that should win where
/// both apply fully. Light id decides the last case, so that two volumes of
/// identical extent still cannot swap places between frames.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// True when `a` should be blended before `b`.
///
/// The arguments are the three fields the order depends on rather than the
/// volume itself, so the rule can be held against a table of cases without a
/// loaded DBC behind it.
inline bool lightVolumeOrderedBefore(float weightA, float outerRadiusA, uint32_t idA,
                                     float weightB, float outerRadiusB, uint32_t idB) {
    if (weightA != weightB) return weightA > weightB;
    if (outerRadiusA != outerRadiusB) return outerRadiusA < outerRadiusB;
    return idA < idB;
}

/// Weight of a positioned Light row at a point: 1 inside its inner radius,
/// falling linearly to 0 at its outer radius, over the 3D distance from the
/// player. Measured against build 8606 across Light 653's falloff (Forge Camp:
/// Mageddon, inner 81.4, outer 127.9 yd): the original's share of Light 653
/// was 0.87, 0.69, 0.38, 0.20 at player distances 86.7, 95.1, 109.6, 118.3 yd,
/// where this gives 0.89, 0.71, 0.39, 0.21 and a smoothstep 0.96, 0.79, 0.34,
/// 0.11. Distance from the camera, or in the horizontal plane, fits worse.
inline float lightVolumeFalloff(float distance, float innerRadius, float outerRadius) {
    if (distance <= innerRadius) return 1.0f;
    if (distance >= outerRadius) return 0.0f;
    return 1.0f - (distance - innerRadius) / (outerRadius - innerRadius);
}

/// Shares of the positioned Light rows covering a point and of the map's
/// default row (Light.dbc position 0,0,0 and radius 0, e.g. Light 1 on map 0).
/// `weights` holds each row's falloff weight, most specific (smallest outer
/// radius) first, and is rewritten in place with its share; the default
/// row's share is returned.
///
/// Each row is laid over everything wider than itself by its weight, so the
/// innermost row takes over entirely inside its inner radius. In Forge Camp:
/// Mageddon, Light 653 (inner 81 yd) inside Light 520 (Hellfire, inner
/// 1779 yd) lights the camp alone in the original; matched captures of the
/// terrain agree within 3% with 653 alone and are 0.65-1.35x off per channel
/// with the two averaged. A row in its falloff leaves the rest of its share to
/// what lies under it: Light 653 at 0.4 gives Light 520 0.6, and one light at
/// 0.3 over the default row gives the default 0.7. Without a default row the
/// shares are normalized to sum to 1.
inline float layerLightWeights(std::vector<float>& weights, bool mapHasDefault) {
    float remaining = 1.0f;
    for (float& w : weights) {
        const float own = w;
        w = own * remaining;
        remaining *= 1.0f - own;
    }
    if (mapHasDefault) return remaining;
    const float covered = 1.0f - remaining;
    if (covered > 0.0f)
        for (float& w : weights) w /= covered;
    return 0.0f;
}

/// Spatial falloff and resolved clear/storm LightSkybox IDs for one Light.dbc
/// volume covering the player, ordered most-specific first (smallest outer
/// radius, then lightId). A LightSkybox ID of 0 means that LightParams defines
/// no skybox override and inherits wider enclosing volumes' skyboxes.
struct SkyVolumeCandidate {
    float falloff = 0.0f;
    uint32_t clearSkyboxId = 0;
    uint32_t stormSkyboxId = 0;
    uint32_t clearParamsId = 0;
    uint32_t stormParamsId = 0;
};

/// Resolved target weight in [0, 1] for one LightSkybox ID.
struct SkyboxIdShare {
    uint32_t skyboxId = 0;
    float weight = 0.0f;
};

/// Resolved target weight in [0, 1] for one LightParams ID contributing to the
/// sky dome gradient behind an active LightSkybox M2.
struct SkyParamsIdShare {
    uint32_t paramsId = 0;
    float weight = 0.0f;
};

/// Layer skybox weights across overlapping Light.dbc volumes and the map's
/// default row, matching TBC 2.4.3 CSkyManager behavior:
/// - Volumes whose active LightParams has lightSkyboxId == 0 (e.g. Netherstorm
///   Eco-Domes LP 552 and Manaforges LP 520 inside Light 502, Netherweb Ridge
///   LP 565 inside Light 742, Blade's Edge groves LP 554 inside Light 455) do
///   not consume skybox share, leaving enclosing zone skyboxes at full weight.
/// - Volumes that do name a skybox layer over wider skybox-bearing volumes by
///   their spatial falloff, and fade smoothly across zone borders where wider
///   volumes or the map default row have lightSkyboxId == 0.
inline std::vector<SkyboxIdShare> layerLightSkyboxWeights(
    const std::vector<SkyVolumeCandidate>& volumes,
    float stormWeight,
    uint32_t defaultClearSkyboxId,
    uint32_t defaultStormSkyboxId,
    bool mapHasDefault,
    uint32_t defaultClearParamsId = 0,
    uint32_t defaultStormParamsId = 0,
    std::vector<SkyParamsIdShare>* outSkyParamsShares = nullptr) {
    if (outSkyParamsShares) outSkyParamsShares->clear();
    const float clampedStorm = std::clamp(stormWeight, 0.0f, 1.0f);
    const float wClear = (clampedStorm <= 0.001f)
        ? 1.0f
        : ((clampedStorm >= 0.999f) ? 0.0f : (1.0f - clampedStorm));
    const float wStorm = 1.0f - wClear;

    std::vector<SkyboxIdShare> out;
    auto accumulate = [&](uint32_t skyboxId, uint32_t paramsId, float add) {
        if (skyboxId == 0 || add <= 0.0f) return;
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const SkyboxIdShare& s) { return s.skyboxId == skyboxId; });
        if (it == out.end()) out.push_back({.skyboxId = skyboxId, .weight = add});
        else it->weight += add;

        if (outSkyParamsShares && paramsId != 0) {
            auto pit = std::find_if(
                outSkyParamsShares->begin(), outSkyParamsShares->end(),
                [&](const SkyParamsIdShare& p) { return p.paramsId == paramsId; });
            if (pit == outSkyParamsShares->end()) {
                outSkyParamsShares->push_back({.paramsId = paramsId, .weight = add});
            } else {
                pit->weight += add;
            }
        }
    };

    float remaining = 1.0f;
    for (const auto& v : volumes) {
        const float f = std::clamp(v.falloff, 0.0f, 1.0f);
        if (f <= 0.0f || remaining <= 0.0f) continue;
        const float clearCov = (v.clearSkyboxId != 0) ? wClear : 0.0f;
        const float stormCov = (v.stormSkyboxId != 0) ? wStorm : 0.0f;
        const float own = f * (clearCov + stormCov);
        if (own <= 0.0f) continue;
        accumulate(v.clearSkyboxId, v.clearParamsId, remaining * f * clearCov);
        accumulate(v.stormSkyboxId, v.stormParamsId, remaining * f * stormCov);
        remaining *= (1.0f - own);
    }

    if (mapHasDefault) {
        if (remaining > 0.0f) {
            if (defaultClearSkyboxId != 0) {
                accumulate(defaultClearSkyboxId, defaultClearParamsId, remaining * wClear);
            }
            if (defaultStormSkyboxId != 0) {
                accumulate(defaultStormSkyboxId, defaultStormParamsId, remaining * wStorm);
            }
        }
    } else {
        const float covered = 1.0f - remaining;
        if (covered > 0.0f) {
            for (auto& s : out) s.weight /= covered;
            if (outSkyParamsShares) {
                for (auto& p : *outSkyParamsShares) p.weight /= covered;
            }
        }
    }
    return out;
}

}  // namespace wowee::rendering
