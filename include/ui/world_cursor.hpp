#pragma once

#include <string>
#include <string_view>
#include "game/entity.hpp"
#include "game/protocol_constants.hpp"
#include "game/quest_giver_status.hpp"

namespace wowee::ui {

// Priority matches the action: a corpse cannot trade and a hostile cannot chat.
// serviceInReach: the player stands within interaction reach of a living
// unit (GameHandler::unitWithinInteractionReach); its service cursor turns
// to the Unable variant beyond it, as the original 2.4.3 shows (pair 8:
// Newton Burnside UnableBuy, Ironbark the Redeemed UnableSpeak, Innkeeper
// Allison UnableQuest). The attack cursor keeps its look.
inline std::string unitCursorPath(bool alive, uint32_t dynamicFlags, int reaction,
                                  uint32_t npcFlags, game::QuestGiverStatus questStatus,
                                  std::string_view iconName = {}, bool attackable = true,
                                  uint32_t unitFlags = 0, bool canHarvest = false,
                                  bool knowsHarvestProfession = false, bool serviceInReach = true) {
    const auto cursor = [](std::string_view name) {
        return "Interface\\Cursor\\" + std::string(name) + ".blp";
    };
    if (!alive) {
        // A lootable corpse shows Pickup.blp, the bag (Gruul the Dragonkiller,
        // Clefthoof, Warmaul Brute; pixel-exact, pair 8), not LootAll.blp.
        // Beyond loot reach (GameHandler::unitWithinLootReach), native 2.4.3
        // greys it to UnablePickup.blp (General Rajaxx, Gruul, Swamp Jaguar).
        if (dynamicFlags & game::UNIT_DYNFLAG_LOOTABLE)
            return cursor(serviceInReach ? "Pickup" : "UnablePickup");
        if ((unitFlags & game::UNIT_FLAG_SKINNABLE) && knowsHarvestProfession)
            return cursor(canHarvest ? "Skin" : "SkinError");
        return cursor("Point");
    }
    if (attackable && reaction <= 4) return cursor("Attack");
    const auto service = [&](std::string_view name) {
        return cursor(std::string(serviceInReach ? "" : "Unable") + std::string(name));
    };
    if (const char* questCursor = game::questGiverCursor(questStatus)) return service(questCursor);
    if (npcFlags & game::NPC_FLAG_INNKEEPER) return service("Innkeeper");
    if (iconName == "Directions") return service("Directions");
    if (iconName == "Buy") return service("Buy");
    if (iconName == "Taxi") return service("Taxi");
    if (iconName == "Trainer") return service("Trainer");
    // Original 2.4.3 (pair 0, Honor Hold): repair beats vendor (Markus Scylan,
    // 0x1080 RepairNPC), trainer beats vendor (Brumman, 0x91 Trainer), and a
    // vendor shows Pickup.blp, not Buy.blp (Logistics Officer Ulrike 0x81,
    // Warrant Officer Tracy Proudwell 0x83: pixel-exact Pickup).
    if (npcFlags & game::NPC_FLAG_REPAIR) return service("RepairNPC");
    if (npcFlags & game::NPC_FLAG_TRAINER) return service("Trainer");
    if (npcFlags & game::NPC_FLAG_ANY_VENDOR) return service("Pickup");
    if (npcFlags & (game::NPC_FLAG_BANKER | game::NPC_FLAG_AUCTIONEER)) return service("Buy");
    if (npcFlags & game::NPC_FLAG_FLIGHT_MASTER) return service("Taxi");
    // Battlemasters carry gossip and show Speak (Arena Battlemaster, Eye of
    // the Storm Emissary); a stable master (Sylista 0x400000) and a quest
    // giver with nothing for the player (Morgan Pestle) the plain pointer.
    if (npcFlags & (game::NPC_FLAG_GOSSIP | game::NPC_FLAG_BATTLEMASTER)) return service("Speak");
    return cursor("Point");
}

} // namespace wowee::ui
