#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/creature_locomotion.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

// ── One-shot completion helper ─────────────────────────────────────────────────────────

bool LocomotionFSM::oneShotComplete(const Input& in, uint32_t expectedAnimId) const {
    if (!in.haveAnimState) return false;
    return in.currentAnimId != expectedAnimId ||
           (in.currentAnimDuration > 0.1f && in.currentAnimTime >= in.currentAnimDuration - 0.05f);
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
            if (state_ != State::SWIM_IDLE && state_ != State::SWIM &&
                state_ != State::FLY_IDLE && state_ != State::FLY) {
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
        case AnimEvent::MOVE_STOP:
            break;
        default:
            break;
    }
}

// ── State transitions ────────────────────────────────────────────────────────

void LocomotionFSM::updateTransitions(const Input& in, const AnimCapabilitySet& caps) {
    const bool effectiveMoving = in.moving;
    const bool effectiveSprinting = in.sprinting;

    if (!in.swimming && in.flying && state_ != State::FLY_IDLE && state_ != State::FLY) {
        state_ = effectiveMoving ? State::FLY : State::FLY_IDLE;
        return;
    }

    switch (state_) {
        case State::IDLE:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = caps.resolvedJumpStart != 0 ? State::JUMP_START : State::JUMP_MID;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (effectiveMoving && effectiveSprinting) {
                state_ = State::RUN;
            } else if (effectiveMoving) {
                state_ = State::WALK;
            } else if (in.turnDirection != 0) {
                state_ = State::TURN;
                shuffle_.start(in.turnDirection);
            }
            break;

        case State::TURN:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = caps.resolvedJumpStart != 0 ? State::JUMP_START : State::JUMP_MID;
            } else if (!in.grounded) {
                state_ = State::JUMP_MID;
            } else if (effectiveMoving) {
                state_ = effectiveSprinting ? State::RUN : State::WALK;
            } else if (!shuffle_.update(in.turnDirection, in.haveAnimState,
                                        in.currentAnimId, in.currentAnimTime)) {
                state_ = State::IDLE;
            }
            break;

        case State::WALK:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.grounded && in.jumping) {
                jumpStartSeen_ = false;
                state_ = caps.resolvedJumpStart != 0 ? State::JUMP_START : State::JUMP_MID;
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
                state_ = caps.resolvedJumpStart != 0 ? State::JUMP_START : State::JUMP_MID;
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
                    ? (effectiveSprinting ? State::RUN : State::WALK)
                    : (caps.resolvedJumpEnd != 0 ? State::JUMP_END : State::IDLE);
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
                    ? (effectiveSprinting ? State::RUN : State::WALK)
                    : (caps.resolvedJumpEnd != 0 ? State::JUMP_END : State::IDLE);
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
                if (in.flying) state_ = effectiveMoving ? State::FLY : State::FLY_IDLE;
                else state_ = effectiveMoving ? (effectiveSprinting ? State::RUN : State::WALK) : State::IDLE;
            } else if (effectiveMoving) {
                state_ = State::SWIM;
            }
            break;

        case State::SWIM:
            if (!in.swimming) {
                if (in.flying) state_ = effectiveMoving ? State::FLY : State::FLY_IDLE;
                else state_ = effectiveMoving ? (effectiveSprinting ? State::RUN : State::WALK) : State::IDLE;
            } else if (!effectiveMoving) {
                state_ = State::SWIM_IDLE;
            }
            break;

        case State::FLY_IDLE:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.flying) {
                if (!in.grounded) {
                    if (in.jumping) jumpStartSeen_ = false;
                    state_ = (in.jumping && caps.resolvedJumpStart != 0) ? State::JUMP_START : State::JUMP_MID;
                } else if (effectiveMoving) {
                    state_ = effectiveSprinting ? State::RUN : State::WALK;
                } else {
                    state_ = State::IDLE;
                }
            } else if (effectiveMoving) {
                state_ = State::FLY;
            }
            break;

        case State::FLY:
            if (in.swimming) {
                state_ = effectiveMoving ? State::SWIM : State::SWIM_IDLE;
            } else if (!in.flying) {
                if (!in.grounded) {
                    if (in.jumping) jumpStartSeen_ = false;
                    state_ = (in.jumping && caps.resolvedJumpStart != 0) ? State::JUMP_START : State::JUMP_MID;
                } else if (effectiveMoving) {
                    state_ = effectiveSprinting ? State::RUN : State::WALK;
                } else {
                    state_ = State::IDLE;
                }
            } else if (!effectiveMoving) {
                state_ = State::FLY_IDLE;
            }
            break;
    }
}

// ── Animation resolution ─────────────────────────────────────────────────────

AnimOutput LocomotionFSM::resolve(const Input& in, const AnimCapabilitySet& caps) {
    updateTransitions(in, caps);

    const bool movingBackward = in.movingBackward;
    const bool strafeLeft = in.strafeLeft;
    const bool strafeRight = in.strafeRight;
    const bool anyStrafeLeft = strafeLeft && !strafeRight && !movingBackward;
    const bool anyStrafeRight = strafeRight && !strafeLeft && !movingBackward;
    const bool hasStealthLoco = caps.resolvedStealthWalk != 0 || caps.resolvedStealthRun != 0;

    uint32_t animId = anim::STAND;
    bool animSelected = true;
    bool loop = true;

    switch (state_) {
        case State::IDLE:
            animId = anim::STAND;
            break;

        case State::TURN:
            // A model without the shuffle plays its AnimationData fallback, Stand.
            animId = shuffle_.animation();
            break;

        case State::WALK:
        case State::RUN:
            if (movingBackward) {
                animId = caps.resolvedWalkBackwards ? caps.resolvedWalkBackwards
                       : caps.resolvedWalk           ? caps.resolvedWalk
                       :                               anim::WALK_BACKWARDS;
            } else if (in.stealthed && hasStealthLoco) {
                if (state_ == State::WALK) {
                    animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
                } else {
                    animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
                }
            } else {
                const std::optional<float> walkStride =
                    in.walkStride > 0.0f ? std::optional<float>(in.walkStride) : std::nullopt;
                const std::optional<float> runStride =
                    in.runStride > 0.0f ? std::optional<float>(in.runStride) : std::nullopt;
                const uint32_t gait = selectGroundForwardGaitAnimation(
                    state_ == State::WALK, in.locomotionSpeed,
                    walkStride, runStride, caps.hasSprint,
                    state_ == State::RUN && in.sprintAura);
                if (gait == anim::SPRINT) {
                    animId = caps.resolvedSprint ? caps.resolvedSprint
                           : caps.resolvedRun    ? caps.resolvedRun
                           :                       anim::RUN;
                } else if (gait == anim::WALK) {
                    animId = caps.resolvedWalk ? caps.resolvedWalk : anim::WALK;
                } else {
                    animId = caps.resolvedRun ? caps.resolvedRun : anim::RUN;
                }
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

        case State::FLY_IDLE:
            animId = caps.hasFly
                ? anim::FLY_IDLE
                : (caps.resolvedSwimIdle ? caps.resolvedSwimIdle : anim::SWIM_IDLE);
            break;

        case State::FLY:
            if (caps.hasFly) {
                animId = anim::FLY_FORWARD;
            } else if (movingBackward) {
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
    jumpStartSeen_ = false;
    jumpEndSeen_ = false;
    shuffle_.reset();
}

} // namespace rendering
} // namespace wowee
