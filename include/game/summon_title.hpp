#pragma once

// The "<Owner's ...>" line under a summoned unit's name.
//
// FrameXML GlobalStrings carries the patterns (UNITNAME_TITLE_COMPANION
// "%s's Companion", UNITNAME_TITLE_MINION "%s's Minion", ...); the client
// picks one and fills in the owner's name. Two cases are established:
//   - a non-combat pet (creature type 12) names its creator as Companion;
//   - a unit created by a creature names it as Minion: the Razorfen
//     Geomancer's Stone Rumbler (CMaNGOS spell 8270, SUMMON_PET; only
//     UNIT_FIELD_CREATEDBY and UNIT_CREATED_BY_SPELL set) reads
//     "<Razorfen Geomancer's Minion>" in the original
//     (_experiments/20261117T152914Z.stale_target_frame/original_rumbler.png).
// Units summoned by players (hunter and warlock pets, totems, guardians) are
// not decided here: no capture says which pattern they take.

#include <cstdint>
#include <string>

namespace wowee::game {

enum class SummonTitle { None, Companion, Minion };

/// A player guid has a zero high word; creatures, pets and vehicles do not.
inline bool isPlayerGuid(uint64_t guid) { return guid != 0 && (guid >> 48) == 0; }

inline SummonTitle summonTitle(uint32_t creatureType, uint64_t creatorGuid) {
    if (creatorGuid == 0) return SummonTitle::None;
    if (creatureType == 12) return SummonTitle::Companion;  // non-combat pet
    if (!isPlayerGuid(creatorGuid)) return SummonTitle::Minion;
    return SummonTitle::None;
}

/// The GlobalStrings name holding the pattern, or nullptr for None.
inline const char* summonTitleGlobal(SummonTitle title) {
    switch (title) {
    case SummonTitle::Companion: return "UNITNAME_TITLE_COMPANION";
    case SummonTitle::Minion:    return "UNITNAME_TITLE_MINION";
    case SummonTitle::None:      return nullptr;
    }
    return nullptr;
}

/// The pattern with its one %s replaced by the owner's name.
inline std::string formatSummonTitle(std::string pattern, const std::string& owner) {
    const auto at = pattern.find("%s");
    if (at == std::string::npos) return {};
    pattern.replace(at, 2, owner);
    return pattern;
}

} // namespace wowee::game
