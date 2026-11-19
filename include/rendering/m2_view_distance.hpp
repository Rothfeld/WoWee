#pragma once

#include <algorithm>

namespace wowee {
namespace rendering {

/// The distance at which one M2 instance stops drawing, squared.
///
/// Scenery: two inputs push an instance further out than the scene's base
/// distance: a large model gets a factor above 1 so a cathedral does not pop
/// before a shrub does, and an animation-disabled model gets a further 2.6x.
/// Both are biases within the drawn world, not licence to leave it. The
/// terrain and the WMOs stop at the view distance the player asked for, so an
/// instance past that ceiling is a tree standing on nothing - which is why the
/// clamp comes last, after every widening rather than before it.
///
/// Server objects (game objects, the emitters of units, spell visuals) draw to
/// the view distance itself: what the server keeps in range is what is seen,
/// as with the units CharacterRenderer draws (setViewDistance there).
inline float m2InstanceMaxDistSq(float baseMaxDistSq,
                                 float perInstanceFactor,
                                 bool serverVisibility,
                                 float viewDistanceAbsolute,
                                 bool isGroundDetail = false,
                                 float groundDetailMaxDistance = 0.0f) {
    const float viewSq = viewDistanceAbsolute * viewDistanceAbsolute;
    if (serverVisibility) return viewSq;
    float maxDistSq = baseMaxDistSq * perInstanceFactor;
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
