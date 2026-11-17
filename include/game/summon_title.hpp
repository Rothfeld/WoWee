#pragma once

// The "<Owner's ...>" line under a summoned unit's name.
//
// FrameXML GlobalStrings carries the patterns (UNITNAME_TITLE_COMPANION
// "%s's Companion", UNITNAME_TITLE_MINION "%s's Minion", ...); the client
// picks one and fills in the owner's name. Two cases are established:
//   - a non-combat pet (creature type 12) names its creator as Companion;
//   - a unit created by a creature is its Pet when it is a beast (creature
//     type 1) and its Minion otherwise. All four are SUMMON_PET (effect 56)
//     cast by creature EventAI; original 2.4.3 on GM Island
//     (_experiments/20261117T164500Z.summon_title/original-spawned2.png,
//     original-close1.png, original-close2.png):
//       Tamed Hyena (beast, family 25, tameable) <Razorfen Beastmaster's Pet>
//       Tamed Ravager (beast, family 0, not tameable) <Mag'har Hunter's Pet>
//       Stone Rumbler (elemental, family 0) <Razorfen Geomancer's Minion>
//       Voidwalker Minion (demon, family 16) <Defias Conjurer's Minion>
//     So neither family nor the tameable flag decides it; the type does.
//   - a unit created by a player (original 2.4.3, Edrikd on pair 4,
//     _experiments/20261117T165200Z.pair4_kits/original-summons.png):
//       Stoneskin Totem 8071 (type 11)               <Edrikd's Totem>
//       Imp 688 (demon, SUMMON_PET: SUMMONEDBY set)   <Edrikd's Minion>
//       Skeletal Warrior 261 (undead, SUMMON 28)      <Edrikd's Guardian>
//       Boar Spirit 8286 (beast, SUMMON 28)            <Edrikd's Guardian>
//     CMaNGOS sets UNIT_FIELD_SUMMONEDBY only for SUMMON_PET/HUNTER_PET pets
//     (Pet::SetOwnerGuid); totems and guardians carry CREATEDBY alone.
//     A player's beast pet (hunter pet) was not observed and stays untitled.

#include <cstdint>
#include <string>

namespace wowee::game {

enum class SummonTitle { None, Companion, Pet, Guardian, Minion, Totem };

/// A player guid has a zero high word; creatures, pets and vehicles do not.
inline bool isPlayerGuid(uint64_t guid) { return guid != 0 && (guid >> 48) == 0; }

inline SummonTitle summonTitle(uint32_t creatureType, uint64_t creatorGuid, uint64_t summonedByGuid) {
    constexpr uint32_t kCreatureTypeBeast = 1;
    constexpr uint32_t kCreatureTypeTotem = 11;
    constexpr uint32_t kCreatureTypeNonCombatPet = 12;
    if (creatorGuid == 0) return SummonTitle::None;
    if (creatureType == kCreatureTypeNonCombatPet) return SummonTitle::Companion;
    const bool beast = creatureType == kCreatureTypeBeast;
    if (!isPlayerGuid(creatorGuid)) return beast ? SummonTitle::Pet : SummonTitle::Minion;
    if (creatureType == kCreatureTypeTotem) return SummonTitle::Totem;
    if (summonedByGuid == creatorGuid) return beast ? SummonTitle::None : SummonTitle::Minion;
    return SummonTitle::Guardian;
}

/// The GlobalStrings name holding the pattern, or nullptr for None.
inline const char* summonTitleGlobal(SummonTitle title) {
    switch (title) {
    case SummonTitle::Companion: return "UNITNAME_TITLE_COMPANION";
    case SummonTitle::Pet:       return "UNITNAME_TITLE_PET";
    case SummonTitle::Guardian:  return "UNITNAME_TITLE_GUARDIAN";
    case SummonTitle::Totem:     return "UNITNAME_SUMMON_TITLE4";
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
