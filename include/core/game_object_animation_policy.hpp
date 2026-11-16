#pragma once

#include <cstdint>

namespace wowee::core {

// Stateful objects retain the end of their authored Open/Close sequence.
// Late template metadata must not turn that held pose into a looping idle.
constexpr bool gameObjectPoseIsStateDriven(uint32_t type) {
    switch (type) {
        case 0:  // DOOR
        case 1:  // BUTTON
        case 3:  // CHEST
        case 6:  // TRAP
        case 9:  // TEXT: readable books
        case 10: // GOOBER
        case 33: // DESTRUCTIBLE_BUILDING
        case 35: // TRAPDOOR
            return true;
        default:
            return false;
    }
}

} // namespace wowee::core
