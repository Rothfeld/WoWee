#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "game/handler_types.hpp"
#include "game/spell_defines.hpp"

namespace wowee {
namespace game {

/// Which interface the arguments are for. 2.4.3's combat log addon reads damage
/// without an overkill and a heal without overhealing or absorbed, and 3.3.5's
/// reads both; a layout for one handed to the other is read a field out.
/// 1.12 has no combat log event at all.
enum class CombatLogLayout { Tbc, Wotlk };

/// Explicit non-CombatTextEntry subevents routed through buildCombatLogEvent.
enum class CombatLogSubevent : uint8_t {
    FromCombatText = 0,
    AuraApplied,
    AuraRemoved,
    AuraRefresh,
    AuraAppliedDose,
    AuraRemovedDose,
    PartyKill,
    UnitDied,
    UnitDestroyed,
    SpellSummon,
    SpellCreate,
    SpellCastFailed,
};

/// What the client knows about one combat event, before it is shaped for Lua.
struct CombatLogEventInput {
    CombatLogLayout layout = CombatLogLayout::Wotlk;
    CombatLogSubevent subevent = CombatLogSubevent::FromCombatText;
    CombatTextEntry::Type type = CombatTextEntry::MELEE_DAMAGE;
    int32_t amount = 0;
    uint32_t spellId = 0;
    uint32_t extraSpellId = 0;
    uint32_t spellSchoolMask = 0;
    uint32_t extraSpellSchoolMask = 0;
    uint32_t damageSchoolMask = 0;
    uint8_t powerType = 0;
    uint64_t srcGuid = 0;
    uint64_t dstGuid = 0;
    uint64_t playerGuid = 0;
    uint64_t petGuid = 0;
    uint32_t srcFlags = 0;
    uint32_t dstFlags = 0;
    std::string srcName;
    std::string dstName;
    std::string spellName;
    std::string extraSpellName;
    int64_t timestamp = 0;
    bool normalRangedAttack = false;
};

namespace combat_log {

/// COMBATLOG_OBJECT_* bits (TBC 2.4.3 / WotLK Constants.lua).
inline constexpr uint32_t kMine = 0x1u, kParty = 0x2u, kRaid = 0x4u, kOutsider = 0x8u;
inline constexpr uint32_t kAffiliationMask = 0x0000000Fu;
inline constexpr uint32_t kFriendly = 0x10u, kNeutral = 0x20u, kHostile = 0x40u;
inline constexpr uint32_t kReactionMask = 0x000000F0u;
inline constexpr uint32_t kControlPlayer = 0x100u, kControlNpc = 0x200u;
inline constexpr uint32_t kControlMask = 0x00000300u;
inline constexpr uint32_t kTypePlayer = 0x400u, kTypeNpc = 0x800u, kTypePet = 0x1000u;
inline constexpr uint32_t kTypeGuardian = 0x2000u, kTypeObject = 0x4000u;
inline constexpr uint32_t kTypeMask = 0x0000FC00u;
inline constexpr uint32_t kTarget = 0x00010000u, kFocus = 0x00020000u;
inline constexpr uint32_t kMainTank = 0x00040000u, kMainAssist = 0x00080000u;
inline constexpr uint32_t kRaidTarget1 = 0x00100000u;
inline constexpr uint32_t kNone = 0x80000000u;
inline constexpr uint32_t kSpecialMask = 0xFFFF0000u;

/// Convert a Lua numeric bitmask (which Lua 5.1 bit.bor may hand over as negative
/// int32, e.g. -2147483648 or -1, or unsigned double up to 4294967295) to uint32_t
/// without C++ negative-float-to-unsigned undefined behaviour.
[[nodiscard]] inline uint32_t doubleToBits(double n) {
    if (!std::isfinite(n)) return 0u;
    const auto i64 = static_cast<int64_t>(n);
    return static_cast<uint32_t>(i64);
}

/// TBC 2.4.3 CombatLog_Object_IsA(unitFlags, mask):
/// - Requires at least one overlapping bit (`(flags & mask) != 0`).
/// - When both `flags` and `mask` carry standard 4-axis unit bits (`0xFFFF`),
///   each constrained axis present in both must overlap; upper `0xFFFF0000`
///   bits (such as `COMBATLOG_OBJECT_NONE` in `COMBATLOG_FLAG_LIST` or `TARGET`)
///   do not reject a matching unit.
/// - Otherwise (standalone special mask such as `COMBATLOG_OBJECT_TARGET`,
///   `RAIDTARGET*`, or `COMBATLOG_OBJECT_NONE` on a sourceless event), matches
///   iff `(flags & mask & kSpecialMask) != 0`.
[[nodiscard]] inline constexpr bool objectIsA(uint32_t flags, uint32_t mask) {
    if ((flags & mask) == 0u) return false;
    if ((flags & 0xFFFFu) != 0u && (mask & 0xFFFFu) != 0u) {
        constexpr uint32_t kCategories[] = {
            kAffiliationMask, kReactionMask, kControlMask, kTypeMask,
        };
        for (uint32_t cat : kCategories) {
            if ((flags & cat) != 0u && (mask & cat) != 0u && (flags & mask & cat) == 0u) {
                return false;
            }
        }
        return true;
    }
    return (flags & mask & kSpecialMask) != 0u;
}

/// Fallback unit flags when no live GameHandler entity/faction context is supplied.
inline uint32_t unitFlags(uint64_t guid, uint64_t playerGuid, uint64_t petGuid) {
    if (guid == 0) return kNone;
    if (guid == playerGuid) return kMine | kFriendly | kControlPlayer | kTypePlayer;
    if (guid == petGuid)    return kMine | kFriendly | kControlPlayer | kTypePet;
    const uint16_t high = static_cast<uint16_t>(guid >> 48);
    if (high == 0) return kOutsider | kFriendly | kControlPlayer | kTypePlayer;
    if (high == 0xF140u) return kOutsider | kNeutral | kControlNpc | kTypePet;
    if (high == 0xF110u || high == 0xF111u) return kOutsider | kNeutral | kControlNpc | kTypeObject;
    return kOutsider | kHostile | kControlNpc | kTypeNpc;
}

inline std::string guidText(uint64_t guid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(guid));
    return buf;
}

inline std::string textOrNil(const std::string& text) {
    return text.empty() ? std::string(kEventNil) : text;
}

inline const char* environmentName(uint8_t type) {
    switch (type) {
        case 0: return "Fatigue";
        case 1: return "Drowning";
        case 2: return "Falling";
        case 3: return "Lava";
        case 4: return "Slime";
        case 5: return "Fire";
        default: return "Falling";
    }
}

}  // namespace combat_log

/// The arguments of one combat event as Blizzard_CombatLog parses them in this
/// client: the timestamp, the subevent, the source's guid, name and flags, the
/// destination's, then the prefix and suffix that subevent carries. There is no
/// hideCaster and there are no raid flags; those arrived with Cataclysm, and a
/// parser that does not expect them reads every later field one place off. Empty when the event has no counterpart in the real
/// client's log - experience, honour and the partial absorb and resist lines
/// that ride along with a hit, which the hit's own event already accounts for.
///
/// The parser does arithmetic on fields past the destination, so a layout that
/// is merely close takes the whole handler down: SWING_DAMAGE subtracts the
/// overkill from the amount, and a missing overkill is a nil in that sum.
inline std::vector<std::string> buildCombatLogEvent(const CombatLogEventInput& in) {
    using T = CombatTextEntry;
    using namespace combat_log;

    const bool hasSpell = in.spellId != 0;
    const std::string amount = std::to_string(in.amount);
    const uint32_t resolvedSpellSchool =
        in.spellSchoolMask != 0 ? in.spellSchoolMask
                                : (in.damageSchoolMask != 0 ? in.damageSchoolMask : 1u);
    const uint32_t resolvedExtraSchool =
        in.extraSpellSchoolMask != 0 ? in.extraSpellSchoolMask : 1u;
    const std::string spellSchool = std::to_string(resolvedSpellSchool);
    const std::string extraSchool = std::to_string(resolvedExtraSchool);
    const std::string nil = kEventNil;

    // Prefix: a spell's id, name and school. A swing has none.
    auto spellPrefix = [&] {
        return std::vector<std::string>{std::to_string(in.spellId),
                                        in.spellName.empty() ? "Spell " + std::to_string(in.spellId)
                                                             : in.spellName,
                                        spellSchool};
    };
    const bool wotlk = in.layout == CombatLogLayout::Wotlk;
    // Suffix of a damage event: amount, [overkill,] school, resisted, blocked,
    // absorbed, critical, glancing, crushing.
    auto damageSuffix = [&](uint32_t schoolMask, bool crit, bool glancing, bool crushing) {
        std::vector<std::string> out{amount};
        if (wotlk) out.push_back("0");   // overkill
        out.insert(out.end(), {std::to_string(schoolMask != 0 ? schoolMask : 1u),
                               nil, nil, nil, std::string(eventBool(crit)),
                               std::string(eventBool(glancing)), std::string(eventBool(crushing))});
        return out;
    };
    auto healSuffix = [&](bool crit) {
        if (!wotlk) return std::vector<std::string>{amount, eventBool(crit)};
        return std::vector<std::string>{amount, "0", nil, eventBool(crit)};
    };

    std::string event;
    std::vector<std::string> prefix;
    std::vector<std::string> suffix;
    bool environmental = false;
    bool destIsNone = false;

    if (in.subevent != CombatLogSubevent::FromCombatText) {
        const char* auraType = in.powerType != 0 ? "DEBUFF" : "BUFF";
        switch (in.subevent) {
            case CombatLogSubevent::AuraApplied:
            case CombatLogSubevent::AuraRemoved:
            case CombatLogSubevent::AuraRefresh:
                if (!hasSpell || in.dstGuid == 0) return {};
                event = in.subevent == CombatLogSubevent::AuraApplied
                            ? "SPELL_AURA_APPLIED"
                            : (in.subevent == CombatLogSubevent::AuraRemoved
                                   ? "SPELL_AURA_REMOVED"
                                   : "SPELL_AURA_REFRESH");
                prefix = spellPrefix();
                suffix = {auraType};
                break;
            case CombatLogSubevent::AuraAppliedDose:
            case CombatLogSubevent::AuraRemovedDose:
                if (!hasSpell || in.dstGuid == 0) return {};
                event = in.subevent == CombatLogSubevent::AuraAppliedDose
                            ? "SPELL_AURA_APPLIED_DOSE"
                            : "SPELL_AURA_REMOVED_DOSE";
                prefix = spellPrefix();
                suffix = {auraType, amount};
                break;
            case CombatLogSubevent::PartyKill:
                if (in.srcGuid == 0 || in.dstGuid == 0) return {};
                event = "PARTY_KILL";
                break;
            case CombatLogSubevent::UnitDied:
            case CombatLogSubevent::UnitDestroyed:
                if (in.dstGuid == 0) return {};
                event = in.subevent == CombatLogSubevent::UnitDestroyed
                            ? "UNIT_DESTROYED"
                            : "UNIT_DIED";
                environmental = true; // Force 0x0000000000000000 / nil / COMBATLOG_OBJECT_NONE source
                break;
            case CombatLogSubevent::SpellSummon:
            case CombatLogSubevent::SpellCreate:
                if (!hasSpell || in.srcGuid == 0 || in.dstGuid == 0) return {};
                event = in.subevent == CombatLogSubevent::SpellCreate
                            ? "SPELL_CREATE"
                            : "SPELL_SUMMON";
                prefix = spellPrefix();
                break;
            case CombatLogSubevent::SpellCastFailed:
                if (!hasSpell || in.srcGuid == 0 || in.extraSpellName.empty()) return {};
                event = "SPELL_CAST_FAILED";
                destIsNone = true; // Force 0x0000000000000000 / nil / COMBATLOG_OBJECT_NONE dest
                prefix = spellPrefix();
                suffix = {in.extraSpellName};
                break;
            case CombatLogSubevent::FromCombatText:
                break;
        }
    } else switch (in.type) {
        case T::MELEE_DAMAGE: case T::CRIT_DAMAGE: case T::GLANCING: case T::CRUSHING:
        case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE: {
            const bool swing = !hasSpell && (in.type == T::MELEE_DAMAGE || in.type == T::CRIT_DAMAGE ||
                                              in.type == T::GLANCING || in.type == T::CRUSHING);
            const bool periodic = in.type == T::PERIODIC_DAMAGE;
            const bool ranged = hasSpell && !periodic && in.normalRangedAttack;
            event = swing ? "SWING_DAMAGE"
                          : (periodic ? "SPELL_PERIODIC_DAMAGE"
                                      : (ranged ? "RANGE_DAMAGE" : "SPELL_DAMAGE"));
            if (!swing) prefix = spellPrefix();
            const uint32_t hitSchool =
                in.damageSchoolMask != 0 ? in.damageSchoolMask
                                         : (!swing ? resolvedSpellSchool : 1u);
            suffix = damageSuffix(hitSchool, in.type == T::CRIT_DAMAGE,
                                  in.type == T::GLANCING, in.type == T::CRUSHING);
            break;
        }
        case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
            event = in.type == T::PERIODIC_HEAL ? "SPELL_PERIODIC_HEAL" : "SPELL_HEAL";
            prefix = spellPrefix();
            suffix = healSuffix(in.type == T::CRIT_HEAL);
            break;
        case T::ENVIRONMENTAL: {
            event = "ENVIRONMENTAL_DAMAGE";
            environmental = true;
            prefix = {environmentName(in.powerType)};
            const uint32_t envSchool =
                in.damageSchoolMask != 0 ? in.damageSchoolMask
                                         : ((in.powerType == 3 || in.powerType == 5) ? 4u : 1u);
            suffix = damageSuffix(envSchool, false, false, false);
            break;
        }
        case T::ENERGIZE:
        case T::PERIODIC_ENERGIZE:
            event = (in.type == T::PERIODIC_ENERGIZE) ? "SPELL_PERIODIC_ENERGIZE" : "SPELL_ENERGIZE";
            prefix = spellPrefix();
            suffix = {amount, std::to_string(in.powerType)};
            break;
        case T::POWER_DRAIN:
            event = "SPELL_DRAIN";
            prefix = spellPrefix();
            suffix = {amount, std::to_string(in.powerType), "0"};
            break;
        case T::MISS: case T::DODGE: case T::PARRY: case T::BLOCK: case T::EVADE:
        case T::IMMUNE: case T::DEFLECT: case T::REFLECT: {
            event = !hasSpell ? "SWING_MISSED"
                              : (in.normalRangedAttack ? "RANGE_MISSED" : "SPELL_MISSED");
            if (hasSpell) prefix = spellPrefix();
            const char* missType =
                in.type == T::DODGE ? "DODGE" : in.type == T::PARRY ? "PARRY" :
                in.type == T::BLOCK ? "BLOCK" : in.type == T::EVADE ? "EVADE" :
                in.type == T::IMMUNE ? "IMMUNE" : in.type == T::DEFLECT ? "DEFLECT" :
                in.type == T::REFLECT ? "REFLECT" : "MISS";
            suffix = {missType};
            // The parser formats these three with the amount turned aside.
            if (wotlk && in.type == T::BLOCK) suffix.push_back(amount);
            break;
        }
        case T::RESIST: case T::ABSORB:
            if (in.type == T::RESIST && in.extraSpellId != 0 && hasSpell) {
                event = "SPELL_DISPEL_FAILED";
                prefix = spellPrefix();
                suffix = {std::to_string(in.extraSpellId),
                          in.extraSpellName.empty() ? "Spell " + std::to_string(in.extraSpellId)
                                                    : in.extraSpellName,
                          extraSchool};
                break;
            }
            // With an amount these are the part of a hit that did not land, and
            // the hit's own event carries that. With none the whole thing was
            // turned aside, which is a miss.
            if (in.amount > 0) return {};
            event = !hasSpell ? "SWING_MISSED"
                              : (in.normalRangedAttack ? "RANGE_MISSED" : "SPELL_MISSED");
            if (hasSpell) prefix = spellPrefix();
            suffix = {in.type == T::RESIST ? "RESIST" : "ABSORB"};
            if (wotlk) suffix.push_back("0");
            break;
        case T::DISPEL: case T::STEAL: {
            if (!hasSpell) return {};
            event = (in.type == T::STEAL) ? "SPELL_STOLEN" : "SPELL_DISPEL";
            const uint32_t castId = in.extraSpellId != 0 ? in.extraSpellId : in.spellId;
            const std::string& castName = in.extraSpellId != 0 ? in.extraSpellName : in.spellName;
            const std::string& castSchool = in.extraSpellId != 0 ? extraSchool : spellSchool;
            prefix = {std::to_string(castId),
                      castName.empty() ? "Spell " + std::to_string(castId) : castName,
                      castSchool};
            suffix = {std::to_string(in.spellId),
                      in.spellName.empty() ? "Spell " + std::to_string(in.spellId) : in.spellName,
                      spellSchool,
                      in.powerType != 0 ? "DEBUFF" : "BUFF"};
            break;
        }
        case T::INTERRUPT: {
            if (!hasSpell) return {};
            event = "SPELL_INTERRUPT";
            const uint32_t castId = in.extraSpellId != 0 ? in.extraSpellId : in.spellId;
            const std::string& castName = in.extraSpellId != 0 ? in.extraSpellName : in.spellName;
            const std::string& castSchool = in.extraSpellId != 0 ? extraSchool : spellSchool;
            prefix = {std::to_string(castId),
                      castName.empty() ? "Spell " + std::to_string(castId) : castName,
                      castSchool};
            suffix = {std::to_string(in.spellId),
                      in.spellName.empty() ? "Spell " + std::to_string(in.spellId) : in.spellName,
                      spellSchool};
            break;
        }
        case T::INSTAKILL:
            if (!hasSpell) return {};
            event = "SPELL_INSTAKILL";
            prefix = spellPrefix();
            break;
        case T::EXTRA_ATTACKS:
            if (!hasSpell) return {};
            event = "SPELL_EXTRA_ATTACKS";
            prefix = spellPrefix();
            suffix = {amount};
            break;
        case T::AURA_BROKEN: {
            if (!hasSpell) return {};
            const char* auraType = in.powerType != 0 ? "DEBUFF" : "BUFF";
            prefix = spellPrefix();
            if (in.extraSpellId != 0) {
                event = "SPELL_AURA_BROKEN_SPELL";
                suffix = {std::to_string(in.extraSpellId),
                          in.extraSpellName.empty() ? "Spell " + std::to_string(in.extraSpellId)
                                                    : in.extraSpellName,
                          extraSchool,
                          auraType};
            } else {
                event = "SPELL_AURA_BROKEN";
                suffix = {auraType};
            }
            break;
        }
        default:
            return {};
    }

    const uint64_t effectiveSrcGuid = (environmental || in.srcGuid == 0) ? 0 : in.srcGuid;
    const uint64_t effectiveDstGuid = (destIsNone || in.dstGuid == 0) ? 0 : in.dstGuid;
    const uint32_t effectiveSrcFlags = effectiveSrcGuid == 0
        ? kNone
        : (in.srcFlags != 0 ? in.srcFlags : unitFlags(effectiveSrcGuid, in.playerGuid, in.petGuid));
    const uint32_t effectiveDstFlags = effectiveDstGuid == 0
        ? kNone
        : (in.dstFlags != 0 ? in.dstFlags : unitFlags(effectiveDstGuid, in.playerGuid, in.petGuid));

    std::vector<std::string> out;
    out.reserve(11 + prefix.size() + suffix.size());
    out.push_back(std::to_string(in.timestamp));
    out.push_back(event);
    out.push_back(guidText(effectiveSrcGuid));
    out.push_back(effectiveSrcGuid == 0 ? nil : textOrNil(in.srcName));
    out.push_back(std::to_string(effectiveSrcFlags));
    out.push_back(guidText(effectiveDstGuid));
    out.push_back(effectiveDstGuid == 0 ? nil : textOrNil(in.dstName));
    out.push_back(std::to_string(effectiveDstFlags));
    out.insert(out.end(), prefix.begin(), prefix.end());
    out.insert(out.end(), suffix.begin(), suffix.end());
    return out;
}

/// One C-side filter rule added via TBC/WotLK CombatLogAddFilter(events, srcFlags, dstFlags).
struct CombatLogFilterRule {
    enum class EndpointMode : uint8_t { Any = 0, Mask, Guid };

