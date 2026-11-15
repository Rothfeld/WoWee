#pragma once

#include "game/entity.hpp"
#include "game/protocol_constants.hpp"

namespace wowee::ui {

// Priority matches the action: a corpse cannot trade and a hostile cannot chat.
inline const char* unitCursorPath(bool alive, uint32_t dynamicFlags, int reaction,
                                  uint32_t npcFlags, const char* questSymbol) {
    if (!alive) {
        return dynamicFlags & game::UNIT_DYNFLAG_LOOTABLE
            ? "Interface\\Cursor\\LootAll.blp" : "Interface\\Cursor\\Point.blp";
    }
    if (reaction <= 4) return "Interface\\Cursor\\Attack.blp";
    if (questSymbol) return questSymbol[0] == '!'
        ? "Interface\\Cursor\\Quest.blp" : "Interface\\Cursor\\QuestTurnIn.blp";
    if (npcFlags & game::NPC_FLAG_REPAIR) return "Interface\\Cursor\\RepairNPC.blp";
    if (npcFlags & game::NPC_FLAG_ANY_VENDOR) return "Interface\\Cursor\\Buy.blp";
    if (npcFlags & game::NPC_FLAG_TRAINER) return "Interface\\Cursor\\Trainer.blp";
    if (npcFlags & game::NPC_FLAG_FLIGHT_MASTER) return "Interface\\Cursor\\Taxi.blp";
    if (npcFlags & game::NPC_FLAG_INNKEEPER) return "Interface\\Cursor\\Innkeeper.blp";
    if (npcFlags & game::NPC_FLAG_GOSSIP) return "Interface\\Cursor\\Speak.blp";
    return "Interface\\Cursor\\Point.blp";
}

} // namespace wowee::ui
