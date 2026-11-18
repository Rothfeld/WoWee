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

// Whether a stateful object without state sequences shows READY through its
// bind pose. A trap does not: the Molten Core rune rings (TRAP 178187-178193,
// FlameCircle.m2, Stand and Hold only) are READY and the original client
// keeps their Stand running, a ring of fire widening round the rune each
// second. Frozen at time zero every flame stood at one point of the ring.
constexpr bool gameObjectBindPoseShowsReady(uint32_t type) {
    return type != 6u && gameObjectPoseIsStateDriven(type);
}

} // namespace wowee::core
