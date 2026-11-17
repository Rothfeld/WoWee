#pragma once

/// Which light volume comes first when two of them weigh the same.
///
/// Light.dbc's volumes overlap, and a volume whose inner radius contains the
/// player weighs exactly 1.0 - not nearly, exactly, because that branch
/// assigns the constant. On the shipped maps 166 points sit inside two such
/// volumes and 23 inside three, and only the top two are blended.
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

/// Share of the map's default Light row (Light.dbc position 0,0,0 and radius
/// 0, e.g. Light 1 on map 0) at a point, given the falloff weights of the
/// positioned lights blended there. The weights are rescaled in place.
///
/// The default row is the light of everywhere no positioned light covers,
/// so it takes whatever the positioned lights leave: one light at 0.3 in its
/// falloff gives the default 0.7. Overlapping lights that together exceed 1
/// are normalized and leave the default nothing. Without a default row the
/// positioned lights are normalized to 1 as before.
inline float distributeLightWeights(std::vector<float>& weights, bool mapHasDefault) {
    float total = 0.0f;
    for (float w : weights) total += w;
    if (total <= 0.0f) return mapHasDefault ? 1.0f : 0.0f;
    if (total >= 1.0f || !mapHasDefault) {
        for (float& w : weights) w /= total;
        return 0.0f;
    }
    return 1.0f - total;
}

}  // namespace wowee::rendering
