#pragma once

#include <string_view>
#include "game/entity.hpp"
#include "game/protocol_constants.hpp"
#include "game/quest_giver_status.hpp"

namespace wowee::ui {

// Priority matches the action: a corpse cannot trade and a hostile cannot chat.
inline const char* unitCursorPath(bool alive, uint32_t dynamicFlags, int reaction,
                                  uint32_t npcFlags, game::QuestGiverStatus questStatus,
                                  std::string_view iconName = {}, bool attackable = true,
                                  uint32_t unitFlags = 0, bool canHarvest = false,
                                  bool knowsHarvestProfession = false) {
    if (!alive) {
        if (dynamicFlags & game::UNIT_DYNFLAG_LOOTABLE) return "Interface\\Cursor\\LootAll.blp";
        if ((unitFlags & game::UNIT_FLAG_SKINNABLE) && knowsHarvestProfession) return canHarvest
            ? "Interface\\Cursor\\Skin.blp" : "Interface\\Cursor\\SkinError.blp";
        return "Interface\\Cursor\\Point.blp";
    }
    if (attackable && reaction <= 4) return "Interface\\Cursor\\Attack.blp";
    const char* questSymbol = questStatus == game::QuestGiverStatus::UNAVAILABLE
        ? nullptr : game::questGiverMarker(questStatus).symbol;
    if (questSymbol) return questSymbol[0] == '!'
        ? "Interface\\Cursor\\Quest.blp" : "Interface\\Cursor\\QuestTurnIn.blp";
    if (npcFlags & game::NPC_FLAG_INNKEEPER) return "Interface\\Cursor\\Innkeeper.blp";
    if (iconName == "Directions") return "Interface\\Cursor\\Directions.blp";
    if (iconName == "Buy") return "Interface\\Cursor\\Buy.blp";
    if (iconName == "Taxi") return "Interface\\Cursor\\Taxi.blp";
    if (iconName == "Trainer") return "Interface\\Cursor\\Trainer.blp";
    // Original 2.4.3 (pair 0, Honor Hold): repair beats vendor (Markus Scylan,
    // 0x1080 RepairNPC), trainer beats vendor (Brumman, 0x91 Trainer), and a
    // vendor shows Pickup.blp, not Buy.blp (Logistics Officer Ulrike 0x81,
    // Warrant Officer Tracy Proudwell 0x83: pixel-exact Pickup).
    if (npcFlags & game::NPC_FLAG_REPAIR) return "Interface\\Cursor\\RepairNPC.blp";
    if (npcFlags & game::NPC_FLAG_TRAINER) return "Interface\\Cursor\\Trainer.blp";
    if (npcFlags & game::NPC_FLAG_ANY_VENDOR) return "Interface\\Cursor\\Pickup.blp";
    if (npcFlags & (game::NPC_FLAG_BANKER | game::NPC_FLAG_AUCTIONEER)) return "Interface\\Cursor\\Buy.blp";
    if (npcFlags & game::NPC_FLAG_FLIGHT_MASTER) return "Interface\\Cursor\\Taxi.blp";
    if (npcFlags & (game::NPC_FLAG_GOSSIP | game::NPC_FLAG_BATTLEMASTER)) return "Interface\\Cursor\\Speak.blp";
    return "Interface\\Cursor\\Point.blp";
}

} // namespace wowee::ui
