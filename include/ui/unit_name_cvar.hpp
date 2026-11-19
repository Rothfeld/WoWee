#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wowee {
namespace ui {

/// The CVar that decides whether a unit's name is drawn over it, with the
/// value build 8606 answers when the CVar was never set (GetCVarDefault).
struct UnitNameCVar {
    const char* name;
    const char* defaultValue;
};

/// Build 8606 (TBC 2.4.3) names a unit by one of eleven CVars, the Names
/// panel's NamePanelOptions. It has no UnitNameNonCombatCreatureName and no
/// totem or guardian CVars (GetCVar: "Couldn't find CVar named ..."); those
/// are Wrath's. Measured on build 8606, pair 19, by turning each CVar off
/// alone (_experiments/20261119T093137Z.far_unit_names):
///   - players: UnitName{Friendly,Enemy}PlayerName;
///   - a player's pet (Imp 688, SUMMONEDBY the player): ...PetName;
///   - a player's totem (Stoneskin 8071) and guardian (Mechanical Dragonling
///     4073), CREATEDBY the player alone: ...CreationName;
///   - a companion (Bombay 10673, creature type 12): UnitNameCompanionName;
///   - everything else is an NPC: Stormwind City Guard, Lady Jaina, a Rabbit
///     (critter, type 8), a spawned Mana Spring Totem with no owner, and the
///     Imp Minion a Dawnblade Summoner keeps (an NPC's pet) all go with
///     UnitNameNPC.
/// Defaults: UnitNameOwn and UnitNameNPC 0, every other one 1.
inline constexpr UnitNameCVar kTbcUnitNameCVars[] = {
    {"UnitNameOwn", "0"},
    {"UnitNameNPC", "0"},
    {"UnitNamePlayerGuild", "1"},
    {"UnitNamePlayerPVPTitle", "1"},
    {"UnitNameEnemyPlayerName", "1"},
    {"UnitNameEnemyPetName", "1"},
    {"UnitNameEnemyCreationName", "1"},
    {"UnitNameFriendlyPlayerName", "1"},
    {"UnitNameFriendlyPetName", "1"},
    {"UnitNameFriendlyCreationName", "1"},
    {"UnitNameCompanionName", "1"},
};

/// Build 8606's default for one of the CVars above, compared without case;
/// nullptr for any other name.
inline const char* tbcUnitNameCVarDefault(std::string_view lowerName) {
    for (const auto& cvar : kTbcUnitNameCVars) {
        const std::string_view name(cvar.name);
        if (name.size() != lowerName.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            const char c = name[i];
            same = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == lowerName[i];
        }
        if (same) return cvar.defaultValue;
    }
    return nullptr;
}

/// What build 8606 knows about a unit when it picks the name CVar.
struct UnitNameKind {
    bool player = false;
    bool hostile = false;
    bool companion = false;      ///< creature type 12, non-combat pet
    uint64_t summonedBy = 0;     ///< UNIT_FIELD_SUMMONEDBY
    uint64_t createdBy = 0;      ///< UNIT_FIELD_CREATEDBY
};

/// The CVar build 8606 reads for this unit's name (see kTbcUnitNameCVars).
/// Only a player's summons are pets or creations; an NPC's are NPCs.
inline UnitNameCVar tbcUnitNameCVar(const UnitNameKind& unit) {
    const auto playerGuid = [](uint64_t guid) { return guid != 0 && (guid >> 48) == 0; };
    const auto pick = [](const char* name) {
        for (const auto& cvar : kTbcUnitNameCVars)
            if (std::string_view(cvar.name) == name) return cvar;
        assert(!"name is one of kTbcUnitNameCVars");
        return UnitNameCVar{name, "1"};
    };
    if (unit.player) return pick(unit.hostile ? "UnitNameEnemyPlayerName" : "UnitNameFriendlyPlayerName");
    if (unit.companion) return pick("UnitNameCompanionName");
    if (playerGuid(unit.summonedBy))
        return pick(unit.hostile ? "UnitNameEnemyPetName" : "UnitNameFriendlyPetName");
    if (playerGuid(unit.createdBy))
        return pick(unit.hostile ? "UnitNameEnemyCreationName" : "UnitNameFriendlyCreationName");
    return pick("UnitNameNPC");
}

} // namespace ui
} // namespace wowee
