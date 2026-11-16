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
                                  uint32_t unitFlags = 0) {
    if (!alive) {
        if (dynamicFlags & game::UNIT_DYNFLAG_LOOTABLE) return "Interface\\Cursor\\LootAll.blp";
        if (unitFlags & game::UNIT_FLAG_SKINNABLE) return "Interface\\Cursor\\Skin.blp";
        return "Interface\\Cursor\\Point.blp";
    }
    if (attackable && reaction <= 4) return "Interface\\Cursor\\Attack.blp";
    const char* questSymbol = questStatus == game::QuestGiverStatus::UNAVAILABLE
        ? nullptr : game::questGiverMarker(questStatus).symbol;
    if (questSymbol) return questSymbol[0] == '!'
        ? "Interface\\Cursor\\Quest.blp" : "Interface\\Cursor\\QuestTurnIn.blp";
    if (iconName == "Directions") return "Interface\\Cursor\\Directions.blp";
    if (iconName == "Buy") return "Interface\\Cursor\\Buy.blp";
    if (iconName == "Taxi") return "Interface\\Cursor\\Taxi.blp";
    if (iconName == "Trainer") return "Interface\\Cursor\\Trainer.blp";
    if (npcFlags & game::NPC_FLAG_REPAIR) return "Interface\\Cursor\\RepairNPC.blp";
    if (npcFlags & (game::NPC_FLAG_ANY_VENDOR | game::NPC_FLAG_BANKER | game::NPC_FLAG_AUCTIONEER)) return "Interface\\Cursor\\Buy.blp";
    if (npcFlags & game::NPC_FLAG_TRAINER) return "Interface\\Cursor\\Trainer.blp";
    if (npcFlags & game::NPC_FLAG_FLIGHT_MASTER) return "Interface\\Cursor\\Taxi.blp";
    if (npcFlags & game::NPC_FLAG_INNKEEPER) return "Interface\\Cursor\\Innkeeper.blp";
    if (npcFlags & game::NPC_FLAG_GOSSIP) return "Interface\\Cursor\\Speak.blp";
    return "Interface\\Cursor\\Point.blp";
}

} // namespace wowee::ui
