#pragma once

#include "game/world_packets.hpp"

namespace wowee::game {

inline uint32_t gameObjectLockId(const GameObjectQueryResponseData* info) {
    if (!info || !info->hasData) return 0;
    switch (info->type) {
        case 0: // Door
        case 1: // Button
            return info->data[1];
        case 2: case 3: case 6: case 10: case 12: case 13: case 24: case 26:
            return info->data[0];
        default:
            return 0;
    }
}

} // namespace wowee::game
