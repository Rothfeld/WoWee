#pragma once

#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "core/coordinates.hpp"

#include <cstdint>
#include <cassert>
#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <mutex>
#include <chrono>
#include <glm/gtc/quaternion.hpp>
#include "math/spline.hpp"
#include "game/flat_field_map.hpp"

namespace wowee {
namespace game {

/**
 * Object type IDs for WoW 3.3.5a
 */
enum class ObjectType : uint8_t {
    OBJECT = 0,
    ITEM = 1,
    CONTAINER = 2,
    UNIT = 3,
    PLAYER = 4,
    GAMEOBJECT = 5,
    DYNAMICOBJECT = 6,
    CORPSE = 7
};

/**
 * Object type masks for update packets
 */
enum class TypeMask : uint16_t {
    OBJECT = 0x0001,
    ITEM = 0x0002,
    CONTAINER = 0x0004,
    UNIT = 0x0008,
    PLAYER = 0x0010,
    GAMEOBJECT = 0x0020,
    DYNAMICOBJECT = 0x0040,
    CORPSE = 0x0080
};

// UNIT_DYNAMIC_FLAGS. LOOTABLE, TAPPED and DEAD are the same in every
// supported expansion; 0x08 is not. CMaNGOS classic/TBC (SharedDefines.h
// UnitDynFlags) send 0x08 as ROOTED and send TAPPED only to a viewer who does
// not hold the tap (Object::BuildValuesUpdate clears it for the tapper), so
// there is no tapped-by-player or tapped-by-all bit. WotLK sends TAPPED with
// 0x08 TAPPED_BY_PLAYER for the tapper and 0x80 TAPPED_BY_ALL_THREAT_LIST.
// The DEAD bit is 0x20 everywhere; confusing it with 0x08 leaves pre-existing
// corpses standing after login.
inline constexpr uint32_t UNIT_DYNFLAG_LOOTABLE         = 0x00000001;
inline constexpr uint32_t UNIT_DYNFLAG_TAPPED           = 0x00000004;
inline constexpr uint32_t UNIT_DYNFLAG_ROOTED_LEGACY    = 0x00000008;  ///< classic/TBC
inline constexpr uint32_t UNIT_DYNFLAG_TAPPED_BY_PLAYER = 0x00000008;  ///< WotLK
inline constexpr uint32_t UNIT_DYNFLAG_DEAD             = 0x00000020;
inline constexpr uint32_t UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST = 0x00000080;  ///< WotLK

// GAMEOBJECT_DYN_FLAGS low half, per player (CMaNGOS SharedDefines.h
// GameObjectDynamicLowFlags, written by Object::BuildValuesUpdate).
inline constexpr uint32_t GO_DYNFLAG_LO_ACTIVATE = 0x01;
inline constexpr uint32_t GO_DYNFLAG_LO_SPARKLE  = 0x08;

/// The object glitters for this player: CMaNGOS sets SPARKLE (with ACTIVATE)
/// on a chest, goober, generic or spell focus a quest of the player's needs,
/// on every such object for a game master, and clears it again once no quest
/// needs it. WotLK keeps path progress in the high half.
inline constexpr bool gameObjectSparkles(uint32_t dynamicField) {
    return (dynamicField & GO_DYNFLAG_LO_SPARKLE) != 0;
}

/// UnitIsTapped: someone holds the tap. Pre-WotLK servers only send TAPPED
/// to viewers who do not hold it.
inline constexpr bool unitIsTapped(uint32_t dynamicFlags) {
    return (dynamicFlags & UNIT_DYNFLAG_TAPPED) != 0;
}
/// UnitIsTappedByPlayer. Pre-WotLK carries no bit for it: a tap the player
/// holds arrives as no TAPPED bit at all.
inline constexpr bool unitIsTappedByPlayer(uint32_t dynamicFlags, bool preWotlk) {
    return !preWotlk && (dynamicFlags & UNIT_DYNFLAG_TAPPED_BY_PLAYER) != 0;
}
inline constexpr bool unitIsTappedByAllThreatList(uint32_t dynamicFlags, bool preWotlk) {
    return !preWotlk && (dynamicFlags & UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST) != 0;
}
/// Grey health: tapped, and not by the player or for the whole threat list.
inline constexpr bool unitTappedByOther(uint32_t dynamicFlags, bool preWotlk) {
    return unitIsTapped(dynamicFlags) && !unitIsTappedByPlayer(dynamicFlags, preWotlk) &&
           !unitIsTappedByAllThreatList(dynamicFlags, preWotlk);
}

inline constexpr uint8_t  UNIT_STAND_STATE_DEAD = 7;
inline constexpr uint32_t EMOTE_STATE_DEAD      = 65;

/// CREATE updates may omit zero-valued health. In that case the dynamic corpse
/// flags are the authoritative indication that the unit must spawn dead.
inline bool isUnitCorpseState(uint32_t health, uint32_t maxHealth, uint32_t dynamicFlags) {
    return (maxHealth > 0 && health == 0) ||
           (dynamicFlags & (UNIT_DYNFLAG_DEAD | UNIT_DYNFLAG_LOOTABLE)) != 0;
}

/// True when the unit's server fields place it in a dead/corpse pose, including
/// Feign Death / Permanent Feign Death (29266) where health stays > 0 while
/// UNIT_DYNFLAG_DEAD is set, UNIT_STAND_STATE_DEAD (bytes1 & 0xFF == 7), and
/// EMOTE_STATE_DEAD (65).
inline bool isUnitDeadPose(uint32_t health, uint32_t maxHealth, uint32_t dynamicFlags,
                           uint8_t standState, uint32_t npcEmoteState = 0) {
    return isUnitCorpseState(health, maxHealth, dynamicFlags) ||
           standState == UNIT_STAND_STATE_DEAD ||
           npcEmoteState == EMOTE_STATE_DEAD;
}

/**
 * Update types for SMSG_UPDATE_OBJECT
 */
enum class UpdateType : uint8_t {
    VALUES = 0,              // Partial update (changed fields only)
    MOVEMENT = 1,            // Movement update
    CREATE_OBJECT = 2,       // Create new object (full data)
    CREATE_OBJECT2 = 3,      // Create new object (alternate format)
    OUT_OF_RANGE_OBJECTS = 4, // Objects left view range
    NEAR_OBJECTS = 5         // Objects entered view range
};

/// The update type's own name, for a log line.
///
/// Beside the enum rather than in the two parsers that print it - the WotLK one
/// and the vanilla one had an identical copy each, and they are the two files
/// where a value added to this list would need noticing.
inline const char* updateTypeName(UpdateType type) {
    switch (type) {
        case UpdateType::VALUES:               return "VALUES";
        case UpdateType::MOVEMENT:             return "MOVEMENT";
        case UpdateType::CREATE_OBJECT:        return "CREATE_OBJECT";
        case UpdateType::CREATE_OBJECT2:       return "CREATE_OBJECT2";
        case UpdateType::OUT_OF_RANGE_OBJECTS: return "OUT_OF_RANGE_OBJECTS";
        case UpdateType::NEAR_OBJECTS:         return "NEAR_OBJECTS";
    }
    return "UNKNOWN";
}

/**
 * Base entity class for all game objects
 */
class Entity {
public:
    Entity() = default;
    explicit Entity(uint64_t guid) : guid(guid) {}
    virtual ~Entity() = default;

