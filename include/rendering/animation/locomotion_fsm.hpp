#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// LocomotionFSM
//
// Pure logic state machine for movement animation. No renderer dependency.
// States: IDLE · TURN · WALK · RUN · JUMP_START · JUMP_MID · JUMP_END · SWIM_IDLE · SWIM
//
// Grace timer is internal - no external locomotionStopGraceTimer_ needed.
// ============================================================================
class LocomotionFSM {
public:
    enum class State : uint8_t {
        IDLE, WALK, RUN,
        JUMP_START, JUMP_MID, JUMP_END,
        SWIM_IDLE, SWIM,
        // Standing and turning in place: ShuffleLeft/ShuffleRight.
        TURN,
    };

    struct Input {
        bool moving = false;
        bool movingForward = false;
        bool sprinting = false;
        bool movingBackward = false;
        bool strafeLeft = false;
        bool strafeRight = false;
        bool grounded = true;
        bool jumping = false;
        bool swimming = false;
        bool sitting = false;
        bool sprintAura = false;      // Sprint/Dash aura - use SPRINT anim
        // Facing turned this frame by the player while standing:
        // +1 left (counter-clockwise from above), -1 right, 0 not turning.
        int turnDirection = 0;
        float deltaTime = 0.0f;
        // Animation state for one-shot completion detection (jump start/end)
        uint32_t currentAnimId = 0;
        float currentAnimTime = 0.0f;
        float currentAnimDuration = 0.0f;
        bool haveAnimState = false;
    };

    /// Process event and update internal state.
    void onEvent(AnimEvent event);

    /// Evaluate current state against input and capabilities.
    /// Returns AnimOutput with valid=false if no change needed (STAY policy).
    AnimOutput resolve(const Input& in, const AnimCapabilitySet& caps);

    [[nodiscard]] State getState() const { return state_; }
    void setState(State s) { state_ = s; }
    void reset();

    static constexpr uint8_t PRIORITY = 10;

private:
    State state_ = State::IDLE;

    // Grace timer: short delay before switching from WALK/RUN to IDLE
    // to avoid flickering on network jitter
    float graceTimer_ = 0.0f;
    bool wasSprinting_ = false;
    bool wasMovingBackward_ = false;
    bool wasStrafeLeft_ = false;
    bool wasStrafeRight_ = false;

    // One-shot tracking for jump start/end animations
    bool jumpStartSeen_ = false;
    bool jumpEndSeen_ = false;

    // Turning in place. A shuffle cycle in which the character turned is
    // followed by another; one without a turn returns to Stand. Build 8606's
    // own character keeps stepping for a cycle after a turn stops.
    bool turnLeft_ = false;
    bool turnedThisCycle_ = false;
    bool shuffleSeen_ = false;
    float shuffleTime_ = 0.0f;

    static constexpr float kGraceSec = 0.12f;

    /// Internal: update state transitions based on input.
    void updateTransitions(const Input& in, const AnimCapabilitySet& caps);
    void startTurn(int direction);
    [[nodiscard]] bool shuffleCycleDone(const Input& in);
    [[nodiscard]] bool oneShotComplete(const Input& in, uint32_t expectedAnimId) const;
};

} // namespace rendering
} // namespace wowee