    bool matchAllEvents = true;
    std::unordered_set<std::string> events;
    EndpointMode srcMode = EndpointMode::Any;
    uint32_t srcMask = 0;
    uint64_t srcGuid = 0;
    EndpointMode dstMode = EndpointMode::Any;
    uint32_t dstMask = 0;
    uint64_t dstGuid = 0;

    void setEventCsv(std::string_view csv) {
        matchAllEvents = false;
        events.clear();
        size_t pos = 0;
        while (pos < csv.size()) {
            const size_t comma = csv.find(',', pos);
            std::string_view tok = (comma == std::string_view::npos)
                ? csv.substr(pos)
                : csv.substr(pos, comma - pos);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1);
            if (!tok.empty()) events.emplace(tok);
            if (comma == std::string_view::npos) break;
            pos = comma + 1;
        }
    }

    void setSrcMask(uint32_t mask) {
        srcMode = EndpointMode::Mask;
        srcMask = mask;
    }

    void setDstMask(uint32_t mask) {
        dstMode = EndpointMode::Mask;
        dstMask = mask;
    }

    static uint64_t parseGuidString(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        if (s.empty()) return 0;
        const std::string tmp(s);
        char* end = nullptr;
        return std::strtoull(tmp.c_str(), &end, 0);
    }

