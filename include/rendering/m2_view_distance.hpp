#pragma once

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

/// TBC 2.4.3 SmallCull threshold for WorldDetail slider levels 0..2
/// (OptionsFrame.lua: Low = 0.07, Medium = 0.04, High = 0.01).
constexpr float tbcSmallCullForWorldDetail(int worldDetail) noexcept {
    if (worldDetail <= 0) return 0.07f;
    if (worldDetail == 1) return 0.04f;
    return 0.01f;
}

/// Inverse mapping from TBC SmallCull threshold back to WorldDetail 0..2.
constexpr int tbcWorldDetailForSmallCull(float smallCull) noexcept {
    if (smallCull <= 0.025f) return 2;
    if (smallCull <= 0.055f) return 1;
    return 0;
}

/// Continuous mapping from WoWee's Environment Detail (0.5..1.5, default 1.0)
/// onto TBC 2.4.3's SmallCull range (0.5 -> 0.07, 1.0 -> 0.04, 1.5 -> 0.01).
inline float tbcSmallCullForEnvironmentDetail(float detail) noexcept {
    const float clamped = std::clamp(detail, 0.5f, 1.5f);
    return std::clamp(0.10f - 0.06f * clamped, 0.01f, 0.07f);
}

/// TBC 2.4.3 opaque draw distance (yards from camera eye) for a placed M2 doodad.
///
/// Outdoor ADT MDDF doodads project their scaled M2 header vertexRadius (0xCC)
/// with focal factor ~2.5 against SmallCull (0.07 / 0.04 / 0.01) with a 0.22-yd
/// minimum radius floor. Indoor WMO MODD doodads use TBC's fixed 0.04 baseline
/// screen threshold and a 0.40-yd minimum radius floor (25.0 yd for small props
/// such as floor skulls). Optional DistCull caps maximum opaque range when > 0.
inline float tbcDoodadOpaqueMaxDistance(float vertexRadius,
                                        float smallCull,
                                        float distCull = 0.0f,
                                        bool wmoDoodad = false) noexcept {
    const float effSmallCull = wmoDoodad ? 0.04f : std::max(smallCull, 0.001f);
    const float minRadius = wmoDoodad ? 0.40f : 0.22f;
    const float radius = std::max(vertexRadius, minRadius);
    float dist = (2.5f * radius) / effSmallCull;
    if (distCull > 0.0f) {
        dist = std::min(dist, distCull);
    }
    return dist;
}

/// Width (in yards) of the alpha fade band beyond tbcDoodadOpaqueMaxDistance
/// when CVar objectFade is enabled ("1" by default in TBC 2.4.3).
inline float tbcDoodadFadeBand(float opaqueDist, bool objectFadeEnabled) noexcept {
    if (!objectFadeEnabled || opaqueDist <= 0.0f) return 0.0f;
    return std::clamp(opaqueDist * 0.20f, 4.0f, 25.0f);
}

/// Per-instance alpha multiplier in [0, 1] across [opaqueDist, opaqueDist + fadeBand].
inline float tbcDoodadDistanceAlpha(float distSq, float opaqueDist, float fadeBand) noexcept {
    const float maxDist = opaqueDist + std::max(fadeBand, 0.0f);
    if (distSq >= maxDist * maxDist) return 0.0f;
    if (fadeBand <= 0.0f || distSq <= opaqueDist * opaqueDist) return 1.0f;
    const float dist = std::sqrt(std::max(distSq, 0.0f));
    return std::clamp((maxDist - dist) / fadeBand, 0.0f, 1.0f);
}

/// The distance at which one M2 instance stops drawing, squared.
///
/// Placed scenery doodads draw to their TBC SmallCull projection distance
/// (plus optional objectFade band) when doodadMaxDistance > 0, or to the
/// scene's base distance scaled by perInstanceFactor otherwise, clamped to the
/// terrain/WMO view distance ceiling so no doodad stands on nothing.
///
/// Server objects (game objects, the emitters of units, spell visuals) draw to
/// the view distance itself: what the server keeps in range is what is seen,
/// as with the units CharacterRenderer draws (setViewDistance there).
inline float m2InstanceMaxDistSq(float baseMaxDistSq,
                                 float perInstanceFactor,
                                 bool serverVisibility,
                                 float viewDistanceAbsolute,
                                 bool isGroundDetail = false,
                                 float groundDetailMaxDistance = 0.0f,
                                 float doodadMaxDistance = 0.0f) {
    const float viewSq = viewDistanceAbsolute * viewDistanceAbsolute;
    if (serverVisibility) return viewSq;
    float maxDistSq = (!isGroundDetail && doodadMaxDistance > 0.0f)
        ? (doodadMaxDistance * doodadMaxDistance)
        : (baseMaxDistSq * perInstanceFactor);
    // Grass and the rest of the ground cover stop far closer than anything
    // else, and are their own setting for it: the original client draws them
    // between 70 and 140 yards while doodads go out to the horizon. A ceiling
    // rather than a scale, because it is a distance the player names outright.
    if (isGroundDetail && groundDetailMaxDistance > 0.0f) {
        maxDistSq = std::min(maxDistSq, groundDetailMaxDistance * groundDetailMaxDistance);
    }
    return std::min(maxDistSq, viewSq);
}

} // namespace rendering
} // namespace wowee