    // GUID access
    [[nodiscard]] uint64_t getGuid() const { return guid; }
    void setGuid(uint64_t g) { guid = g; }

    // Position
    [[nodiscard]] float getX() const { return x; }
    [[nodiscard]] float getY() const { return y; }
    [[nodiscard]] float getZ() const { return z; }
    [[nodiscard]] float getOrientation() const { return orientation; }
    // Update orientation only, without disrupting an in-progress movement interpolation.
    void setOrientation(float o) { orientation = o; remoteFacing_ = {}; }

    void setPosition(float px, float py, float pz, float o) {
        x = px;
        y = py;
        z = pz;
        orientation = o;
        remoteFacing_ = {};
        remotePredictedMotion_ = false;
        isMoving_ = false; // Instant position set cancels interpolation
        usePathMode_ = false;
        serverSpline_ = false;
        hasServerSplinePosition_ = false;
        serverSplineFlags_ = 0;
        splineFacingTargetGuid_ = 0;
        splineFinalOrientation_.reset();
        splineArrivalTargetPending_ = false;
        activeSpline_.reset();
    }

    void markServerSplinePosition(uint32_t splineFlags = 0, uint64_t facingTargetGuid = 0) {
        hasServerSplinePosition_ = true;
        serverSplineFlags_ = splineFlags;
        splineFacingTargetGuid_ = facingTargetGuid;
    }

    // Multi-segment path movement (Catmull-Rom or piecewise-linear spline interpolation)
    void startMoveAlongPath(const std::vector<std::array<float, 3>>& path, float destO, float totalDuration,
                            bool smooth = true, bool followPathFacing = false, bool cyclic = false,
                            std::optional<float> finalOrientation = std::nullopt,
                            uint64_t facingTargetGuid = 0, bool enterCycle = false) {
        remotePredictedMotion_ = false;
        remoteAirborne_ = false;
        serverSpline_ = false;
        hasServerSplinePosition_ = false;
        serverSplineFlags_ = 0;
        splineFacingTargetGuid_ = 0;
        splineFinalOrientation_.reset();
        splineArrivalTargetPending_ = false;
        enterCycle_ = false;
        if (path.empty()) return;
        if (path.size() == 1 || totalDuration <= 0.0f) {
            setPosition(path.back()[0], path.back()[1], path.back()[2],
                        finalOrientation.value_or(destO));
            splineFacingTargetGuid_ = facingTargetGuid;
            splineArrivalTargetPending_ = facingTargetGuid != 0;
            return;
        }
        if (!buildPathSpline(path, totalDuration)) {
            setPosition(path.back()[0], path.back()[1], path.back()[2],
                        finalOrientation.value_or(destO));
            splineFacingTargetGuid_ = facingTargetGuid;
            splineArrivalTargetPending_ = facingTargetGuid != 0;
            return;
        }
        cyclicPath_ = cyclic;
        enterCycle_ = cyclic && enterCycle && path.size() >= 4;
        smoothPath_ = smooth;
        followPathFacing_ = followPathFacing;
        splineFinalOrientation_ = finalOrientation;
        splineFacingTargetGuid_ = facingTargetGuid;

        x = path.front()[0];
        y = path.front()[1];
        z = path.front()[2];
        moveEndX_ = path.back()[0]; moveEndY_ = path.back()[1]; moveEndZ_ = path.back()[2];
        moveDuration_ = totalDuration;
        moveElapsed_ = 0.0f;
        orientation = destO;
        remoteFacing_ = {};
        isMoving_ = true;
        usePathMode_ = true;
        velX_ = 0.0f;
        velY_ = 0.0f;
        velZ_ = 0.0f;
    }

    // Server-authored SMSG_MONSTER_MOVE spline (0 or more intermediate waypoints).
    // Stops exactly at path.back() with zero dead-reckoning overrun and retains
    // hasServerSplinePosition_ so stationary ground floor projection persists
    // (e.g. CMaNGOS MoveCharge adds +1.0 yd to spline Z).
    // Cyclic splines loop continuously; Enter_Cycle drops the pre-loop entry
    // vertex after the first full lap (matching CMaNGOS MoveSpline::_updateState).
    void startServerSpline(const std::vector<std::array<float, 3>>& path, float destO,
                           float totalDuration, bool smooth, bool followPathFacing,
                           uint32_t splineFlags, uint64_t facingTargetGuid = 0,
                           std::optional<float> finalOrientation = std::nullopt,
                           bool cyclic = false, bool enterCycle = false) {
        startMoveAlongPath(path, destO, totalDuration, smooth, followPathFacing,
                           cyclic, finalOrientation, facingTargetGuid, enterCycle);
        serverSpline_ = isMoving_;
        hasServerSplinePosition_ = true;
        serverSplineFlags_ = splineFlags;
    }

