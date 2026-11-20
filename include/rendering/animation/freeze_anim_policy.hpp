#pragma once

#include <cstdint>

namespace wowee::rendering {

/// SpellVisualKit.dbc CharProc 11 freezes the unit's model animation clock and
/// holds its current pose in place while any active aura's StateKit carries it
/// (10255 Stoned, 16245 Freeze Anim, 27619/45438 Ice Block, 17624 Petrification).
inline constexpr uint32_t kCharProcFreezeAnim = 11;

constexpr bool charProcFreezesAnimation(uint32_t charProc) {
    return charProc == kCharProcFreezeAnim;
}

/// While frozen, playAnimation() ignores pose changes once an initial sequence
/// is bound (preserving mid-action freezes and ignoring subsequent emotes or
/// locomotion), while still allowing an uninitialized spawn to bind its first
/// sequence at frame 0.
constexpr bool frozenAnimationAllowsPlay(bool frozen, int currentSequenceIndex) {
    return !frozen || currentSequenceIndex < 0;
}

}  // namespace wowee::rendering
