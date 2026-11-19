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

// Whether READY loops the model's Stand where a model with Closed (147) would
// otherwise loop Closed. ScholomanceCrystalBall01.m2 (display 4891) authors
// Stand and Opened at 333-667 ms, constant, and Closed at 1000-2000 ms, which
// fades its additive ORBREFLECT layer from 1 to 0: a 1 s pulse. Paired 20 Hz
// recordings of all its READY spawns on original 2.4.3
// (_experiments/20261119T1445Z.orb_pulse): the 1 s pulse shows on QUESTGIVER
// (Orb of Command 179879), SPELL_FOCUS (Archmage Vargoth's Orb 183507) and
// GOOBER (Orb of Domination 177808, scale 1.5); GENERIC (Orb of the Blue Flight
// 188415) and CAMERA (Magisters' Terrace Scrying Orb 187578) glow steadily.
// A model without Stand plays Closed either way: its playable lookup sends
// Stand to Closed (every spawned GENERIC model with Closed and no Stand).
constexpr bool gameObjectReadyShowsStand(uint32_t type) {
    return type == 5u     // GENERIC
           || type == 13u; // CAMERA
}

} // namespace wowee::core
