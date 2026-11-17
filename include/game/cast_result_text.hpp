#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::game {

// Which GlobalStrings line a spell cast failure shows. The interface's own
// FrameXML/GlobalStrings.lua holds every text; this names the line, the
// expansion's file supplies the words. The original shows them only on the
// red error line (UI_ERROR_MESSAGE, UIErrorsFrame colour 1.0/0.1/0.1), never
// in chat; Riposte unavailable reads "You can't do that yet"
// (SPELL_FAILED_CASTER_AURASTATE). Observed on 2.4.3 build 8606:
// _experiments/20261117T182446Z.pair7_cast_errors/.
//
// Index: the WotLK-numbered SpellCastResult every parser normalises to
// (AzerothCore SharedDefines.h; 188..191 keep TBC-only meanings, see
// tbc_cast_results.hpp). Each name is the enum name, which is the GlobalStrings
// key. nullptr: no text - SUCCESS, and DONT_REPORT, which exists to say
// nothing.
inline constexpr std::array<const char*, 192> kCastResultGlobalNames = {
    nullptr,                                   // 0 SUCCESS
    "SPELL_FAILED_AFFECTING_COMBAT",
    "SPELL_FAILED_ALREADY_AT_FULL_HEALTH",
    "SPELL_FAILED_ALREADY_AT_FULL_MANA",
    "SPELL_FAILED_ALREADY_AT_FULL_POWER",
    "SPELL_FAILED_ALREADY_BEING_TAMED",        // 5
    "SPELL_FAILED_ALREADY_HAVE_CHARM",
    "SPELL_FAILED_ALREADY_HAVE_SUMMON",
    "SPELL_FAILED_ALREADY_OPEN",
    "SPELL_FAILED_AURA_BOUNCED",
    "SPELL_FAILED_AUTOTRACK_INTERRUPTED",      // 10
    "SPELL_FAILED_BAD_IMPLICIT_TARGETS",
    "SPELL_FAILED_BAD_TARGETS",
    "SPELL_FAILED_CANT_BE_CHARMED",
    "SPELL_FAILED_CANT_BE_DISENCHANTED",
    "SPELL_FAILED_CANT_BE_DISENCHANTED_SKILL", // 15
    "SPELL_FAILED_CANT_BE_MILLED",
    "SPELL_FAILED_CANT_BE_PROSPECTED",
    "SPELL_FAILED_CANT_CAST_ON_TAPPED",
    "SPELL_FAILED_CANT_DUEL_WHILE_INVISIBLE",
    "SPELL_FAILED_CANT_DUEL_WHILE_STEALTHED",  // 20
    "SPELL_FAILED_CANT_STEALTH",
    "SPELL_FAILED_CASTER_AURASTATE",
    "SPELL_FAILED_CASTER_DEAD",
    "SPELL_FAILED_CHARMED",
    "SPELL_FAILED_CHEST_IN_USE",               // 25
    "SPELL_FAILED_CONFUSED",
    nullptr,                                   // 27 DONT_REPORT
    "SPELL_FAILED_EQUIPPED_ITEM",
    "SPELL_FAILED_EQUIPPED_ITEM_CLASS",
    "SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND", // 30
    "SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND",
    "SPELL_FAILED_ERROR",
    "SPELL_FAILED_FIZZLE",
    "SPELL_FAILED_FLEEING",
    "SPELL_FAILED_FOOD_LOWLEVEL",              // 35
    "SPELL_FAILED_HIGHLEVEL",
    "SPELL_FAILED_HUNGER_SATIATED",
    "SPELL_FAILED_IMMUNE",
    "SPELL_FAILED_INCORRECT_AREA",
    "SPELL_FAILED_INTERRUPTED",                // 40
    "SPELL_FAILED_INTERRUPTED_COMBAT",
    "SPELL_FAILED_ITEM_ALREADY_ENCHANTED",
    "SPELL_FAILED_ITEM_GONE",
    "SPELL_FAILED_ITEM_NOT_FOUND",
    "SPELL_FAILED_ITEM_NOT_READY",             // 45
    "SPELL_FAILED_LEVEL_REQUIREMENT",
    "SPELL_FAILED_LINE_OF_SIGHT",
    "SPELL_FAILED_LOWLEVEL",
    "SPELL_FAILED_LOW_CASTLEVEL",
    "SPELL_FAILED_MAINHAND_EMPTY",             // 50
    "SPELL_FAILED_MOVING",
    "SPELL_FAILED_NEED_AMMO",
    "SPELL_FAILED_NEED_AMMO_POUCH",
    "SPELL_FAILED_NEED_EXOTIC_AMMO",
    "SPELL_FAILED_NEED_MORE_ITEMS",            // 55
    "SPELL_FAILED_NOPATH",
    "SPELL_FAILED_NOT_BEHIND",
    "SPELL_FAILED_NOT_FISHABLE",
    "SPELL_FAILED_NOT_FLYING",
    "SPELL_FAILED_NOT_HERE",                   // 60
    "SPELL_FAILED_NOT_INFRONT",
    "SPELL_FAILED_NOT_IN_CONTROL",
    "SPELL_FAILED_NOT_KNOWN",
    "SPELL_FAILED_NOT_MOUNTED",
    "SPELL_FAILED_NOT_ON_TAXI",                // 65
    "SPELL_FAILED_NOT_ON_TRANSPORT",
    "SPELL_FAILED_NOT_READY",
    "SPELL_FAILED_NOT_SHAPESHIFT",
    "SPELL_FAILED_NOT_STANDING",
    "SPELL_FAILED_NOT_TRADEABLE",              // 70
    "SPELL_FAILED_NOT_TRADING",
    "SPELL_FAILED_NOT_UNSHEATHED",
    "SPELL_FAILED_NOT_WHILE_GHOST",
    "SPELL_FAILED_NOT_WHILE_LOOTING",
    "SPELL_FAILED_NO_AMMO",                    // 75
    "SPELL_FAILED_NO_CHARGES_REMAIN",
    "SPELL_FAILED_NO_CHAMPION",
    "SPELL_FAILED_NO_COMBO_POINTS",
    "SPELL_FAILED_NO_DUELING",
    "SPELL_FAILED_NO_ENDURANCE",               // 80
    "SPELL_FAILED_NO_FISH",
    "SPELL_FAILED_NO_ITEMS_WHILE_SHAPESHIFTED",
    "SPELL_FAILED_NO_MOUNTS_ALLOWED",
    "SPELL_FAILED_NO_PET",
    "SPELL_FAILED_NO_POWER",                   // 85, see noPowerGlobalName
    "SPELL_FAILED_NOTHING_TO_DISPEL",
    "SPELL_FAILED_NOTHING_TO_STEAL",
    "SPELL_FAILED_ONLY_ABOVEWATER",
    "SPELL_FAILED_ONLY_DAYTIME",
    "SPELL_FAILED_ONLY_INDOORS",               // 90
    "SPELL_FAILED_ONLY_MOUNTED",
    "SPELL_FAILED_ONLY_NIGHTTIME",
    "SPELL_FAILED_ONLY_OUTDOORS",
    "SPELL_FAILED_ONLY_SHAPESHIFT",
    "SPELL_FAILED_ONLY_STEALTHED",             // 95
    "SPELL_FAILED_ONLY_UNDERWATER",
    "SPELL_FAILED_OUT_OF_RANGE",
    "SPELL_FAILED_PACIFIED",
    "SPELL_FAILED_POSSESSED",
    "SPELL_FAILED_REAGENTS",                   // 100
    "SPELL_FAILED_REQUIRES_AREA",
    "SPELL_FAILED_REQUIRES_SPELL_FOCUS",
    "SPELL_FAILED_ROOTED",
    "SPELL_FAILED_SILENCED",
    "SPELL_FAILED_SPELL_IN_PROGRESS",          // 105
    "SPELL_FAILED_SPELL_LEARNED",
    "SPELL_FAILED_SPELL_UNAVAILABLE",
    "SPELL_FAILED_STUNNED",
    "SPELL_FAILED_TARGETS_DEAD",
    "SPELL_FAILED_TARGET_AFFECTING_COMBAT",    // 110
    "SPELL_FAILED_TARGET_AURASTATE",
    "SPELL_FAILED_TARGET_DUELING",
    "SPELL_FAILED_TARGET_ENEMY",
    "SPELL_FAILED_TARGET_ENRAGED",
    "SPELL_FAILED_TARGET_FRIENDLY",            // 115
    "SPELL_FAILED_TARGET_IN_COMBAT",
    "SPELL_FAILED_TARGET_IS_PLAYER",
    "SPELL_FAILED_TARGET_IS_PLAYER_CONTROLLED",
    "SPELL_FAILED_TARGET_NOT_DEAD",
    "SPELL_FAILED_TARGET_NOT_IN_PARTY",        // 120
    "SPELL_FAILED_TARGET_NOT_LOOTED",
    "SPELL_FAILED_TARGET_NOT_PLAYER",
    "SPELL_FAILED_TARGET_NO_POCKETS",
    "SPELL_FAILED_TARGET_NO_WEAPONS",
    "SPELL_FAILED_TARGET_NO_RANGED_WEAPONS",   // 125
    "SPELL_FAILED_TARGET_UNSKINNABLE",
    "SPELL_FAILED_THIRST_SATIATED",
    "SPELL_FAILED_TOO_CLOSE",
    "SPELL_FAILED_TOO_MANY_OF_ITEM",
    "SPELL_FAILED_TOTEM_CATEGORY",             // 130
    "SPELL_FAILED_TOTEMS",
    "SPELL_FAILED_TRY_AGAIN",
    "SPELL_FAILED_UNIT_NOT_BEHIND",
    "SPELL_FAILED_UNIT_NOT_INFRONT",
    "SPELL_FAILED_WRONG_PET_FOOD",             // 135
    "SPELL_FAILED_NOT_WHILE_FATIGUED",
    "SPELL_FAILED_TARGET_NOT_IN_INSTANCE",
    "SPELL_FAILED_NOT_WHILE_TRADING",
    "SPELL_FAILED_TARGET_NOT_IN_RAID",
    "SPELL_FAILED_TARGET_FREEFORALL",          // 140
    "SPELL_FAILED_NO_EDIBLE_CORPSES",
    "SPELL_FAILED_ONLY_BATTLEGROUNDS",
    "SPELL_FAILED_TARGET_NOT_GHOST",
    "SPELL_FAILED_TRANSFORM_UNUSABLE",
    "SPELL_FAILED_WRONG_WEATHER",              // 145
    "SPELL_FAILED_DAMAGE_IMMUNE",
    "SPELL_FAILED_PREVENTED_BY_MECHANIC",
    "SPELL_FAILED_PLAY_TIME",
    "SPELL_FAILED_REPUTATION",
    "SPELL_FAILED_MIN_SKILL",                  // 150
    "SPELL_FAILED_NOT_IN_ARENA",
    "SPELL_FAILED_NOT_ON_SHAPESHIFT",
    "SPELL_FAILED_NOT_ON_STEALTHED",
    "SPELL_FAILED_NOT_ON_DAMAGE_IMMUNE",
    "SPELL_FAILED_NOT_ON_MOUNTED",             // 155
    "SPELL_FAILED_TOO_SHALLOW",
    "SPELL_FAILED_TARGET_NOT_IN_SANCTUARY",
    "SPELL_FAILED_TARGET_IS_TRIVIAL",
    "SPELL_FAILED_BM_OR_INVISGOD",
    "SPELL_FAILED_EXPERT_RIDING_REQUIREMENT",  // 160
    "SPELL_FAILED_ARTISAN_RIDING_REQUIREMENT",
    "SPELL_FAILED_NOT_IDLE",
    "SPELL_FAILED_NOT_INACTIVE",
    "SPELL_FAILED_PARTIAL_PLAYTIME",
    "SPELL_FAILED_NO_PLAYTIME",                // 165
    "SPELL_FAILED_NOT_IN_BATTLEGROUND",
    "SPELL_FAILED_NOT_IN_RAID_INSTANCE",
    "SPELL_FAILED_ONLY_IN_ARENA",
    "SPELL_FAILED_TARGET_LOCKED_TO_RAID_INSTANCE",
    "SPELL_FAILED_ON_USE_ENCHANT",             // 170
    "SPELL_FAILED_NOT_ON_GROUND",
    "SPELL_FAILED_CUSTOM_ERROR",
    "SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW",
    "SPELL_FAILED_TOO_MANY_SOCKETS",
    "SPELL_FAILED_INVALID_GLYPH",              // 175
    "SPELL_FAILED_UNIQUE_GLYPH",
    "SPELL_FAILED_GLYPH_SOCKET_LOCKED",
    "SPELL_FAILED_NO_VALID_TARGETS",
    "SPELL_FAILED_ITEM_AT_MAX_CHARGES",
    "SPELL_FAILED_NOT_IN_BARBERSHOP",          // 180
    "SPELL_FAILED_FISHING_TOO_LOW",
    "SPELL_FAILED_ITEM_ENCHANT_TRADE_WINDOW",
    "SPELL_FAILED_SUMMON_PENDING",
    "SPELL_FAILED_MAX_SOCKETS",
    "SPELL_FAILED_PET_CAN_RENAME",             // 185
    "SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED",
    "SPELL_FAILED_UNKNOWN",
    "SPELL_FAILED_TRAINING_POINTS",            // 188..191: TBC only
    "SPELL_FAILED_DISENCHANT_WHILE_LOOTING",
    "SPELL_FAILED_PROSPECT_WHILE_LOOTING",
    "SPELL_FAILED_TOO_MANY_SKILLS",
};