    // Movement interpolation (syncs entity position with renderer during movement)
    void startMoveTo(float destX, float destY, float destZ, float destO, float durationSec) {
        remotePredictedMotion_ = false;
        usePathMode_ = false;
        serverSpline_ = false;
        hasServerSplinePosition_ = false;
        serverSplineFlags_ = 0;
        splineFacingTargetGuid_ = 0;
        splineFinalOrientation_.reset();
        splineArrivalTargetPending_ = false;
        activeSpline_.reset();
        if (durationSec <= 0.0f) {
            setPosition(destX, destY, destZ, destO);
            return;
        }
        // Movement heartbeats and repeated spline destinations can carry a
        // positive duration without any actual displacement. Treat these as
        // authoritative stops; otherwise isActivelyMoving() drives Run/Walk
        // while the model has nowhere to move. This applies equally to nearby
        // players and creatures.
        const float remainingX = destX - x;
        const float remainingY = destY - y;
        const float remainingZ = destZ - z;
        constexpr float kNoOpMoveDistanceSq = 0.02f * 0.02f;
        if (remainingX * remainingX + remainingY * remainingY +
                remainingZ * remainingZ <= kNoOpMoveDistanceSq) {
            setPosition(destX, destY, destZ, destO);
            return;
        }
        // If we're in the dead-reckoning overrun phase, snap x/y/z back to the
        // destination before using them as the new start.  The renderer was showing
        // the entity at moveEnd (via getLatest) during overrun, so the new
        // interpolation must start there to avoid a visible teleport.
        if (isMoving_ && moveElapsed_ >= moveDuration_) {
            x = moveEndX_;
            y = moveEndY_;
            z = moveEndZ_;
        }
        // Derive velocity from the displacement this packet implies.
        // Use the previous destination (not current lerped pos) as the "from" so
        // variable network timing doesn't inflate/shrink the implied speed.
        float fromX = isMoving_ ? moveEndX_ : x;
        float fromY = isMoving_ ? moveEndY_ : y;
        float fromZ = isMoving_ ? moveEndZ_ : z;
        float impliedVX = (destX - fromX) / durationSec;
        float impliedVY = (destY - fromY) / durationSec;
        float impliedVZ = (destZ - fromZ) / durationSec;
        // Exponential moving average on velocity - 65% new sample, 35% previous.
        // Smooths out jitter from irregular server update intervals (~200-600ms)
        // without introducing visible lag on direction changes.
        const float alpha = 0.65f;
        velX_ = alpha * impliedVX + (1.0f - alpha) * velX_;
        velY_ = alpha * impliedVY + (1.0f - alpha) * velY_;
        velZ_ = alpha * impliedVZ + (1.0f - alpha) * velZ_;

        moveStartX_ = x; moveStartY_ = y; moveStartZ_ = z;
        moveEndX_ = destX; moveEndY_ = destY; moveEndZ_ = destZ;
        moveDuration_ = durationSec;
        locomotionSpeed_ = std::sqrt((destX - x) * (destX - x) +
            (destY - y) * (destY - y) + (destZ - z) * (destZ - z)) / durationSec;
        moveElapsed_ = 0.0f;
        orientation = destO;
        remoteFacing_ = {};
        isMoving_ = true;
    }

    // Player movement packets are observations of ongoing motion, not spline
    // destinations. Integrate their velocity immediately; blend only positional
    // correction so heartbeat boundaries do not stop or restart locomotion.
    void updateRemoteGroundMotion(const glm::vec3& position, float yaw,
                                  const glm::vec2& velocity) {
        remotePredictedMotion_ = true;
        remoteAirborne_ = false;
        usePathMode_ = false;
        serverSpline_ = false;
        hasServerSplinePosition_ = false;
        serverSplineFlags_ = 0;
        splineFacingTargetGuid_ = 0;
        splineFinalOrientation_.reset();
        splineArrivalTargetPending_ = false;
        activeSpline_.reset();
        remoteVelocity_ = velocity;
        remoteCorrection_ = position - glm::vec3(x, y, z);
        remoteCorrectionRemaining_ = 0.1f;
        remotePacketAge_ = 0.0f;
        moveEndX_ = position.x; moveEndY_ = position.y; moveEndZ_ = position.z;
        orientation = yaw;
        remoteFacing_ = {};
        locomotionSpeed_ = glm::length(velocity);
        isMoving_ = true;
    }

    // Facing of another player between its movement packets. The packet's
    // facing is the target. While its TURN_LEFT/TURN_RIGHT flag is set the
    // target keeps turning at turnSpeed (rad/s, positive = left), as build
    // 8606 turns a standing player through a keyboard turn although no packet
    // comes between MSG_MOVE_START_TURN and MSG_MOVE_STOP_TURN; ground
    // movement turns with it. Other facing changes (mouse-look sends
    // MSG_MOVE_SET_FACING about every 100 ms) are drawn from the facing shown
    // to the target over blendSec, not as steps. Call after the position
    // update that read the same packet; shownFacing is the facing before it.
    void updateRemoteFacing(float shownFacing, float targetFacing, float turnSpeed, float blendSec) {
        assert(std::isfinite(shownFacing) && std::isfinite(targetFacing) && std::isfinite(turnSpeed));
        assert(blendSec >= 0.0f);
        orientation = blendSec > 0.0f ? shownFacing : targetFacing;
        remoteFacing_ = {targetFacing, turnSpeed, blendSec, true};
    }

    [[nodiscard]] float getRemoteTurnSpeed() const { return remoteFacing_.turnSpeed; }

    // Turn rate from the server's movement block, updated by
    // MSG_MOVE_SET_TURN_RATE and SMSG_SPLINE_SET_TURN_RATE; rad/s. Zero means
    // not initialized.
    float turnRate = 0.0f;

    static constexpr float remoteGravity = 19.29110527038574f;
    static constexpr float remoteTerminalVelocity = 60.148003f;

    void updateRemoteAirMotion(const glm::vec3& position, float yaw,
                               const glm::vec3& velocity, bool slowFall = false) {
        updateRemoteGroundMotion(position, yaw, glm::vec2(velocity));
        remoteAirborne_ = true;
        remoteVerticalVelocity_ = velocity.z;
        remoteTerminalSpeed_ = slowFall ? 7.0f : remoteTerminalVelocity;
    }

    [[nodiscard]] bool isRemoteAirborne() const {
        return remotePredictedMotion_ && remoteAirborne_;
    }

    // Ground speeds come from the server's movement block, including mounts
    // and movement-speed auras. Zero means not initialized, never a guessed speed.
    float groundWalkSpeed = 0.0f;
    float groundRunSpeed = 0.0f;
    float groundBackSpeed = 0.0f;

