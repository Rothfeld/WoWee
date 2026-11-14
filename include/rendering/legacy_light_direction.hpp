#pragma once

#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering {

// Build 8606's surface light is independent of the celestial sun. Its polar
// angle alternates between 127 and 110 degrees every six hours; azimuth is 225.
// The original uses a periodic cubic cosine approximation for these angles.
inline glm::vec3 legacySurfaceLightDirection(float hours) {
    const float quadrant = hours / 6.0f;
    const float whole = std::floor(quadrant);
    const float fraction = quadrant - whole;
    const float phase = (static_cast<int>(whole) & 1) ? 1.0f - fraction : fraction;
    const float polar = (127.0f - 17.0f * phase) / 180.0f;
    const auto cosine = [](float halfTurns) {
        const float whole = std::floor(halfTurns);
        const float t = halfTurns - whole;
        const float value = 1.0f - t * t * (6.0f - 4.0f * t);
        return (static_cast<int>(whole) & 1) ? -value : value;
    };
    const float horizontal = cosine(polar - 0.5f);
    return {horizontal * cosine(1.25f), horizontal * cosine(0.75f), cosine(polar)};
}

} // namespace wowee::rendering
