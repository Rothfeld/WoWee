#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {

// Native TBC 2.4.3 standing turn-in-place body lag: when turning with the
// keyboard (or mouse-look while stationary), the camera and logical facing
// rotate immediately (~180 deg/s) while the rendered body trails behind (up to
// ~55-60 deg at steady state) and smoothly catches up over ~0.5-0.6s after
// release while ShuffleLeft/ShuffleRight keeps stepping.
constexpr float kMaxTurnBodyLagDeg = 60.0f;
constexpr float kStandingTurnBodyFollowRate = 3.2f;   // 180 / 3.2 = 56.25 deg steady-state lag
constexpr float kMovingTurnBodyFollowRate = 18.0f;    // rapid catch-up once movement starts
constexpr float kTurnBodyLagSnapDeg = 0.25f;
constexpr float kTurnBodyCatchupShuffleThresholdDeg = 2.0f;

// Advances the rendered body yaw offset (visualYaw - logicalYaw, in degrees)
// for a standing character turning in place.
//   logicalDeltaDeg: wrapped change in logical characterYaw this frame (+CCW/left, -CW/right).
//   accumulateLag:   true while standing still and eligible for turn-in-place lag.
inline float stepTurnBodyLagDeg(float currentLagDeg, float logicalDeltaDeg,
                                float dt, bool accumulateLag) {
    if (!std::isfinite(currentLagDeg)) currentLagDeg = 0.0f;
    if (!std::isfinite(logicalDeltaDeg)) logicalDeltaDeg = 0.0f;
    if (!std::isfinite(dt) || dt <= 0.0f) {
        return std::clamp(currentLagDeg, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg);
    }
    float lag = currentLagDeg;
    if (accumulateLag) {
        // Logical facing rotated by +logicalDeltaDeg while the body has not
        // yet moved, so visualYaw - logicalYaw shifts by -logicalDeltaDeg.
        lag = std::clamp(lag - logicalDeltaDeg, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg);
    }
    const float rate = accumulateLag ? kStandingTurnBodyFollowRate : kMovingTurnBodyFollowRate;
    lag *= std::exp(-rate * dt);
    if (std::abs(lag) < kTurnBodyLagSnapDeg) return 0.0f;
    return std::clamp(lag, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg);
}

// Direction the visual body is turning to catch up to logical facing:
//   +1 = turning left (CCW, ShuffleLeft), -1 = turning right (CW, ShuffleRight), 0 = aligned.
inline int turnBodyCatchupDirection(float lagDeg) {
    if (!std::isfinite(lagDeg)) return 0;
    if (lagDeg < -kTurnBodyCatchupShuffleThresholdDeg) return 1;
    if (lagDeg > kTurnBodyCatchupShuffleThresholdDeg) return -1;
    return 0;
}

// Feet of a standing unit turning in place: ShuffleLeft (11) toward a left
// turn, ShuffleRight (12) toward a right one. A shuffle cycle in which the
// unit turned is followed by another; a cycle without a turn ends the
// shuffle, so the feet keep stepping for up to a cycle after the turn stops
// and a one-frame turn still steps once. Build 8606 does this for its own
// character (keyboard and mouse-look) and for other players it sees turn.
// Pure logic: the caller supplies the facing change and the playing animation.
class TurnShuffle {
public:
    // turnDirection: the unit's facing changed this frame, +1 left
    // (counter-clockwise from above), -1 right, 0 not.
    void start(int turnDirection) {
        active_ = true;
        left_ = turnDirection > 0;
        turnedThisCycle_ = true;
        seen_ = false;
        time_ = 0.0f;
    }

    // Advances a started shuffle by one frame. A reversed turn restarts it
    // toward the new side. Returns false when a cycle passed without a turn:
    // the shuffle is over.
    bool update(int turnDirection, bool haveAnimState, uint32_t currentAnimId, float currentAnimTime) {
        if (!active_) return false;
        if (turnDirection != 0 && (turnDirection > 0) != left_) {
            start(turnDirection);
            return true;
        }
        if (turnDirection != 0) turnedThisCycle_ = true;
        // Each cycle ends by asking whether the unit turned during it;
        // mouse-look turns arrive in steps, not every frame.
        if (cycleDone(haveAnimState, currentAnimId, currentAnimTime)) {
            if (!turnedThisCycle_) {
                active_ = false;
                return false;
            }
            turnedThisCycle_ = false;
        }
        return true;
    }

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] uint32_t animation() const { return left_ ? anim::SHUFFLE_LEFT : anim::SHUFFLE_RIGHT; }
    void reset() { *this = TurnShuffle{}; }

private:
    bool active_ = false;
    bool left_ = false;
    bool turnedThisCycle_ = false;
    bool seen_ = false;
    float time_ = 0.0f;

    // True when the looping shuffle has wrapped back to its start since it
    // was last asked about: a cycle has finished.
    bool cycleDone(bool haveAnimState, uint32_t currentAnimId, float currentAnimTime) {
        if (!haveAnimState) return true;
        if (currentAnimId != animation()) return seen_;
        const bool wrapped = seen_ && currentAnimTime < time_;
        seen_ = true;
        time_ = currentAnimTime;
        return wrapped;
    }
};

} // namespace rendering
} // namespace wowee