    void updateMovement(float deltaTime) {
        const float turned = advanceRemoteFacing(deltaTime);
        if (!isMoving_) return;
        if (remotePredictedMotion_) {
            // Bound extrapolation if the connection stops delivering packets.
            const float motionDt = std::min(deltaTime, std::max(0.0f, 2.0f - remotePacketAge_));
            glm::vec2 displacement = remoteVelocity_ * motionDt;
            if (turned != 0.0f && !remoteAirborne_) {
                // On the ground the movement turns with the facing: move
                // along the arc (its chord, turned by half the arc), then
                // turn the velocity.
                const float half = 0.5f * turned;
                displacement = rotateServerTurn(displacement, half) * (std::sin(half) / half);
                remoteVelocity_ = rotateServerTurn(remoteVelocity_, turned);
            }
            x += displacement.x;
            y += displacement.y;
            if (remoteAirborne_) {
                remoteVerticalVelocity_ = std::max(remoteVerticalVelocity_, -remoteTerminalSpeed_);
                const float accelerationTime = std::min(motionDt,
                    (remoteVerticalVelocity_ + remoteTerminalSpeed_) / remoteGravity);
                z += remoteVerticalVelocity_ * accelerationTime -
                     0.5f * remoteGravity * accelerationTime * accelerationTime -
                     remoteTerminalSpeed_ * (motionDt - accelerationTime);
                remoteVerticalVelocity_ = std::max(-remoteTerminalSpeed_,
                    remoteVerticalVelocity_ - remoteGravity * motionDt);
            }
            remotePacketAge_ += deltaTime;
            if (remoteCorrectionRemaining_ > 0.0f) {
                const float fraction = std::min(1.0f, deltaTime / remoteCorrectionRemaining_);
                const glm::vec3 correction = remoteCorrection_ * fraction;
                x += correction.x; y += correction.y; z += correction.z;
                remoteCorrection_ -= correction;
                remoteCorrectionRemaining_ = std::max(0.0f, remoteCorrectionRemaining_ - deltaTime);
            }
            return;
        }
        moveElapsed_ += deltaTime;
        if (usePathMode_ && cyclicPath_ && moveDuration_ > 0.0f) {
            if (moveElapsed_ >= moveDuration_) {
                moveElapsed_ = std::fmod(moveElapsed_, moveDuration_);
                if (enterCycle_ && activeSpline_ && activeSpline_->keyCount() >= 4) {
                    // Drop the pre-cycle entry vertex (keys[0]) after the first full
                    // lap, preserving total duration (CMaNGOS MoveSpline::_updateState).
                    const auto& oldKeys = activeSpline_->keys();
                    std::vector<std::array<float, 3>> loopPath;
                    loopPath.reserve(oldKeys.size() - 1);
                    for (size_t i = 1; i < oldKeys.size(); ++i) {
                        const auto& p = oldKeys[i].position;
                        loopPath.push_back({p.x, p.y, p.z});
                    }
                    enterCycle_ = false;
                    if (buildPathSpline(loopPath, moveDuration_)) {
                        moveEndX_ = loopPath.back()[0];
                        moveEndY_ = loopPath.back()[1];
                        moveEndZ_ = loopPath.back()[2];
                    }
                }
            }
        }
        if (moveElapsed_ < moveDuration_) {
            if (usePathMode_ && activeSpline_) {
                // Catmull-Rom spline interpolation
                uint32_t pathTimeMs = static_cast<uint32_t>(moveElapsed_ * 1000.0f);
                if (pathTimeMs >= splineDurationMs_) pathTimeMs = splineDurationMs_ - 1;
                glm::vec3 pos;
                glm::vec3 tangent;
                if (smoothPath_) {
                    const auto sample = activeSpline_->evaluate(pathTimeMs);
                    pos = sample.position;
                    tangent = sample.tangent;
                } else {
                    const auto& keys = activeSpline_->keys();
                    auto end = std::upper_bound(keys.begin(), keys.end(), pathTimeMs,
                        [](uint32_t time, const math::SplineKey& key) { return time < key.timeMs; });
                    if (end == keys.begin()) end = keys.begin() + 1;
                    if (end == keys.end()) end = keys.end() - 1;
                    const auto& begin = *(end - 1);
                    const uint32_t segDur = end->timeMs > begin.timeMs ? (end->timeMs - begin.timeMs) : 1u;
                    const float fraction = static_cast<float>(pathTimeMs - begin.timeMs) /
                        static_cast<float>(segDur);
                    tangent = end->position - begin.position;
                    pos = begin.position + tangent * fraction;
                }
                if (followPathFacing_ && glm::dot(glm::vec2(tangent), glm::vec2(tangent)) > 0.000001f)
                    orientation = std::atan2(-tangent.y, tangent.x);
                x = pos.x;
                y = pos.y;
                z = pos.z;
            } else {
                // Single-segment linear interpolation
                float t = moveElapsed_ / moveDuration_;
                x = moveStartX_ + (moveEndX_ - moveStartX_) * t;
                y = moveStartY_ + (moveEndY_ - moveStartY_) * t;
                z = moveStartZ_ + (moveEndZ_ - moveStartZ_) * t;
            }
        } else if (usePathMode_ || serverSpline_) {
            // Authoritative spline path has reached its explicit endpoint: clamp
            // directly to destination with zero dead-reckoning overrun, and apply
            // any server-authored final facing (Final_Angle / Final_Point / Final_Target).
            x = moveEndX_;
            y = moveEndY_;
            z = moveEndZ_;
            if (splineFinalOrientation_) {
                orientation = *splineFinalOrientation_;
                splineFinalOrientation_.reset();
            }
            if (splineFacingTargetGuid_ != 0) {
                splineArrivalTargetPending_ = true;
            }
            velX_ = 0.0f;
            velY_ = 0.0f;
            velZ_ = 0.0f;
            isMoving_ = false;
            serverSpline_ = false;
        } else {
            // Past the interpolation window: dead-reckon at the smoothed velocity
            // rather than freezing in place. Cap to one extra interval so we don't
            // drift endlessly if the entity stops sending packets.
            float overrun = moveElapsed_ - moveDuration_;
            if (overrun < moveDuration_) {
                x = moveEndX_ + velX_ * overrun;
                y = moveEndY_ + velY_ * overrun;
                z = moveEndZ_ + velZ_ * overrun;
            } else {
                // Two intervals with no update - entity has probably stopped.
                x = moveEndX_; y = moveEndY_; z = moveEndZ_;
                velX_ = 0.0f; velY_ = 0.0f; velZ_ = 0.0f;
                isMoving_ = false;
            }
        }
    }

    [[nodiscard]] bool isServerSplineActive() const {
        return serverSpline_ && isActivelyMoving();
    }
    [[nodiscard]] bool hasServerSplinePosition() const { return hasServerSplinePosition_; }
    [[nodiscard]] uint32_t serverSplineFlags() const { return serverSplineFlags_; }
    [[nodiscard]] uint64_t splineFacingTargetGuid() const { return splineFacingTargetGuid_; }
    [[nodiscard]] uint64_t consumeSplineArrivalTargetGuid() {
        if (!splineArrivalTargetPending_) return 0;
        splineArrivalTargetPending_ = false;
        return splineFacingTargetGuid_;
    }

    [[nodiscard]] float getLocomotionSpeed() const {
        return isActivelyMoving() ? locomotionSpeed_ : 0.0f;
    }

