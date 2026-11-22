#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string_view>
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

/// Valid Spell.dbc Speed range (yards/s) for launching a spell missile visual.
/// Includes near-stationary directional flash bolts such as Kael'thas's Pure Nether
/// Beam (36196/36197/36198, Speed=0.01, arcanelightning*.m2) and slow arcs (Juggle
/// Torch 45638, Speed=0.5), while rejecting zero/negative/unbounded garbage values.
inline constexpr float kMaxSpellMissileSpeed = 200.0f;
inline constexpr float kStationaryFlashMissileSpeedThreshold = 0.1f;

inline bool isValidSpellMissileSpeed(float speed) {
    return std::isfinite(speed) && speed > 0.0f && speed <= kMaxSpellMissileSpeed;
}

/// True when a missile has a sub-crawling speed (< 0.1 yd/s, e.g. 0.01 yd/s on
/// Pure Nether Beam 36196-36198) whose nominal flight time (distance / speed)
/// exceeds its M2 animation length. Such models are directional lightning/beam
/// flashes whose animation plays in real time and retires at animDurationSec
/// rather than freezing at progress ~0 for thousands of seconds.
inline bool isStationaryFlashMissile(float nominalSpeed, float flightDuration, float animDurationSec) {
    return std::isfinite(nominalSpeed) && nominalSpeed > 0.0f &&
           nominalSpeed < kStationaryFlashMissileSpeedThreshold &&
           std::isfinite(animDurationSec) && animDurationSec > 0.0f &&
           flightDuration > animDurationSec;
}

/// Maximum catch-up speed multiplier applied when a caster's release event
/// ($CSL/$CSR/$CST) eats most or all of the nominal flight time.
/// Measured at 30 Hz on Native TBC 2.4.3:
///   - Imp Firebolt (3110, speed 16, $CST 0.766s) at 10 yd & 15 yd: ~33-34 yd/s (~2x)
///   - Mage Fireball (133, speed 24, $CSL 0.200s) at 6 yd: ~48-50 yd/s (~2x)
///   - Mage Fireball (133, speed 24, $CSL 0.200s) at 15 yd: ~35-40 yd/s (uncapped 1.47x)
inline constexpr float kMaxMissileCatchupFactor = 2.0f;