/// GlobalStrings key for a normalised SpellCastResult, nullptr for none.
///
/// TBC's GlobalStrings names three lines differently from the shared enum:
/// the full-health/full-mana refusals carry an ERR_ prefix and the
/// prospecting shortfall is PROSPECT_NEED_MORE ("Requires %d %s."), the only
/// line in the file for that failure. Not yet observed on the original.
inline const char* castResultGlobalName(uint8_t result, bool tbc) {
    if (tbc) {
        switch (result) {
            case 2:  return "ERR_SPELL_FAILED_ALREADY_AT_FULL_HEALTH";
            case 3:  return "ERR_SPELL_FAILED_ALREADY_AT_FULL_MANA";
            case 55: return "SPELL_FAILED_PROSPECT_NEED_MORE";
            default: break;
        }
    }
    return result < kCastResultGlobalNames.size() ? kCastResultGlobalNames[result] : nullptr;
}

/// NO_POWER has no line of its own: the text names the spell's resource.
/// Original 2.4.3, Sinister Strike without energy: "Not enough energy".
/// Spell.dbc PowerType: 0 mana, 1 rage, 2 focus, 3 energy, -2 (254 as a
/// byte) health. nullptr: a resource GlobalStrings has no line for
/// (happiness).
inline const char* noPowerGlobalName(uint8_t powerType) {
    switch (powerType) {
        case 0:   return "ERR_OUT_OF_MANA";
        case 1:   return "ERR_OUT_OF_RAGE";
        case 2:   return "ERR_OUT_OF_FOCUS";
        case 3:   return "ERR_OUT_OF_ENERGY";
        case 254: return "ERR_OUT_OF_HEALTH";
        default:  return nullptr;
    }
}

/// Lua string.format over a GlobalStrings pattern, for the %s/%d conversions
/// those patterns use. Arguments are already text; each conversion takes the
/// next one. "%%" is a literal percent. Empty when the count does not match
/// or the pattern uses another conversion.
inline std::string formatGlobalString(std::string_view pattern,
                                      const std::vector<std::string>& args) {
    std::string out;
    auto next = args.begin();
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 == pattern.size()) {
            out += pattern[i];
            continue;
        }
        const char conversion = pattern[++i];
        if (conversion == '%') {
            out += '%';
            continue;
        }
        if (conversion != 's' && conversion != 'd') return {};
        if (next == args.end()) return {};
        out += *next++;
    }
    return next == args.end() ? out : std::string{};
}

/// True when the pattern still needs arguments.
inline bool globalStringHasConversions(std::string_view pattern) {
    for (size_t i = 0; i + 1 < pattern.size(); ++i) {
        if (pattern[i] != '%') continue;
        if (pattern[i + 1] == '%') { ++i; continue; }
        return true;
    }
    return false;
}

} // namespace wowee::game