    [[nodiscard]] bool isEntityMoving() const { return isMoving_; }

    /// True only during the active interpolation phase (before reaching destination).
    /// Unlike isEntityMoving(), this does NOT include the dead-reckoning overrun window,
    /// so animations (Run/Walk) should use this to avoid "running in place" after arrival.
    [[nodiscard]] bool isActivelyMoving() const {
        if (remotePredictedMotion_)
            return remotePacketAge_ < 2.0f && (remoteAirborne_ || glm::dot(remoteVelocity_, remoteVelocity_) > 0.0001f);
        return isMoving_ && moveElapsed_ < moveDuration_;
    }

    // Returns the latest server-authoritative position: destination if moving, current if not.
    // Unlike getX/Y/Z (which only update via updateMovement), this always reflects the
    // last known server position regardless of distance culling.
    [[nodiscard]] float getLatestX() const { return isMoving_ ? moveEndX_ : x; }
    [[nodiscard]] float getLatestY() const { return isMoving_ ? moveEndY_ : y; }
    [[nodiscard]] float getLatestZ() const { return isMoving_ ? moveEndZ_ : z; }

    // Object type
    [[nodiscard]] ObjectType getType() const { return type; }
    void setType(ObjectType t) { type = t; }

    /// True if this entity is a Unit or Player (both derive from Unit).
    [[nodiscard]] bool isUnit() const { return type == ObjectType::UNIT || type == ObjectType::PLAYER; }

    // Fields (for update values)
    void setField(uint16_t index, uint32_t value) {
        fields[index] = value;
    }

    [[nodiscard]] uint32_t getField(uint16_t index) const {
        auto it = fields.find(index);
        return (it != fields.end()) ? it->second : 0;
    }

    [[nodiscard]] bool hasField(uint16_t index) const {
        return fields.find(index) != fields.end();
    }

    [[nodiscard]] const FlatFieldMap& getFields() const {
        return fields;
    }

protected:
    uint64_t guid = 0;
    ObjectType type = ObjectType::OBJECT;

    // Position
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;

    // Update fields (dynamic values) - flat sorted vector. See FlatFieldMap docs.
    FlatFieldMap fields;

    // Movement interpolation state
    bool remotePredictedMotion_ = false;
    bool remoteAirborne_ = false;
    float remoteVerticalVelocity_ = 0.0f;
    float remoteTerminalSpeed_ = remoteTerminalVelocity;
    glm::vec2 remoteVelocity_{0.0f};
    glm::vec3 remoteCorrection_{0.0f};
    float remoteCorrectionRemaining_ = 0.0f;
    float remotePacketAge_ = 0.0f;
    bool isMoving_ = false;
    bool usePathMode_ = false;
    float moveStartX_ = 0, moveStartY_ = 0, moveStartZ_ = 0;
    float moveEndX_ = 0, moveEndY_ = 0, moveEndZ_ = 0;
    float locomotionSpeed_ = 0.0f; // Authoritative path length / duration, before render projection.
    float moveDuration_ = 0;
    float moveElapsed_ = 0;
    float velX_ = 0, velY_ = 0, velZ_ = 0;  // Smoothed velocity for dead reckoning
    struct RemoteFacing {
        float target = 0.0f;
        float turnSpeed = 0.0f;
        float blendRemaining = 0.0f;
        // Set by a packet: the next update's time partly passed before the
        // packet came, so it does not turn the facing. Turning over it ran
        // past MSG_MOVE_STOP_TURN's facing at low frame rates, and turning
        // back to it looked like a turn the other way.
        bool fresh = false;
    };
    RemoteFacing remoteFacing_;

    static float wrapAngle(float a) { return std::remainder(a, 6.28318530717958647692f); }

    // Canonical (x, y) is server (y, x): a counter-clockwise (left) server
    // turn by angle is clockwise in canonical x/y.
    static glm::vec2 rotateServerTurn(const glm::vec2& v, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        return glm::vec2(c * v.x + s * v.y, -s * v.x + c * v.y);
    }

    // Advances the remote facing one frame; returns the angle turned at the
    // turn rate (rad, positive = left).
    float advanceRemoteFacing(float deltaTime) {
        auto& f = remoteFacing_;
        const float turned = f.fresh ? 0.0f : f.turnSpeed * deltaTime;
        f.fresh = false;
        if (turned != 0.0f) {
            f.target = wrapAngle(f.target + turned);
            orientation = wrapAngle(orientation + turned);
        }
        if (f.blendRemaining > 0.0f) {
            const float fraction = std::min(1.0f, deltaTime / f.blendRemaining);
            orientation = wrapAngle(orientation + wrapAngle(f.target - orientation) * fraction);
            f.blendRemaining = std::max(0.0f, f.blendRemaining - deltaTime);
        }
        return turned;
    }

    bool buildPathSpline(const std::vector<std::array<float, 3>>& path, float totalDuration) {
        if (path.size() < 2 || totalDuration <= 0.0f) return false;
        std::vector<float> cumDist(path.size(), 0.0f);
        float totalDist = 0.0f;
        for (size_t i = 1; i < path.size(); i++) {
            float dx = path[i][0] - path[i - 1][0];
            float dy = path[i][1] - path[i - 1][1];
            float dz = path[i][2] - path[i - 1][2];
            totalDist += std::sqrt(dx * dx + dy * dy + dz * dz);
            cumDist[i] = totalDist;
        }
        if (totalDist < 0.001f) return false;
        uint32_t durationMs = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(totalDuration * 1000.0f)));
        const float invTotalDist = static_cast<float>(durationMs) / totalDist;
        std::vector<math::SplineKey> keys(path.size());
        for (size_t i = 0; i < path.size(); i++) {
            keys[i].timeMs = static_cast<uint32_t>(cumDist[i] * invTotalDist);
            keys[i].position = {path[i][0], path[i][1], path[i][2]};
        }
        keys.back().timeMs = durationMs;
        activeSpline_.emplace(std::move(keys), /*timeClosed=*/false);
        splineDurationMs_ = durationMs;
        locomotionSpeed_ = totalDist / totalDuration;
        return true;
    }

    // CatmullRom spline for multi-segment path movement (replaces linear pathPoints_/pathSegDists_)
    std::optional<math::CatmullRomSpline> activeSpline_;
    uint32_t splineDurationMs_ = 0;
    bool cyclicPath_ = false;
    bool enterCycle_ = false;
    bool smoothPath_ = true;
    bool followPathFacing_ = false;
    bool serverSpline_ = false;
    bool hasServerSplinePosition_ = false;
    uint32_t serverSplineFlags_ = 0;
    uint64_t splineFacingTargetGuid_ = 0;
    std::optional<float> splineFinalOrientation_;
    bool splineArrivalTargetPending_ = false;
};