/// Effective pursuit speed (yards/s) after the caster's cast-release delay.
/// Preserves native TBC 2.4.3 landing timing (initialDistance / nominalSpeed) on
/// stationary targets when the remaining flight window is wide enough, and caps
/// close-range catch-up at kMaxMissileCatchupFactor * nominalSpeed so missiles
/// visibly travel instead of teleporting in 1 frame into an instantaneous beam.
inline float missileFlightSpeed(float initialDistance, float nominalSpeed, float launchDelay) {
    assert(std::isfinite(nominalSpeed) && nominalSpeed > 0.0f);
    assert(std::isfinite(launchDelay) && launchDelay >= 0.0f);
    if (!std::isfinite(initialDistance) || initialDistance <= 1e-4f) return 0.0f;
    const float maxSpeed = nominalSpeed * kMaxMissileCatchupFactor;
    const float nominalDuration = initialDistance / nominalSpeed;
    const float flightWindow = nominalDuration - launchDelay;
    if (flightWindow <= 1e-4f) return maxSpeed;
    return std::min(initialDistance / flightWindow, maxSpeed);
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

/// Stuck-arrow lifetime and fade constants measured on Native TBC 2.4.3 (build 8606):
/// Arcatraz Warder arrows stick in the target's torso for ~3.0 s, tracking chest bone
/// rotation/movement and fading out cleanly at expiry.
inline constexpr float kStuckArrowLifetimeSec = 3.0f;
inline constexpr float kStuckArrowFadeSec = 0.5f;
inline constexpr size_t kMaxStuckArrowsPerTarget = 8u;

/// True when a physical ranged projectile should remain embedded in the target's
/// body on arrival (arrows that hit; excludes bullets, spinning thrown weapons,
/// and missed shots).
inline bool shouldStickPhysicalProjectile(std::string_view modelPath, bool spin, bool hitTarget) {
    if (spin || !hitTarget || modelPath.empty()) return false;
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string_view base = (slash == std::string_view::npos) ? modelPath : modelPath.substr(slash + 1);
    constexpr std::string_view kNeedle = "arrow";
    if (base.size() < kNeedle.size()) return false;
    for (size_t i = 0; i + kNeedle.size() <= base.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < kNeedle.size(); ++j) {
            const char c = base[i + j];
            const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (lower != kNeedle[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/// Opacity (1..0) of a stuck arrow at `stuckElapsed` seconds after impact.
inline float stuckArrowOpacity(float stuckElapsed,
                               float lifetime = kStuckArrowLifetimeSec,
                               float fadeSec = kStuckArrowFadeSec) {
    if (!std::isfinite(stuckElapsed) || stuckElapsed <= 0.0f) return 1.0f;
    if (stuckElapsed >= lifetime) return 0.0f;
    const float fadeStart = std::max(0.0f, lifetime - fadeSec);
    if (stuckElapsed <= fadeStart || fadeSec <= 1e-4f) return 1.0f;
    return std::clamp((lifetime - stuckElapsed) / fadeSec, 0.0f, 1.0f);
}

/// Target-chest-local pose for an embedded arrow so it translates and rotates
/// rigidly with the target's torso bone without inheriting bone scale.
struct StuckArrowLocalPose {
    glm::vec3 localOffset{0.0f};
    glm::vec3 localDir{1.0f, 0.0f, 0.0f};
};

inline glm::mat3 orthonormalAttachBasis(const glm::mat4& attachMat) {
    glm::vec3 c0(attachMat[0]);
    glm::vec3 c1(attachMat[1]);
    glm::vec3 c2(attachMat[2]);
    const float l0 = glm::length(c0);
    const float l1 = glm::length(c1);
    const float l2 = glm::length(c2);
    c0 = (l0 > 1e-4f) ? (c0 / l0) : glm::vec3(1.0f, 0.0f, 0.0f);
    c1 = (l1 > 1e-4f) ? (c1 / l1) : glm::vec3(0.0f, 1.0f, 0.0f);
    c2 = (l2 > 1e-4f) ? (c2 / l2) : glm::vec3(0.0f, 0.0f, 1.0f);
    return glm::mat3(c0, c1, c2);
}

/// Convert world flight direction at impact into the target chest attachment's
/// local frame with a small deterministic spread across `slotIndex` so multiple
/// arrows on the same unit do not overlap identically.
inline StuckArrowLocalPose computeStuckArrowLocalPose(const glm::mat4& attachMat,
                                                      const glm::vec3& worldDir,
                                                      uint32_t slotIndex) {
    const glm::mat3 basis = orthonormalAttachBasis(attachMat);
    const glm::vec3 normDir = (glm::length(worldDir) > 1e-4f)
        ? glm::normalize(worldDir) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 localDir = glm::normalize(glm::transpose(basis) * normDir);
    // Small chest-local scatter (y = lateral, z = vertical). The arrow M2's
    // Impact animation (anim::IMPACT = 191) scales Bone 1 to 0.5025 and shifts
    // it by -0.738 yd along +X so the arrowhead embeds at the attachment origin
    // while the fletching protrudes out the incoming (-localDir) side.
    static constexpr glm::vec2 kScatter[8] = {
        { 0.00f,  0.04f},
        {-0.09f,  0.10f},
        { 0.09f, -0.04f},
        {-0.06f, -0.09f},
        { 0.11f,  0.08f},
        {-0.11f,  0.01f},
        { 0.05f,  0.12f},
        {-0.04f, -0.12f},
    };
    const glm::vec2 jitter = kScatter[slotIndex % 8u];
    const glm::vec3 localOffset = glm::vec3(0.0f, jitter.x, jitter.y);
    return {localOffset, localDir};
}

inline void evaluateStuckArrowWorldPose(const glm::mat4& attachMat,
                                        const StuckArrowLocalPose& local,
                                        glm::vec3& outPos,
                                        glm::vec3& outRot) {
    const glm::mat3 basis = orthonormalAttachBasis(attachMat);
    outPos = glm::vec3(attachMat[3]) + basis * local.localOffset;
    const glm::vec3 worldDir = basis * local.localDir;
    outRot = missileDirectionToEuler(worldDir);
}

}  // namespace wowee::rendering
