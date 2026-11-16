#pragma once
#include "rendering/animation/animation_ids.hpp"
#include <cstdint>
#include <optional>
#include <cassert>

namespace wowee::rendering {
struct StandPose {
    uint32_t enter;
    uint32_t idle;
    uint32_t leave;
};
inline constexpr StandPose standPose(uint8_t state) {
    switch (state) {
    case 1: return {anim::SIT_GROUND_DOWN, anim::SITTING, anim::SIT_GROUND_UP};
    case 3: return {anim::SLEEP_DOWN, anim::SLEEP, anim::SLEEP_UP};
    case 8: return {anim::KNEEL_START, anim::KNEEL_LOOP, anim::KNEEL_END};
    case 4: return {0, anim::SIT_CHAIR_LOW, 0};
    case 2: case 5: return {0, anim::SIT_CHAIR_MED, 0};
    case 6: return {0, anim::SIT_CHAIR_HIGH, 0};
    case 7: return {0, anim::DEAD, 0};
    default: return {0, anim::STAND, 0};
    }
}
inline constexpr std::optional<uint8_t> emoteStandState(uint32_t procedure, uint32_t parameter) {
    if (procedure == 1) {
        assert(parameter <= 8);
        return static_cast<uint8_t>(parameter);
    }
    return std::nullopt;
}
inline constexpr bool isSeatedStandState(uint8_t state) {
    return state == 1 || state == 2 || (state >= 4 && state <= 6);
}
inline constexpr bool isSeatedBaseAnimation(uint32_t animation) {
    return animation == anim::SITTING || animation == anim::SIT_CHAIR_LOW ||
        animation == anim::SIT_CHAIR_MED || animation == anim::SIT_CHAIR_HIGH;
}
inline constexpr bool emoteAllowsStandState(uint32_t flags, uint8_t state) {
    if ((flags & 1u) && state != 0) return false;
    return (state != 3 && state != 7) || (flags & 0x200u);
}
} // namespace wowee::rendering