/**
 * Unit entity (NPCs, creatures, players)
 */
class Unit : public Entity {
public:
    Unit() { type = ObjectType::UNIT; }
    explicit Unit(uint64_t guid) : Entity(guid) { type = ObjectType::UNIT; }

    // Name
    [[nodiscard]] const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }
    // A pet's own name (SMSG_PET_NAME_QUERY_RESPONSE), e.g. a warlock's imp
    // "Gragup": the creature template's "Imp" must not replace it.
    [[nodiscard]] bool hasPetName() const { return petNamed_; }
    void setPetName(const std::string& n) { name = n; petNamed_ = true; }
    // UNIT_FIELD_PET_NAME_TIMESTAMP last queried; a new value means a new name.
    uint32_t queriedPetNameTimestamp = 0;

    // Health
    [[nodiscard]] uint32_t getHealth() const { return health; }
    void setHealth(uint32_t h) { health = h; }

    [[nodiscard]] uint32_t getMaxHealth() const { return maxHealth; }
    void setMaxHealth(uint32_t h) { maxHealth = h; }

    // Power (mana/rage/energy) - indexed by power type (0-6)
    [[nodiscard]] uint32_t getPower() const { return powers[powerType < 7 ? powerType : 0]; }
    void setPower(uint32_t p) { powers[powerType < 7 ? powerType : 0] = p; }
    void setPowerByType(uint8_t type, uint32_t p) { if (type < 7) powers[type] = p; }

    [[nodiscard]] uint32_t getMaxPower() const { return maxPowers[powerType < 7 ? powerType : 0]; }
    void setMaxPower(uint32_t p) { maxPowers[powerType < 7 ? powerType : 0] = p; }
    void setMaxPowerByType(uint8_t type, uint32_t p) { if (type < 7) maxPowers[type] = p; }

    [[nodiscard]] uint32_t getPowerByType(uint8_t type) const { return type < 7 ? powers[type] : 0; }
    [[nodiscard]] uint32_t getMaxPowerByType(uint8_t type) const { return type < 7 ? maxPowers[type] : 0; }

    [[nodiscard]] uint8_t getPowerType() const { return powerType; }
    void setPowerType(uint8_t t) { powerType = t; }

    [[nodiscard]] uint32_t getAuraState() const { return auraState; }
    void setAuraState(uint32_t state) { auraState = state; }

    // Level
    [[nodiscard]] uint32_t getLevel() const { return level; }
    void setLevel(uint32_t l) { level = l; }

    // Entry ID (creature template entry)
    [[nodiscard]] uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    // Display ID (model display)
    [[nodiscard]] uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }
    /// How far this unit's edge is from its centre, and how far past that it
    /// can reach. Range between two units is measured edge to edge, so both
    /// come off the centre distance - see UNIT_FIELD_COMBATREACH.
    [[nodiscard]] float getCombatReach() const { return combatReach; }
    void setCombatReach(float r) { combatReach = r; }
    [[nodiscard]] float getBoundingRadius() const { return boundingRadius; }
    void setBoundingRadius(float r) { boundingRadius = r; }

    // Mount display ID (UNIT_FIELD_MOUNTDISPLAYID, index 69)
    [[nodiscard]] uint32_t getMountDisplayId() const { return mountDisplayId; }
    void setMountDisplayId(uint32_t id) { mountDisplayId = id; }

    // Unit flags (UNIT_FIELD_FLAGS, index 59)
    [[nodiscard]] uint32_t getUnitFlags() const { return unitFlags; }
    void setUnitFlags(uint32_t f) { unitFlags = f; }

    // Client presentation flags (byte 2 of UNIT_FIELD_BYTES_1).
    [[nodiscard]] uint8_t getVisibilityFlags() const { return visibilityFlags; }
    void setVisibilityFlags(uint8_t f) { visibilityFlags = f; }
    [[nodiscard]] bool hasCreepVisibility() const { return (visibilityFlags & UNIT_VIS_FLAG_CREEP) != 0; }
    void clearCreepVisibility() {
        visibilityFlags &= static_cast<uint8_t>(~UNIT_VIS_FLAG_CREEP);
    }

    // Victim of this unit's melee attack: set by SMSG_ATTACKSTART, cleared by
    // SMSG_ATTACKSTOP (CMaNGOS Unit::SendMeleeAttackStart/Stop).
    [[nodiscard]] uint64_t getMeleeVictimGuid() const { return meleeVictimGuid; }
    void setMeleeVictimGuid(uint64_t guid) { meleeVictimGuid = guid; }

    // Dynamic flags (UNIT_DYNAMIC_FLAGS, index 147)
    [[nodiscard]] uint32_t getDynamicFlags() const { return dynamicFlags; }
    void setDynamicFlags(uint32_t f) { dynamicFlags = f; }

    // NPC flags (UNIT_NPC_FLAGS, index 82)
    [[nodiscard]] uint32_t getNpcFlags() const { return npcFlags; }
    void setNpcFlags(uint32_t f) { npcFlags = f; }

    // NPC emote state (UNIT_NPC_EMOTESTATE) - persistent looping animation for NPCs
    [[nodiscard]] uint32_t getNpcEmoteState() const { return npcEmoteState; }
    void setNpcEmoteState(uint32_t e) { npcEmoteState = e; }

    // Returns true if NPC has interaction flags (gossip/vendor/quest/trainer)
    [[nodiscard]] bool isInteractable() const { return npcFlags != 0; }

    // Faction-based hostility
    [[nodiscard]] uint32_t getFactionTemplate() const { return factionTemplate; }
    void setFactionTemplate(uint32_t f) { factionTemplate = f; }
    [[nodiscard]] bool isHostile() const { return hostile; }
    void setHostile(bool h) { hostile = h; }

    // True when created via SMSG_UPDATE_OBJECT UPDATETYPE_CREATE_OBJECT2
    // (newly spawned/summoned in-world) rather than CREATE_OBJECT (existing unit entering view).
    [[nodiscard]] bool isNewlySpawned() const { return newlySpawned_; }
    void setNewlySpawned(bool v) { newlySpawned_ = v; }

