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
// keeps their Stand running, a flame front running round the rune once a
// second. Frozen at time zero every flame stood at one point of the ring.
constexpr bool gameObjectBindPoseShowsReady(uint32_t type) {
    return type != 6u && gameObjectPoseIsStateDriven(type);
}

// Whether ACTIVE shows the model's Destroy (150), held at its end, where a
// model without Open (148) and Opened (149) has one. Measured on original
// 2.4.3 (_experiments/20261118T230000Z.brazier_state): Zul'Gurub's Brazier of
// Madness (BUTTON 180327, G_BrazierTroll: Destroy, Stand, Custom0) goes out
// when used and stays out, in view again too; a Thunderbrew Lager Keg
// (GOOBER 164911, G_Keg: Stand, Destroy) bursts. Where Open exists it wins
// over Destroy: Blackrock Depths' Supply Crate (GOOBER 166872, G_Crate01)
// lifts its lid and the Bar Door (DOOR 170571, BlackrockDoorSingle) swings.
// GENERIC objects keep Stand: the Troll Hero Fire/Air Portals (180253/180254,
// Fire/AirRift: Stand, Hold, Destroy) burn on in ACTIVE.
constexpr bool gameObjectActiveShowsDestroy(uint32_t type, bool hasOpen, bool hasOpened,
                                            bool hasDestroy) {
    return gameObjectPoseIsStateDriven(type) && hasDestroy && !hasOpen && !hasOpened;
}

} // namespace wowee::core
