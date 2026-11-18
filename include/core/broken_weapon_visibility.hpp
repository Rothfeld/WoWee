#pragma once

#include <array>
#include <cstdint>

namespace wowee::core {

// Which of the local player's weapons build 8606 has stopped drawing because
// they broke. Not a durability rule: the original drops the model at the
// moment the item's ITEM_FIELD_DURABILITY goes from above zero to zero, and
// draws it again whenever it rebuilds that weapon - so a broken weapon is on
// show after login, a map change, a sheath change or equipping it.
//
// Measured on the original, pair 13 (Edrikm, rogue, daggers 3187/5742/15244
// and bow 15287), _experiments/20261118T124909.broken_items:
//  - The break hides the slot's model, in hand or sheathed, and also the
//    weapon then held in the same hand: an off hand breaking in the ranged
//    state takes the bow out of the left hand; the bow breaking in the melee
//    state takes the off-hand dagger out of it. The off hand, sheathed at the
//    hip while the bow is in hand, stays when the bow breaks.
//  - Repair (durability above zero again) brings back that slot only; the
//    off hand the bow took with it stays hidden.
//  - Any sheath state change, a different (or re-equipped) item in the slot,
//    and a new player model (login, map change) draw everything again.
//  - /reload, a same-map teleport, death and resurrection, opening the
//    character frame and equipping another slot change nothing; the
//    character frame's figure shows what the world model shows.
//  - Armour is never hidden, and neither is another player's weapon: their
//    item objects (and so their durability) never reach this client.
class BrokenWeaponVisibility {
public:
    // Main hand, off hand, ranged - the order of EquipSlot 15, 16, 17.
    static constexpr int kSlots = 3;

    struct Slot {
        uint64_t itemGuid = 0;        // 0: nothing equipped
        bool broken = false;          // durability 0 of a nonzero maximum
        uint32_t heldPoint = 0;       // attachment the item is held at
        uint8_t heldSheathState = 0;  // sheath state in which it is in hand
    };
    using Snapshot = std::array<Slot, kSlots>;

    // Fold in the equipment as it is now, for the character model
    // `instanceId` in sheath state `sheathState`.
    void observe(const Snapshot& now, uint32_t instanceId, uint8_t sheathState) {
        if (instanceId != instanceId_) {
            // A new model is built from the equipment, broken or not.
            hidden_ = {};
            last_ = now;
            instanceId_ = instanceId;
            sheathState_ = sheathState;
            return;
        }
        if (sheathState != sheathState_) {
            hidden_ = {};
            sheathState_ = sheathState;
        }
        for (int s = 0; s < kSlots; ++s) {
            Hidden& h = hidden_[s];
            if (h.itemGuid != now[s].itemGuid || (h.own && !now[s].broken)) h = {};
        }
        for (int s = 0; s < kSlots; ++s) {
            const Slot& item = now[s];
            const bool breaks = item.itemGuid != 0 && item.broken &&
                                last_[s].itemGuid == item.itemGuid && !last_[s].broken;
            if (!breaks) continue;
            hidden_[s] = {.itemGuid = item.itemGuid, .own = true};
            for (int t = 0; t < kSlots; ++t) {
                const Slot& other = now[t];
                if (t == s || other.itemGuid == 0 || hidden_[t].itemGuid != 0) continue;
                if (other.heldPoint == item.heldPoint && other.heldSheathState == sheathState)
                    hidden_[t] = {.itemGuid = other.itemGuid, .own = false};
            }
        }
        last_ = now;
    }

    [[nodiscard]] bool hidden(int slot) const { return hidden_[slot].itemGuid != 0; }
    [[nodiscard]] bool any() const {
        for (const Hidden& h : hidden_) if (h.itemGuid != 0) return true;
        return false;
    }

private:
    struct Hidden {
        uint64_t itemGuid = 0;
        bool own = false;  // broke itself, rather than shared its hand
    };
    std::array<Hidden, kSlots> hidden_{};
    Snapshot last_{};
    uint32_t instanceId_ = 0;
    uint8_t sheathState_ = 0;
};

}  // namespace wowee::core