protected:
    std::string name;
    bool petNamed_ = false;
    uint32_t health = 0;
    uint32_t maxHealth = 0;
    uint32_t powers[7] = {};     // Indexed by power type (0=mana,1=rage,2=focus,3=energy,4=happiness,5=runes,6=runic)
    uint32_t maxPowers[7] = {};  // Max values per power type
    uint8_t powerType = 0;       // Active power type
    uint32_t auraState = 0;      // UNIT_FIELD_AURASTATE reactive opportunity mask
    uint32_t level = 1;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    // Zero until the server says otherwise, which is the right default: it
    // makes an edge-to-edge distance fall back to centre to centre rather
    // than inventing reach for a unit whose size has not arrived.
    float combatReach = 0.0f;
    float boundingRadius = 0.0f;
    uint32_t mountDisplayId = 0;
    uint32_t unitFlags = 0;
    uint8_t visibilityFlags = 0;
    uint64_t meleeVictimGuid = 0;
    uint32_t dynamicFlags = 0;
    uint32_t npcFlags = 0;
    uint32_t npcEmoteState = 0;
    uint32_t factionTemplate = 0;
    bool hostile = false;
    bool newlySpawned_ = false;
};

/**
 * Player entity
 * Name is inherited from Unit - do NOT redeclare it here or the
 * shadowed field will diverge from Unit::name, causing nameplates
 * and other Unit*-based lookups to read an empty string.
 */
class Player : public Unit {
public:
    Player() { type = ObjectType::PLAYER; }
    explicit Player(uint64_t guid) : Unit(guid) { type = ObjectType::PLAYER; }
};

/// Builds a Z-only world-space GameObject quaternion from canonical yaw.
/// Server/render space uses serverYaw = canonicalYaw + pi/2 around +Z.
inline glm::quat gameObjectRotationFromCanonicalYaw(float canonicalYaw) {
    const float serverYaw = core::coords::canonicalToServerYaw(canonicalYaw);
    return glm::quat(std::cos(serverYaw * 0.5f), 0.0f, 0.0f, std::sin(serverYaw * 0.5f));
}

/// Unpacks WotLK UPDATEFLAG_ROTATION (0x0200) 64-bit packed quaternion.
/// Layout: x = bits [0..21] / 2^21 (22-bit signed), y = bits [22..42] / 2^20
/// (21-bit signed), z = bits [43..63] / 2^20 (21-bit signed),
/// w = sqrt(max(0, 1 - x^2 - y^2 - z^2)).
inline glm::quat unpackGameObjectRotation(int64_t packed) {
    const uint64_t u = static_cast<uint64_t>(packed);
    auto signExtend = [](uint64_t value, int bits) -> int64_t {
        const uint64_t mask = uint64_t(1) << (bits - 1);
        return static_cast<int64_t>(((value & ((uint64_t(1) << bits) - 1)) ^ mask) - mask);
    };
    const float x = static_cast<float>(signExtend(u, 22)) / 2097152.0f;
    const float y = static_cast<float>(signExtend(u >> 22, 21)) / 1048576.0f;
    const float z = static_cast<float>(signExtend(u >> 43, 21)) / 1048576.0f;
    const float wSq = std::max(0.0f, 1.0f - (x * x + y * y + z * z));
    const glm::quat q(std::sqrt(wSq), x, y, z);
    const float lenSq = glm::dot(q, q);
    return (lenSq > 1e-6f && std::isfinite(lenSq)) ? glm::normalize(q)
                                                 : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

/**
 * GameObject entity (doors, chests, etc.)
 */
class GameObject : public Entity {
public:
    GameObject() { type = ObjectType::GAMEOBJECT; }
    explicit GameObject(uint64_t guid) : Entity(guid) { type = ObjectType::GAMEOBJECT; }

    [[nodiscard]] const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    [[nodiscard]] uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    [[nodiscard]] uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }

    /// Full 3D world-space orientation quaternion (x, y, z, w in server/render
    /// coordinates) from GAMEOBJECT_ROTATION / GAMEOBJECT_PARENTROTATION /
    /// UPDATEFLAG_ROTATION.
    [[nodiscard]] bool hasRotation() const { return hasRotation_; }
    [[nodiscard]] const glm::quat& getRotation() const { return rotation_; }
    void setRotation(const glm::quat& q) {
        [[maybe_unused]] const float lenSq = glm::dot(q, q);
        assert(std::isfinite(lenSq) && lenSq > 1e-6f);
        rotation_ = glm::normalize(q);
        hasRotation_ = true;
    }

    /// Returns the explicit 3D quaternion when set, otherwise the Z-only
    /// quaternion derived from canonical orientation.
    [[nodiscard]] glm::quat effectiveRotation() const {
        return hasRotation_ ? rotation_ : gameObjectRotationFromCanonicalYaw(orientation);
    }

protected:
    std::string name;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    glm::quat rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    bool hasRotation_ = false;
};

/// What to call this entity on screen, whatever kind it turns out to be.
///
/// The name lives on Unit and on GameObject and not on Entity, so anything
/// wanting to write one down has to ask which kind it is holding first. Five
/// places did that for themselves - the four files GameScreen was split into
/// and the chat commands - in identical copies, and two of the four never
/// called their own.
///
/// "Unknown" rather than an empty string, because every caller is putting this
/// in a sentence: an entity whose name has not arrived yet reads as a gap in
/// the line otherwise, with nothing to say a name was expected.
inline std::string entityDisplayName(const std::shared_ptr<Entity>& entity) {
    if (!entity) return "Unknown";
    if (entity->getType() == ObjectType::PLAYER) {
        auto player = std::static_pointer_cast<Player>(entity);
        if (!player->getName().empty()) return player->getName();
    } else if (entity->getType() == ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (!unit->getName().empty()) return unit->getName();
    } else if (entity->getType() == ObjectType::GAMEOBJECT) {
        auto go = std::static_pointer_cast<GameObject>(entity);
        if (!go->getName().empty()) return go->getName();
    }
    return "Unknown";
}

/**
 * Entity manager for tracking all entities in view
 */
class EntityManager {
public:
    // Add entity
    void addEntity(uint64_t guid, std::shared_ptr<Entity> entity);

    // Remove entity
    void removeEntity(uint64_t guid);

    // Get entity
    std::shared_ptr<Entity> getEntity(uint64_t guid) const;

    // Check if entity exists
    bool hasEntity(uint64_t guid) const;

    // Main-thread spatial query. The cell index is refreshed at most four times
    // per second, avoiding a full entity-map distance scan every rendered frame.
    std::vector<std::shared_ptr<Entity>> getEntitiesNear(float x, float y, float radius) const;
    void getEntitiesNear(float x, float y, float radius,
                         std::vector<std::shared_ptr<Entity>>& out) const;

