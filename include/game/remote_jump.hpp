#pragma once

#include "game/world_packets.hpp"

namespace wowee::game {

inline float remoteJumpHorizontalSpeed(const MovementInfo& info, bool retainedMomentum) {
    // Native stationary water jumps carry the swim speed in jumpXYSpeed even
    // though their X/Y never change. Direction flags establish travel at
    // takeoff; later packets may release those flags while retaining momentum.
    const bool directional = info.hasFlag(MovementFlags::FORWARD) ||
        info.hasFlag(MovementFlags::BACKWARD) ||
        info.hasFlag(MovementFlags::STRAFE_LEFT) ||
        info.hasFlag(MovementFlags::STRAFE_RIGHT);
    return directional || retainedMomentum ? info.jumpXYSpeed : 0.0f;
}

} // namespace wowee::game
