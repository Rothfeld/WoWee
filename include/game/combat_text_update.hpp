#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "game/spell_defines.hpp"

namespace wowee {
namespace game {

/// COMBAT_TEXT_UPDATE as build 8606 fires it for the player: arg1 is the
/// message type Blizzard_CombatText switches on, arg2/arg3 its data (empty
/// strings are passed to Lua as nil).
///
/// Read back on pair 7 with a frame registered for the event
/// (_experiments/20261118T074350Z.plates_fct, enableCombatText 0 and 1 alike):
///   DAMAGE|13             melee hit taken
///   MISS                  melee miss, no data
///   ABSORB                melee hit absorbed whole (Power Word: Shield)
///   SPELL_RESIST          spell resisted whole
///   HEAL|Edrikg|225       arg2 the healer's name, arg3 the amount
///   ENERGY|100            power gained (Thistle Tea)
///   SPELL_AURA_START|Power Word: Fortitude, SPELL_AURA_END|Rejuvenation,
///   SPELL_AURA_START_HARMFUL|Weakened Soul, SPELL_AURA_END_HARMFUL|...
/// The partial forms (BLOCK/ABSORB/RESIST with the damage in arg2 and the
/// reduced part in arg3) follow Blizzard_CombatText.lua's reading of them;
/// they were not provoked natively.
struct CombatTextUpdate {
    std::string type;
    std::string arg2;
    std::string arg3;
};

/// The update for one line that landed on the player, or nothing when build
/// 8606 has no message type for it. `precedingDamage` is the damage of the
/// hit this line belongs to (-1 when it does not follow one): a reduction of
/// a hit that still did damage is the partial form.
inline std::optional<CombatTextUpdate> combatTextUpdateFor(CombatTextEntry::Type type, int32_t amount,
                                                           bool fromSpell, uint8_t powerType,
                                                           std::string_view casterName,
                                                           int32_t precedingDamage) {
    using T = CombatTextEntry;
    const std::string spell = fromSpell ? "SPELL_" : "";
    const std::string value = std::to_string(amount);
    switch (type) {
        case T::MELEE_DAMAGE: case T::GLANCING: case T::CRUSHING:
            if (amount <= 0) return std::nullopt;
            return CombatTextUpdate{"DAMAGE", value, {}};
        case T::CRIT_DAMAGE:
            if (amount <= 0) return std::nullopt;
            return CombatTextUpdate{fromSpell ? "SPELL_DAMAGE_CRIT" : "DAMAGE_CRIT", value, {}};
        case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE:
            if (amount <= 0) return std::nullopt;
            return CombatTextUpdate{"SPELL_DAMAGE", value, {}};
        case T::MISS: return CombatTextUpdate{spell + "MISS", {}, {}};
        case T::DODGE: return CombatTextUpdate{spell + "DODGE", {}, {}};
        case T::PARRY: return CombatTextUpdate{spell + "PARRY", {}, {}};
        case T::EVADE: return CombatTextUpdate{spell + "EVADE", {}, {}};
        case T::IMMUNE: return CombatTextUpdate{spell + "IMMUNE", {}, {}};
        case T::DEFLECT: return CombatTextUpdate{spell + "DEFLECT", {}, {}};
        case T::REFLECT: return CombatTextUpdate{"SPELL_REFLECT", {}, {}};
        case T::BLOCK:
            if (precedingDamage > 0)
                return CombatTextUpdate{fromSpell ? "SPELL_BLOCKED" : "BLOCK", std::to_string(precedingDamage), value};
            return CombatTextUpdate{fromSpell ? "SPELL_BLOCK" : "BLOCK", {}, {}};
        case T::ABSORB:
            if (precedingDamage > 0)
                return CombatTextUpdate{fromSpell ? "SPELL_ABSORBED" : "ABSORB", std::to_string(precedingDamage), value};
            return CombatTextUpdate{fromSpell ? "SPELL_ABSORB" : "ABSORB", {}, {}};
        case T::RESIST:
            if (precedingDamage > 0)
                return CombatTextUpdate{fromSpell ? "SPELL_RESISTED" : "RESIST", std::to_string(precedingDamage), value};
            return CombatTextUpdate{fromSpell ? "SPELL_RESIST" : "RESIST", {}, {}};
        case T::HEAL: return CombatTextUpdate{"HEAL", std::string(casterName), value};
        case T::CRIT_HEAL: return CombatTextUpdate{"HEAL_CRIT", std::string(casterName), value};
        case T::PERIODIC_HEAL: return CombatTextUpdate{"PERIODIC_HEAL", std::string(casterName), value};
        case T::ENERGIZE: {
            static constexpr const char* kPower[] = {"MANA", "RAGE", "FOCUS", "ENERGY"};
            if (amount <= 0 || powerType > 3) return std::nullopt;
            return CombatTextUpdate{kPower[powerType], value, {}};
        }
        case T::HONOR_GAIN:
            return CombatTextUpdate{"HONOR_GAINED", value, {}};
        default:
            return std::nullopt;
    }
}

} // namespace game
} // namespace wowee
