#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include "rendering/animation/turn_shuffle.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// ============================================================================
// LocomotionFSM
//
// Pure logic state machine for local movement animation. No renderer dependency.
// States: IDLE · TURN · WALK · RUN · JUMP_START · JUMP_MID · JUMP_END · SWIM_IDLE · SWIM
//
// Transitions to IDLE / SWIM_IDLE immediately when movement stops, matching
// Native TBC 2.4.3 (smooth visual deceleration comes from M2 sequence crossfade).
// ============================================================================
class LocomotionFSM {
public:
    enum class State : uint8_t {
        IDLE, WALK, RUN,
        JUMP_START, JUMP_MID, JUMP_END,
        SWIM_IDLE, SWIM,
        FLY_IDLE, FLY,
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
        bool flying = false;
        bool sitting = false;
        bool stealthed = false;
        bool sprintAura = false;      // Sprint/Dash aura - use SPRINT anim
        float locomotionSpeed = 0.0f; // Current world speed (yd/s) for gait thresholds
        float walkStride = 0.0f;      // Scaled Walk (4) stride (yd/s)
        float runStride = 0.0f;       // Scaled Run (5) stride (yd/s)
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

    // One-shot tracking for jump start/end animations
    bool jumpStartSeen_ = false;
    bool jumpEndSeen_ = false;

    // Turning in place (TURN state).
    TurnShuffle shuffle_;

    /// Internal: update state transitions based on input.
    void updateTransitions(const Input& in, const AnimCapabilitySet& caps);
    [[nodiscard]] bool oneShotComplete(const Input& in, uint32_t expectedAnimId) const;
};

} // namespace rendering
} // namespace wowee