    // Get all entities. MAIN-THREAD-ONLY: mutations happen via dispatchQueuedPackets()
    // on the main thread, and this reference is not lock-protected. Callers on any
    // other thread (e.g. the headless HTTP API thread) must use snapshotEntities()
    // instead, which is safe to call from anywhere.
    const std::unordered_map<uint64_t, std::shared_ptr<Entity>>& getEntities() const {
        return entities;
    }

    // Thread-safe copy of all tracked entities, safe to call from any thread.
    std::vector<std::shared_ptr<Entity>> snapshotEntities() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<Entity>> snapshot;
        snapshot.reserve(entities.size());
        for (const auto& [guid, entity] : entities) {
            snapshot.push_back(entity);
        }
        return snapshot;
    }

    // Clear all entities
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entities.clear();
        spatialCells_.clear();
        spatialDirty_ = true;
    }

    // Get entity count
    size_t getEntityCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entities.size();
    }

private:
    // MAIN-THREAD-ONLY for getEntities(): all entity map mutations happen via
    // dispatchQueuedPackets() on the main thread. mutex_ guards addEntity/removeEntity/
    // getEntity/hasEntity/snapshotEntities so those are safe to call cross-thread (the
    // headless HTTP API thread reads player HP and nearby entities this way); it is not
    // taken by the unlocked getEntities() reference accessor above, which remains
    // main-thread-only.
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Entity>> entities;
    static constexpr float kSpatialCellSize = 64.0f;
    mutable std::unordered_map<int64_t, std::vector<std::shared_ptr<Entity>>> spatialCells_;
    mutable std::chrono::steady_clock::time_point lastSpatialRebuild_{};
    mutable bool spatialDirty_ = true;
};

/// Who a unit has selected, from the two halves the wire splits the guid into.
///
/// UNIT_FIELD_TARGET arrives as a low and a high update field, and reading it
/// was written out separately in six files - the combat handler, two unit
/// APIs, the target frames, the nameplates and a slash command. Six copies of
/// a two-field read is how one of them comes to be missing its high half, and
/// a guid missing its high half matches nothing on a server that uses one.
inline uint64_t unitTargetGuid(const Entity& entity) {
    const auto& fields = entity.getFields();
    auto lo = fields.find(fieldIndex(UF::UNIT_FIELD_TARGET_LO));
    if (lo == fields.end()) return 0;
    uint64_t guid = lo->second;
    auto hi = fields.find(fieldIndex(UF::UNIT_FIELD_TARGET_HI));
    if (hi != fields.end()) guid |= (static_cast<uint64_t>(hi->second) << 32);
    return guid;
}

inline uint64_t unitTargetGuid(const std::shared_ptr<Entity>& entity) {
    return entity ? unitTargetGuid(*entity) : 0;
}

/// Native 2.4.3 turns a unit in place (toward its UNIT_FIELD_TARGET, or toward
/// the player while a gossip/quest panel is open) only while its stand state
/// (UNIT_FIELD_BYTES_1 byte 0) is 0 and it is not UNIT_FLAG_STUNNED. Measured
/// on Marshal Dughan: sitting, sleeping, dead, kneeling, chair and submerged
/// states and the stun flag keep the server heading.
inline bool unitTurnsInPlace(uint8_t standState, uint32_t unitFlags) {
    return standState == 0 && (unitFlags & UNIT_FLAG_STUNNED) == 0;
}

// CMaNGOS Unit::Attack sets this field because stationary creatures turn to
// their selected target client-side. Player selections never control facing.
inline std::optional<float> creatureTargetFacing(const Entity& entity,
                                                const EntityManager& entities) {
    if (entity.getType() != ObjectType::UNIT || entity.isActivelyMoving() ||
        static_cast<const Unit&>(entity).getHealth() == 0) return std::nullopt;
    if (!unitTurnsInPlace(static_cast<uint8_t>(entity.getField(fieldIndex(UF::UNIT_FIELD_BYTES_1))),
                          static_cast<const Unit&>(entity).getUnitFlags())) return std::nullopt;
    // A channel's object first: build 8606 shows a Tempest-Smith channelling
    // Golem Repair (34946) facing its Crystalcore Sentinel when the observer
    // comes into view mid-channel, its spawn heading and UNIT_FIELD_TARGET
    // notwithstanding (_experiments/20261118T122247Z.eye_channel_beam, f2).
    const uint64_t channelObject =
        static_cast<uint64_t>(entity.getField(fieldIndex(UF::UNIT_FIELD_CHANNEL_OBJECT_LO))) |
        (static_cast<uint64_t>(entity.getField(fieldIndex(UF::UNIT_FIELD_CHANNEL_OBJECT_HI))) << 32);
    const bool channelling = channelObject != 0 && channelObject != entity.getGuid() &&
                             entity.getField(fieldIndex(UF::UNIT_CHANNEL_SPELL)) != 0;
    const auto target = entities.getEntity(channelling ? channelObject : unitTargetGuid(entity));
    if (!target) return std::nullopt;
    const float dx = target->getX() - entity.getX();
    const float dy = target->getY() - entity.getY();
    if (dx * dx + dy * dy < 0.0001f) return std::nullopt;
    return std::atan2(-dy, dx);
}

/// Who summoned this unit, or 0 for a unit nobody did.
///
/// Answers 0 on an expansion whose table has no entry for the field, which is
/// the same answer as "nobody summoned it" on purpose: the callers use it to
/// decide whether a unit is somebody's pet, and not knowing has to read as no.
inline uint64_t unitSummonedByGuid(const Entity& entity) {
    const auto& fields = entity.getFields();
    auto lo = fields.find(fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_LO));
    if (lo == fields.end()) return 0;
    uint64_t guid = lo->second;
    auto hi = fields.find(fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_HI));
    if (hi != fields.end()) guid |= (static_cast<uint64_t>(hi->second) << 32);
    return guid;
}

inline uint64_t unitCreatedByGuid(const Entity& entity) {
    return static_cast<uint64_t>(entity.getField(fieldIndex(UF::UNIT_FIELD_CREATEDBY_LO))) |
           (static_cast<uint64_t>(entity.getField(fieldIndex(UF::UNIT_FIELD_CREATEDBY_HI))) << 32);
}

inline uint64_t unitCharmedByGuid(const Entity& entity) {
    const uint16_t loIdx = fieldIndex(UF::UNIT_FIELD_CHARMEDBY);
    if (loIdx == 0xFFFF) return 0;
    return static_cast<uint64_t>(entity.getField(loIdx)) |
           (static_cast<uint64_t>(entity.getField(static_cast<uint16_t>(loIdx + 1))) << 32);
}

} // namespace game
} // namespace wowee
