#pragma once
#include <cstdint>

namespace wowee::rendering {
inline constexpr bool gestureSheathesWeapons(uint32_t emoteFlags, uint32_t animationWeaponFlags) {
    // NOSHEATHE emotes can share the normal emote's animation (Wave 3/70).
    return !(emoteFlags & 0x4u) && (animationWeaponFlags & 0x14u);
}
inline constexpr bool gestureWeaponPoseActive(uint32_t gestureAnimation,
                                              uint32_t baseAnimation,
                                              uint32_t upperAnimation) {
    return gestureAnimation != 0 &&
        (gestureAnimation == baseAnimation || gestureAnimation == upperAnimation);
}
} // namespace wowee::rendering
