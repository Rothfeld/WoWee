#pragma once
#include "rendering/animation/animation_ids.hpp"
#include <cstdint>
#include <optional>
#include <string_view>

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
inline constexpr std::optional<uint8_t> emoteStandState(std::string_view name) {
    if (name == "sit") return 1;
    if (name == "stand") return 0;
    if (name == "sleep") return 3;
    if (name == "kneel") return 8;
    return std::nullopt;
}
} // namespace wowee::rendering
