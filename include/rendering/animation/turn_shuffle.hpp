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

// Standing turn-in-place lower-body / upper-body split (Native TBC 2.4.3.8606,
// measured across HumanMale, BloodElfMale, and TaurenMale):
//   - Mouse-look (RMB) standing turns leave the lower body (feet/pelvis)
//     planted in Stand with zero ShuffleLeft/ShuffleRight while
//     |footYaw - facingYaw| <= 60 deg (pi/3 rad), counter-rotating SpineLow
//     (keyBoneId 4) by -0.5 * footLag and Head (keyBoneId 6) by -0.5 * footLag
//     so the upper body and head twist immediately into the turn.
//   - Once RMB turning pushes |footYaw - facingYaw| past +-60 deg, the lower
//     body is pulled along at the +-60 deg boundary and plays ShuffleLeft /
//     ShuffleRight only on the frames where the feet step; stopping mouse
//     motion retains the twisted foot offset until movement or key-turning.
//   - Keyboard turns (A/D), combat auto-facing, and models without SpineLow
//     (quadruped shapeshifts) actively slew the lower body toward facingYaw
//     (3.2 s^-1) and shuffle until aligned.
constexpr float kStandingTurnSpineCounterScale = 0.5f;
constexpr float kStandingTurnHeadCounterScale  = 0.5f;
constexpr float kStandingTurnBoundaryEpsDeg    = 0.05f;

struct StandingTurnState {
    float footLagDeg = 0.0f;       // visualFootYaw - logicalFacingYaw, in [-60, +60] deg
    bool aligningToFacing = false; // latched by keyboard turn / auto-face until aligned
};

struct StandingTurnStepInput {
    float logicalDeltaDeg = 0.0f;    // wrapped change in characterYaw this frame (+CCW/left, -CW/right)
    float dt = 0.0f;
    bool groundedStationary = false; // !moving && grounded && !sitting
    bool keyTurning = false;         // A/D keyboard turn or combat auto-face active
    bool hasSpineLow = true;         // model has M2 keyBoneId 4 (SpineLow)
};

struct StandingTurnStepOutput {
    StandingTurnState state{};
    int shuffleDirection = 0;      // +1 = ShuffleLeft (CCW), -1 = ShuffleRight (CW), 0 = none
    float spineLowYawRad = 0.0f;   // extra Z yaw on keyBoneId 4 (SpineLow)
    float headYawRad = 0.0f;       // extra Z yaw on keyBoneId 6 (Head)
};

inline StandingTurnStepOutput stepStandingTurn(StandingTurnState current,
                                               const StandingTurnStepInput& in) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float lag = std::isfinite(current.footLagDeg)
        ? std::clamp(current.footLagDeg, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg)
        : 0.0f;
    const float delta = std::isfinite(in.logicalDeltaDeg) ? in.logicalDeltaDeg : 0.0f;

    auto makeOutput = [&](float outLag, bool aligning, int shuffleDir) -> StandingTurnStepOutput {
        const float clampedLag = std::clamp(outLag, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg);
        const float lagRad = clampedLag * kDegToRad;
        return StandingTurnStepOutput{
            .state = {clampedLag, aligning},
            .shuffleDirection = shuffleDir,
            .spineLowYawRad = in.hasSpineLow ? (-kStandingTurnSpineCounterScale * lagRad) : 0.0f,
            .headYawRad     = in.hasSpineLow ? (-kStandingTurnHeadCounterScale * lagRad)  : 0.0f,
        };
    };

    if (!std::isfinite(in.dt) || in.dt <= 0.0f) {
        return makeOutput(lag, current.aligningToFacing && in.hasSpineLow, 0);
    }

    if (!in.groundedStationary) {
        lag = stepTurnBodyLagDeg(lag, delta, in.dt, false);
        return makeOutput(lag, false, 0);
    }

    const bool activelyAligning =
        in.keyTurning || !in.hasSpineLow ||
        (current.aligningToFacing && std::abs(delta) <= 1e-4f);
    if (activelyAligning) {
        lag = stepTurnBodyLagDeg(lag, delta, in.dt, true);
        int shuffleDir = 0;
        if ((in.keyTurning || !in.hasSpineLow) && delta != 0.0f) {
            shuffleDir = delta > 0.0f ? 1 : -1;
        } else {
            shuffleDir = turnBodyCatchupDirection(lag);
        }
        const bool keepAligning = (in.keyTurning || current.aligningToFacing) &&
                                  (lag != 0.0f) && in.hasSpineLow;
        return makeOutput(lag, keepAligning, shuffleDir);
    }

    const float unclamped = lag - delta;
    int shuffleDir = 0;
    if (unclamped < -kMaxTurnBodyLagDeg - kStandingTurnBoundaryEpsDeg) {
        lag = -kMaxTurnBodyLagDeg;
        shuffleDir = 1;
    } else if (unclamped > kMaxTurnBodyLagDeg + kStandingTurnBoundaryEpsDeg) {
        lag = kMaxTurnBodyLagDeg;
        shuffleDir = -1;
    } else {
        lag = std::clamp(unclamped, -kMaxTurnBodyLagDeg, kMaxTurnBodyLagDeg);
    }
    return makeOutput(lag, false, shuffleDir);
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
