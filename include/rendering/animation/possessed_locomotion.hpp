#pragma once

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include <cstdint>
#include <optional>

namespace wowee {
namespace rendering {

/// Drives locomotion and jump animations for a locally possessed/charmed unit
/// (`possessedBody_ != 0`) directly from CameraController input state instead
/// of remote-entity position deltas.
class PossessedLocomotionDriver {
public:
    struct Input {
        bool dead = false;
        bool moving = false;
        bool sprinting = true;
        bool movingForward = false;
        bool movingBackward = false;
        bool strafeLeft = false;
        bool strafeRight = false;
        bool grounded = true;
        bool jumping = false;
        bool swimming = false;
        bool flying = false;
        bool hasFlyForward = false;
        bool hasFlyIdle = false;
        int turnDirection = 0;
        bool haveAnimState = false;
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
    };

    struct Decision {
        bool shouldPlay = false;
        uint32_t animId = anim::STAND;
        bool loop = true;
    };

    void reset() {
        trackedGuid_ = 0;
        trackedInstanceId_ = 0;
        fsm_.reset();
        caps_ = AnimCapabilitySet{};
        capsProbed_ = false;
        lastAnimId_ = UINT32_MAX;
        lastLoop_ = true;
        lastYawDeg_.reset();
    }

    uint64_t trackedGuid() const { return trackedGuid_; }
    uint32_t trackedInstanceId() const { return trackedInstanceId_; }
    bool capsProbed() const { return capsProbed_; }
    const AnimCapabilitySet& capabilities() const { return caps_; }
    LocomotionFSM::State locomotionState() const { return fsm_.getState(); }

    void bind(uint64_t guid, uint32_t instanceId, const AnimCapabilitySet& caps) {
        if (trackedGuid_ != guid || trackedInstanceId_ != instanceId) {
            reset();
            trackedGuid_ = guid;
            trackedInstanceId_ = instanceId;
        }
        caps_ = caps;
        capsProbed_ = true;
    }

    int computeTurnDirection(float yawDeg, bool turningInput) {
        int dir = 0;
        if (lastYawDeg_ && turningInput) {
            float delta = yawDeg - *lastYawDeg_;
            while (delta > 180.0f) delta -= 360.0f;
            while (delta < -180.0f) delta += 360.0f;
            if (delta != 0.0f) dir = delta > 0.0f ? 1 : -1;
        }
        lastYawDeg_ = yawDeg;
        return dir;
    }

    static bool isLocomotionAnimation(uint32_t id) {
        switch (id) {
            case anim::STAND:
            case anim::WALK:
            case anim::RUN:
            case anim::SHUFFLE_LEFT:
            case anim::SHUFFLE_RIGHT:
            case anim::WALK_BACKWARDS:
            case anim::JUMP_START:
            case anim::JUMP:
            case anim::JUMP_END:
            case anim::FALL:
            case anim::SWIM_IDLE:
            case anim::SWIM:
            case anim::SWIM_LEFT:
            case anim::SWIM_RIGHT:
            case anim::SWIM_BACKWARDS:
            case anim::SPRINT:
            case anim::FLY_FORWARD:
            case anim::FLY_IDLE:
                return true;
            default:
                return false;
        }
    }

    Decision step(const Input& in) {
        if (in.dead || (in.haveAnimState && anim::isCorpsePose(in.currentAnimId))) {
            return {};
        }

        // Preserve active one-shot spell cast / attack animations while standing still
        // on the ground so casting possessed abilities plays through cleanly.
        if (!in.moving && in.grounded && !in.swimming && !in.flying && in.haveAnimState &&
            !isLocomotionAnimation(in.currentAnimId) &&
            in.currentAnimDuration > 0.1f &&
            in.currentAnimTime < in.currentAnimDuration - 0.05f) {
            lastAnimId_ = UINT32_MAX;
            return {};
        }

        uint32_t animId = anim::STAND;
        bool loop = true;

        if (in.flying) {
            if (in.moving) {
                animId = in.hasFlyForward ? anim::FLY_FORWARD
                       : caps_.resolvedRun ? caps_.resolvedRun
                       : caps_.resolvedWalk ? caps_.resolvedWalk
                       : anim::RUN;
            } else {
                animId = in.hasFlyIdle ? anim::FLY_IDLE : caps_.resolvedStand;
            }
        } else {
            LocomotionFSM::Input loco;
            loco.moving = in.moving;
            loco.sprinting = in.sprinting;
            loco.movingForward = in.movingForward;
            loco.movingBackward = in.movingBackward;
            loco.strafeLeft = in.strafeLeft;
            loco.strafeRight = in.strafeRight;
            loco.grounded = in.grounded;
            loco.jumping = in.jumping;
            loco.swimming = in.swimming;
            loco.turnDirection = in.turnDirection;
            loco.haveAnimState = in.haveAnimState;
            loco.currentAnimId = in.currentAnimId;
            loco.currentAnimTime = in.currentAnimTime;
            loco.currentAnimDuration = in.currentAnimDuration;

            const AnimOutput out = fsm_.resolve(loco, caps_);
            if (!out.valid) return {};
            animId = out.animId;
            loop = out.loop;

            // Models without jump sequences (e.g. Creature\Drake\Drake.m2) fall back
            // to STAND in AnimationData.dbc; resolve explicitly here so `drifted`
            // does not re-trigger playAnimation every frame while airborne.
            if (animId == anim::JUMP_START && caps_.resolvedJumpStart == 0) {
                animId = caps_.resolvedJump ? caps_.resolvedJump : caps_.resolvedStand;
                loop = true;
            } else if (animId == anim::JUMP && caps_.resolvedJump == 0) {
                animId = caps_.resolvedStand;
            } else if (animId == anim::JUMP_END && caps_.resolvedJumpEnd == 0) {
                animId = caps_.resolvedStand;
                loop = true;
            } else if (animId == anim::SWIM_IDLE && caps_.resolvedSwimIdle == 0) {
                animId = caps_.resolvedStand;
            } else if (animId == anim::SWIM && caps_.resolvedSwim == 0) {
                animId = caps_.resolvedRun ? caps_.resolvedRun : caps_.resolvedWalk;
            } else if ((animId == anim::SHUFFLE_LEFT || animId == anim::SHUFFLE_RIGHT) &&
                       !caps_.hasStand) {
                animId = caps_.resolvedStand;
            }
        }

        const bool requestChanged = (lastAnimId_ != animId) || (lastLoop_ != loop);
        const bool drifted = in.haveAnimState && in.currentAnimId != animId && loop;
        const bool shouldPlay = requestChanged || drifted;
        if (shouldPlay) {
            lastAnimId_ = animId;
            lastLoop_ = loop;
        }
        return {shouldPlay, animId, loop};
    }

private:
    uint64_t trackedGuid_ = 0;
    uint32_t trackedInstanceId_ = 0;
    LocomotionFSM fsm_;
    AnimCapabilitySet caps_;
    bool capsProbed_ = false;
    uint32_t lastAnimId_ = UINT32_MAX;
    bool lastLoop_ = true;
    std::optional<float> lastYawDeg_;
};

} // namespace rendering
} // namespace wowee
