#pragma once

#include <cstdint>

namespace wowee::game {

// TBC Spell TRACK_RESOURCES misc values and Lock skill indices share LockType IDs.
// Find Herbs is 2, Find Minerals is 3; PLAYER_TRACK_RESOURCES stores bit(id - 1).
inline bool tracksResourceLock(uint32_t trackingMask, uint32_t keyType, uint32_t lockType) {
    return keyType == 2 && lockType > 0 && lockType <= 32 &&
        (trackingMask & (uint32_t{1} << (lockType - 1))) != 0;
}

// A tracked node remains visible while another player works it. Only a
// conditional quest object's per-player activation changes its availability.
inline bool resourceTrackingAvailable(uint32_t flags, uint32_t dynamicFlags) {
    return !(flags & 0x04u) || (dynamicFlags & 0x01u);
}

} // namespace wowee::game
