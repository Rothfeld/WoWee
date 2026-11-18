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

}  // namespace wowee::rendering
