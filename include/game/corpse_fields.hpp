#pragma once

#include "game/game_utils.hpp"
#include <cstdint>

namespace wowee::game {

// Corpse fields from CMaNGOS/AzerothCore UpdateFields.h. TBC adds PARTY;
// Wrath moves the four position/facing fields into the movement block.
struct CorpseFields {
    uint16_t item;
    uint16_t bytes1;
    uint16_t bytes2;
    uint16_t flags;
    static CorpseFields resolve() {
        if (isActiveExpansion("tbc")) return {15, 34, 35, 37};
        if (isPreWotlk()) return {13, 32, 33, 35};
        return {11, 30, 31, 33};
    }
};

inline constexpr uint32_t corpseFlagBones = 0x01;

} // namespace wowee::game
