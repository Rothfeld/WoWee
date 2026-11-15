#pragma once

#include "core/coordinates.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::core {

// Native conversation turns settle in about 50 ms. The interval belongs to
// the heading change, not to each frame or repeated gossip response.
struct NpcInteractionFacing {
    float current;
    float target;
    float remaining = 0.0f;
    int turnDirection = 0; // +1 left, -1 right; event for this update only.

    explicit NpcInteractionFacing(float heading) : current(heading), target(heading) {}

    float advance(float requested, float dt) {
        assert(std::isfinite(requested) && std::isfinite(dt) && dt >= 0.0f);
        turnDirection = 0;
        if (std::abs(coords::normalizeAngleRad(requested - target)) > 0.0001f) {
            const float turn = coords::normalizeAngleRad(requested - current);
            if (std::abs(turn) > 0.01f) turnDirection = turn > 0.0f ? 1 : -1;
            target = requested;
            remaining = 0.05f;
        }
        const float elapsed = std::min(dt, remaining);
        if (remaining > 0.0f) {
            current = coords::normalizeAngleRad(current +
                coords::normalizeAngleRad(target - current) * (elapsed / remaining));
            remaining -= elapsed;
        }
        return current;
    }
};

} // namespace wowee::core