    void setSrcGuid(std::string_view guidStr) {
        srcMode = EndpointMode::Guid;
        srcGuid = parseGuidString(guidStr);
    }

    void setDstGuid(std::string_view guidStr) {
        dstMode = EndpointMode::Guid;
        dstGuid = parseGuidString(guidStr);
    }

    [[nodiscard]] bool matches(const std::string& subevent,
                               uint64_t evSrcGuid, uint32_t evSrcFlags,
                               uint64_t evDstGuid, uint32_t evDstFlags) const {
        if (!matchAllEvents && events.find(subevent) == events.end()) return false;
        if (srcMode == EndpointMode::Mask && !combat_log::objectIsA(evSrcFlags, srcMask)) return false;
        if (srcMode == EndpointMode::Guid && evSrcGuid != srcGuid) return false;
        if (dstMode == EndpointMode::Mask && !combat_log::objectIsA(evDstFlags, dstMask)) return false;
        if (dstMode == EndpointMode::Guid && evDstGuid != dstGuid) return false;
        return true;
    }
};

/// Active C-side filter set managed by CombatLogResetFilter() and CombatLogAddFilter().
/// An empty table passes all events (matching build 8606 right after CombatLogResetFilter()
/// and preserving unfiltered delivery when Blizzard_CombatLog is not loaded).
class CombatLogFilterTable {
public:
    void reset() { rules_.clear(); }
    void addRule(CombatLogFilterRule rule) { rules_.push_back(std::move(rule)); }
    [[nodiscard]] bool empty() const { return rules_.empty(); }

    [[nodiscard]] bool matchesParsed(const std::string& subevent,
                                     uint64_t srcGuid, uint32_t srcFlags,
                                     uint64_t dstGuid, uint32_t dstFlags) const {
        if (rules_.empty()) return true;
        for (const auto& rule : rules_) {
            if (rule.matches(subevent, srcGuid, srcFlags, dstGuid, dstFlags)) return true;
        }
        return false;
    }

    [[nodiscard]] bool matches(const std::vector<std::string>& args) const {
        if (rules_.empty()) return true;
        if (args.size() < 8) return false;
        const uint64_t srcGuid = CombatLogFilterRule::parseGuidString(args[2]);
        const uint32_t srcFlags = static_cast<uint32_t>(std::strtoll(args[4].c_str(), nullptr, 10));
        const uint64_t dstGuid = CombatLogFilterRule::parseGuidString(args[5]);
        const uint32_t dstFlags = static_cast<uint32_t>(std::strtoll(args[7].c_str(), nullptr, 10));
        return matchesParsed(args[1], srcGuid, srcFlags, dstGuid, dstFlags);
    }

private:
    std::vector<CombatLogFilterRule> rules_;
};

}  // namespace game
}  // namespace wowee
