#pragma once

#include "game/handler_types.hpp"

#include <cstdint>
#include <optional>

namespace wowee::rendering {

// Pure TBC 2.4.3 unit spell-cast animation transition policy.
//
// Empirical Native TBC 2.4.3.8606 behavior (observed frame-by-frame on OgreMage.m2
// with Earthgrab Totem 8376 [52->54], Healing Wave 15982 [52->54], Chain Heal 1064 [51->53]):
//   1. PRECAST or CHANNEL arriving while a CAST one-shot is still playing MUST NOT
//      interrupt the one-shot. The one-shot finishes its authored duration and then
//      crossfades into the new held precast/channel animation.
//   2. CAST arriving for a DIFFERENT spell while a PRECAST or CHANNEL is held plays
//      once and returns to that held precast/channel animation (the hold is only
//      released when the held spell itself finishes or is stopped).
//   3. Back-to-back CAST one-shots with the SAME animation ID do not restart the
//      already-playing one-shot; back-to-back CAST one-shots with DIFFERENT animation
//      IDs (e.g. SpellCastOmni 54 -> SpellCastDirected 53) crossfade immediately into
//      the new one-shot.
//   4. STOP arriving while a previous CAST one-shot is still playing clears the hold
//      and retargets the one-shot's return to idle instead of snapping mid-cast.
struct SpellCastHold {
    uint32_t spellId = 0;
    uint32_t animation = 0;
    bool channel = false;
};

struct SpellCastOneShot {
    uint32_t spellId = 0;
    uint32_t animation = 0;
};

struct SpellCastAnimState {
    std::optional<SpellCastHold> hold;
    std::optional<SpellCastOneShot> oneShot;
};

struct SpellCastAnimAction {
    enum class Kind : uint8_t {
        NONE,
        PLAY_LOOP,
        PLAY_ONESHOT,
        RETARGET_ONESHOT_RETURN,
    };

    Kind kind = Kind::NONE;
    uint32_t animation = 0;
    uint32_t returnAnimation = 0;
};

/// True when a creature MonsterMove spline actually translates the unit
/// (excluding stationary facing-only turns such as dur=1ms, speed=0).
inline constexpr bool shouldStartCreatureLocomotion(uint32_t durationMs, float locomotionSpeed) {
    return durationMs > 0 && locomotionSpeed > 0.0001f;
}

/// Compute the next animation action for a remote unit (creature or remote player)
/// receiving a spell cast animation phase event.
///
/// `idleAnimation` is the unit's non-cast idle/ready animation (what the unit should
/// return to when no cast hold is active).
inline SpellCastAnimAction stepRemoteSpellCastAnim(
    SpellCastAnimState& state,
    game::SpellAnimPhase phase,
    uint32_t spellId,
    uint32_t kitAnimation,
    uint32_t noAnimationSentinel,
    uint32_t idleAnimation,
    bool playingOneShot,
    uint32_t playingOneShotAnim) {
    const bool activeCastOneShot =
        state.oneShot.has_value() && playingOneShot && playingOneShotAnim == state.oneShot->animation;

    if (!activeCastOneShot) {
        state.oneShot.reset();
    }

    switch (phase) {
    case game::SpellAnimPhase::STOP: {
        if (!state.hold.has_value()) {
            state.oneShot.reset();
            return {};
        }
        state.hold.reset();
        if (activeCastOneShot) {
            return {SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN, 0, idleAnimation};
        }
        state.oneShot.reset();
        return {SpellCastAnimAction::Kind::PLAY_LOOP, idleAnimation, 0};
    }

    case game::SpellAnimPhase::PRECAST: {
        if (kitAnimation == noAnimationSentinel) {
            return {};
        }
        state.hold = SpellCastHold{spellId, kitAnimation, false};
        if (activeCastOneShot) {
            return {SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN, 0, kitAnimation};
        }
        state.oneShot.reset();
        return {SpellCastAnimAction::Kind::PLAY_LOOP, kitAnimation, 0};
    }

    case game::SpellAnimPhase::CAST: {
        const bool underOtherHold = state.hold.has_value() && state.hold->spellId != spellId;
        if (kitAnimation == noAnimationSentinel) {
            // Only this spell's own precast ends here.
            if (state.hold.has_value() && !state.hold->channel && state.hold->spellId == spellId) {
                state.hold.reset();
                if (activeCastOneShot) {
                    return {SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN, 0, idleAnimation};
                }
                return {SpellCastAnimAction::Kind::PLAY_LOOP, idleAnimation, 0};
            }
            return {};
        }

        const uint32_t returnAnim = underOtherHold ? state.hold->animation : idleAnimation;
        if (!underOtherHold) {
            state.hold.reset();
        }

        // Back-to-back CAST one-shots with the same animation ID let the active
        // one-shot finish instead of restarting from frame 0.
        if (activeCastOneShot && playingOneShotAnim == kitAnimation) {
            state.oneShot = SpellCastOneShot{spellId, kitAnimation};
            return {SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN, 0, returnAnim};
        }

        state.oneShot = SpellCastOneShot{spellId, kitAnimation};
        return {SpellCastAnimAction::Kind::PLAY_ONESHOT, kitAnimation, returnAnim};
    }

    case game::SpellAnimPhase::CHANNEL: {
        if (kitAnimation == noAnimationSentinel) {
            return {};
        }
        state.hold = SpellCastHold{spellId, kitAnimation, true};
        if (activeCastOneShot) {
            return {SpellCastAnimAction::Kind::RETARGET_ONESHOT_RETURN, 0, kitAnimation};
        }
        state.oneShot.reset();
        return {SpellCastAnimAction::Kind::PLAY_LOOP, kitAnimation, 0};
    }
    }

    return {};
}

} // namespace wowee::rendering
