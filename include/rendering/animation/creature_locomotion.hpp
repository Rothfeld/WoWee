#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <algorithm>
#include <cassert>
#include <optional>

namespace wowee::rendering {

// WALKING selects ground speed, but does not turn a swimmer into a land animal.
inline constexpr uint32_t creatureMovingAnimation(bool flying, bool swimming, bool walking) {
    if (flying) return anim::FLY_FORWARD;
    if (swimming) return anim::SWIM;
    return walking ? anim::WALK : anim::RUN;
}

inline float creatureSwimmingZ(float serverZ, bool flying, bool swimming,
                               std::optional<float> surface, float authoredDepth) {
    if (flying || !swimming || !surface) return serverZ;
    assert(authoredDepth > 0.0f);
    return std::min(serverZ, *surface - authoredDepth);
}

} // namespace wowee::rendering
