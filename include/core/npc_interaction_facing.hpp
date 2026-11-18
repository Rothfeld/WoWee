#pragma once

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::core {

// Native 2.4.3 turns a conversation partner toward the player only while its
// stand state (UNIT_FIELD_BYTES_1 byte 0) is 0 (standing) and it is not
// UNIT_FLAG_STUNNED. Measured: stand states 1-9 (sit, chairs, sleep, dead,
// kneel, submerged) and the stun flag keep the server heading; emote state,
// pacified, combat, confused, fleeing, possessed, control-lost, root aura,
// unit flags 0x8040 and a dragon model still turn. Vaelastrasz (13020) lies
// in stand state 7 until his speech, so he does not turn.
inline bool npcTurnsToConversationPartner(uint8_t standState, uint32_t unitFlags) {
    return standState == 0 && (unitFlags & game::UNIT_FLAG_STUNNED) == 0;
}

// Native conversation turns settle in about 50 ms. The interval belongs to
// the heading change, not to each frame or repeated gossip response.
struct NpcInteractionFacing {
    float current;
    float target;
    float remaining = 0.0f;
    int turnDirection = 0; // +1 left, -1 right; event for this update only.

    explicit NpcInteractionFacing(float heading) : current(heading), target(heading) {}

    float advance(float requested, float dt) {
        assert(std::isfinite(requested) && std::isfinite(dt) && dt >= 0.0f);
        turnDirection = 0;
        if (std::abs(coords::normalizeAngleRad(requested - target)) > 0.0001f) {
            const float turn = coords::normalizeAngleRad(requested - current);
            if (std::abs(turn) > 0.01f) turnDirection = turn > 0.0f ? 1 : -1;
            target = requested;
            remaining = 0.05f;
        }
        const float elapsed = std::min(dt, remaining);
        if (remaining > 0.0f) {
            current = coords::normalizeAngleRad(current +
                coords::normalizeAngleRad(target - current) * (elapsed / remaining));
            remaining -= elapsed;
        }
        return current;
    }
};

} // namespace wowee::core
