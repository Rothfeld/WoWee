#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Live target snapshot sampled per frame for a homing spell or ranged projectile.
struct MissileTargetSample {
    glm::vec3 position{0.0f};
    uint32_t attachInstanceId = 0;
};

/// Result of advancing a homing projectile by one simulation step.
struct HomingStepResult {
    glm::vec3 position{0.0f};
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    glm::vec3 rotation{0.0f};
    float animProgress = 0.0f;
    bool arrived = false;
};

/// Convert a unit flight direction (+X forward model convention) to placement Euler
/// angles (roll=0, pitch=y, yaw=z) compatible with placementModelMatrix (Z, Y, X).
inline glm::vec3 missileDirectionToEuler(const glm::vec3& dir) {
    glm::vec3 rotation(0.0f);
    const float len = glm::length(dir);
    if (len > 1e-4f && std::isfinite(len)) {
        const glm::vec3 u = dir / len;
        rotation.z = std::atan2(u.y, u.x);
        rotation.y = -std::asin(std::clamp(u.z, -1.0f, 1.0f));
    }
    return rotation;
}

/// Effective pursuit speed (yards/s) after the caster's cast-release delay.
/// Preserves native TBC 2.4.3 landing timing (initialDistance / nominalSpeed) on
/// stationary targets despite release delay ($CSL/$CSR). Returns 0 when the release
/// delay already meets or exceeds the landing time (close-range 1-frame cross).
inline float missileFlightSpeed(float initialDistance, float nominalSpeed, float launchDelay) {
    assert(std::isfinite(nominalSpeed) && nominalSpeed > 0.0f);
    assert(std::isfinite(launchDelay) && launchDelay >= 0.0f);
    if (!std::isfinite(initialDistance) || initialDistance <= 1e-4f) return 0.0f;
    const float nominalDuration = initialDistance / nominalSpeed;
    const float flightWindow = nominalDuration - launchDelay;
    if (flightWindow <= 1e-4f) return 0.0f;
    return initialDistance / flightWindow;
}

/// Upper safety bound on homing flight duration so a projectile chasing a teleported
/// or uncatchable target cannot remain active indefinitely.
inline float missileMaxFlightDuration(float initialDuration) {
    const float safeDur = (std::isfinite(initialDuration) && initialDuration > 0.0f) ? initialDuration : 0.0f;
    return std::max(safeDur * 5.0f, 15.0f);
}

/// Advance a homing projectile from currentPos toward live targetPos at flightSpeed.
/// Never overshoots on large dt, handles zero-distance safely without NaN, and updates
/// distanceTraveled so monotonic animation progress reaches 1.0 on arrival.
inline HomingStepResult stepHomingMissile(const glm::vec3& currentPos,
                                          const glm::vec3& targetPos,
                                          const glm::vec3& prevDir,
                                          float flightSpeed,
                                          float dt,
                                          float& distanceTraveled) {
    const glm::vec3 toTarget = targetPos - currentPos;
    const float remaining = glm::length(toTarget);
    const glm::vec3 fallbackDir = (glm::length(prevDir) > 1e-4f) ? glm::normalize(prevDir) : glm::vec3(1.0f, 0.0f, 0.0f);

    if (!std::isfinite(remaining) || remaining <= 1e-4f) {
        return {
            .position = targetPos,
            .direction = fallbackDir,
            .rotation = missileDirectionToEuler(fallbackDir),
            .animProgress = 1.0f,
            .arrived = true,
        };
    }

    const glm::vec3 dir = toTarget / remaining;
    const glm::vec3 rot = missileDirectionToEuler(dir);

    if (!std::isfinite(flightSpeed) || flightSpeed <= 0.0f) {
        distanceTraveled += remaining;
        return {
            .position = targetPos,
            .direction = dir,
            .rotation = rot,
            .animProgress = 1.0f,
            .arrived = true,
        };
    }

    const float safeDt = (std::isfinite(dt) && dt > 0.0f) ? dt : 0.0f;
    const float step = flightSpeed * safeDt;
    if (step >= remaining) {
        distanceTraveled += remaining;
        return {
            .position = targetPos,
            .direction = dir,
            .rotation = rot,
            .animProgress = 1.0f,
            .arrived = true,
        };
    }

    distanceTraveled += step;
    const float remainingAfter = remaining - step;
    const float totalEst = distanceTraveled + remainingAfter;
    const float animProgress = (totalEst > 1e-4f) ? std::clamp(distanceTraveled / totalEst, 0.0f, 1.0f) : 1.0f;
    return {
        .position = currentPos + dir * step,
        .direction = dir,
        .rotation = rot,
        .animProgress = animProgress,
        .arrived = false,
    };
}

}  // namespace wowee::rendering
