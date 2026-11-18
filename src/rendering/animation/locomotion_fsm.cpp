#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

// ── One-shot completion helper ─────────────────────────────────────────────────────────

bool LocomotionFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
}

// ── Turning in place ────────────────────────────────────────────────────────

void LocomotionFSM::startTurn(int direction) {
    state_ = State::TURN;
    turnLeft_ = direction > 0;
    turnedThisCycle_ = true;
    shuffleSeen_ = false;
    shuffleTime_ = 0.0f;
}

// True when the looping shuffle has wrapped back to its start since it was
// last asked about: a cycle has finished.
bool LocomotionFSM::shuffleCycleDone(const Input& in) {
    if (!in.haveAnimState) return true;
    const uint32_t expected = turnLeft_ ? anim::SHUFFLE_LEFT : anim::SHUFFLE_RIGHT;
    if (in.currentAnimId != expected) return shuffleSeen_;
    const bool wrapped = shuffleSeen_ && in.currentAnimTime < shuffleTime_;
    shuffleSeen_ = true;
    shuffleTime_ = in.currentAnimTime;
    return wrapped;
}

// ── Event handling ───────────────────────────────────────────────────────────

void LocomotionFSM::onEvent(AnimEvent event) {
    switch (event) {
        case AnimEvent::SWIM_ENTER:
            state_ = State::SWIM_IDLE;
            break;
        case AnimEvent::SWIM_EXIT:
            state_ = State::IDLE;
            break;
        case AnimEvent::JUMP:
            if (state_ != State::SWIM_IDLE && state_ != State::SWIM) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            }
            break;
        case AnimEvent::LANDED:
            if (state_ == State::JUMP_MID || state_ == State::JUMP_START) {
                jumpEndSeen_ = false;
                state_ = State::JUMP_END;
            }
            break;
        case AnimEvent::MOVE_START:
            graceTimer_ = kGraceSec;
            break;
        case AnimEvent::MOVE_STOP:
            // Grace timer handles the delay in updateTransitions
            break;
        default:
            break;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void LocomotionFSM::updateTransitions(const Input& in, const AnimCapabilitySet& caps) {
    // Update grace timer
    if (in.moving) {
        graceTimer_ = kGraceSec;
        wasSprinting_ = in.sprinting;
        wasMovingBackward_ = in.movingBackward;
        wasStrafeLeft_ = in.strafeLeft;
        wasStrafeRight_ = in.strafeRight;
    } else {
        graceTimer_ = std::max(0.0f, graceTimer_ - in.deltaTime);
    }

    const bool effectiveMoving = in.moving || graceTimer_ > 0.0f;
    const bool effectiveSprinting = in.sprinting || (!in.moving && effectiveMoving && wasSprinting_);

    switch (state_) {
        case State::IDLE:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (effectiveMoving && effectiveSprinting) {
                state_ = State::RUN;
            } else if (effectiveMoving) {
                state_ = State::WALK;
            } else if (in.turnDirection != 0) {
                startTurn(in.turnDirection);
            }
            break;

        case State::TURN:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (effectiveMoving) {
                state_ = effectiveSprinting ? State::RUN : State::WALK;
            } else if (in.turnDirection != 0 && (in.turnDirection > 0) != turnLeft_) {
                startTurn(in.turnDirection);
            } else {
                if (in.turnDirection != 0) turnedThisCycle_ = true;
                // Each cycle ends by asking whether the character turned
                // during it; mouse-look turns in steps, not every frame.
                if (shuffleCycleDone(in)) {
                    if (!turnedThisCycle_) state_ = State::IDLE;
                    turnedThisCycle_ = false;
                }
            }
            break;

        case State::WALK:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (effectiveSprinting) {
                state_ = State::RUN;
            }
            break;

        case State::RUN:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = State::JUMP_START;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (!effectiveMoving) {
                state_ = State::IDLE;
            } else if (!effectiveSprinting) {
                state_ = State::WALK;
            }
            break;

        case State::JUMP_START:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                state_ = effectiveMoving
                    ? (effectiveSprinting ? State::RUN : State::WALK) : State::JUMP_END;
                jumpEndSeen_ = false;
            } else if (caps.resolvedJumpStart == 0) {
                // Model doesn't have JUMP_START animation - skip to mid-air
                state_ = State::JUMP_MID;
            } else if (in.haveAnimState) {
                // Use the same resolved ID that resolve() outputs
                uint32_t expected = caps.resolvedJumpStart;
                if (in.currentAnimId == expected) jumpStartSeen_ = true;
                // Also detect completion via renderer's auto-STAND reset:
                // once the animation was seen and currentAnimId changed, it completed.
                if (jumpStartSeen_ && oneShotComplete(in, expected)) {
                    state_ = State::JUMP_MID;
                }
            } else {
                // No animation state available - fall through after 1 frame
                state_ = State::JUMP_MID;
            }
            break;

        case State::JUMP_MID:
            if (in.swimming) {
                state_ = State::SWIM_IDLE;
            } else if (in.grounded) {
                state_ = effectiveMoving
                    ? (effectiveSprinting ? State::RUN : State::WALK) : State::JUMP_END;
                jumpEndSeen_ = false;
            }
            break;

        case State::JUMP_END:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (effectiveMoving) {
                // Renderer blends the current jump/landing pose into the stride.
                state_ = effectiveSprinting ? State::RUN : State::WALK;
            } else if (caps.resolvedJumpEnd == 0) {
                // Model doesn't have JUMP_END animation - go straight to IDLE
                state_ = State::IDLE;
            } else if (in.haveAnimState) {
                uint32_t expected = caps.resolvedJumpEnd;
                if (in.currentAnimId == expected) jumpEndSeen_ = true;
                // Only transition to IDLE after landing animation completes
                if (jumpEndSeen_ && oneShotComplete(in, expected)) {
                    state_ = State::IDLE;
                }
            } else {
                state_ = State::IDLE;
            }
            break;

        case State::SWIM_IDLE:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (effectiveMoving) {
                state_ = State::SWIM;
            }
            break;

        case State::SWIM:
            if (!in.swimming) {
                state_ = effectiveMoving ? State::WALK : State::IDLE;
            } else if (!effectiveMoving) {
                state_ = State::SWIM_IDLE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput LocomotionFSM::resolve(const Input& in, const AnimCapabilitySet& caps) {
    updateTransitions(in, caps);

    // Stop grace preserves the whole movement pose, including direction.
    // Released input must not insert a forward stride before reaching idle.
    const bool stopping = !in.moving && graceTimer_ > 0.0f;
    const bool movingBackward = stopping ? wasMovingBackward_ : in.movingBackward;
    const bool strafeLeft = stopping ? wasStrafeLeft_ : in.strafeLeft;
    const bool strafeRight = stopping ? wasStrafeRight_ : in.strafeRight;
    const bool anyStrafeLeft = strafeLeft && !strafeRight && !movingBackward;
    const bool anyStrafeRight = strafeRight && !strafeLeft && !movingBackward;

    uint32_t animId = anim::STAND;
    bool animSelected = true;
    bool loop = true;

    switch (state_) {
        case State::IDLE:
            animId = anim::STAND;
            break;

        case State::TURN:
            // A model without the shuffle plays its AnimationData fallback, Stand.
            animId = turnLeft_ ? anim::SHUFFLE_LEFT : anim::SHUFFLE_RIGHT;
            break;

        case State::WALK:
            if (movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else {
                animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
            }
            break;

        case State::RUN:
            if (movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else if (in.sprintAura) {
                animId = caps.resolvedSprint ? caps.resolvedSprint : anim::RUN;
            } else {
                animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
            }
            break;

        case State::JUMP_START:
            animId = caps.resolvedJumpStart ? caps.resolvedJumpStart : anim::JUMP_START;
            loop = false;
            break;
        case State::JUMP_MID:
            animId = caps.resolvedJump ? caps.resolvedJump : anim::JUMP;
            loop = true;  // Must loop - long falls outlast a single play cycle
            break;
        case State::JUMP_END:
            animId = caps.resolvedJumpEnd ? caps.resolvedJumpEnd : anim::JUMP_END;
            loop = false;
            break;

        case State::SWIM_IDLE:
            animId = caps.resolvedSwimIdle ? caps.resolvedSwimIdle : anim::SWIM_IDLE;
            break;

        case State::SWIM:
            if (movingBackward) {
                animId = caps.resolvedSwimBackwards ? caps.resolvedSwimBackwards
                       : caps.resolvedSwim           ? caps.resolvedSwim
                       :                               anim::SWIM;
            } else if (anyStrafeLeft) {
                animId = caps.resolvedSwimLeft ? caps.resolvedSwimLeft
                       : caps.resolvedSwim    ? caps.resolvedSwim
                       :                        anim::SWIM;
            } else if (anyStrafeRight) {
                animId = caps.resolvedSwimRight ? caps.resolvedSwimRight
                       : caps.resolvedSwim     ? caps.resolvedSwim
                       :                         anim::SWIM;
            } else {
                animId = caps.resolvedSwim ? caps.resolvedSwim : anim::SWIM;
            }
            break;
    }

    if (!animSelected) return AnimOutput::stay();
    return AnimOutput::ok(animId, loop);
}

// ── Reset ────────────────────────────────────────────────────────────────────

void LocomotionFSM::reset() {
    state_ = State::IDLE;
    graceTimer_ = 0.0f;
    wasSprinting_ = false;
    wasMovingBackward_ = false;
    wasStrafeLeft_ = false;
    wasStrafeRight_ = false;
    jumpStartSeen_ = false;
    jumpEndSeen_ = false;
    turnLeft_ = false;
    turnedThisCycle_ = false;
    shuffleSeen_ = false;
    shuffleTime_ = 0.0f;
}

} // namespace rendering
} // namespace wowee
