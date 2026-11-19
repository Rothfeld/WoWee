#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace wowee::game {

/// One macro as the interface edits it.
struct Macro {
    std::string text;
    std::string name;
    std::string icon;
};

/// The same macro: equal text and name, icon path compared without case
/// (an older icon list handed out lower-case paths).
bool sameMacro(const Macro& a, const Macro& b);

/// Macros by identity: account macros n, character macros 0x01000000 | n
/// (native 2.4.3 macros-cache.txt: MACRO 2 "GenB", MACRO 16777217 "ChrA").
using MacroSet = std::map<uint32_t, Macro>;

inline constexpr uint32_t kCharacterMacroFlag = 0x01000000u;

inline bool isCharacterMacroId(uint32_t id) { return (id & kCharacterMacroFlag) != 0; }

/// Number of macros in one half.
size_t macroHalfSize(const MacroSet& macros, bool character);

/// The id a new macro of that half gets: one past the highest in use.
/// Native 2.4.3 with GenA 1 and GenB 2: deleting GenA and creating GenC
/// stored GenC as 3.
uint32_t nextMacroId(const MacroSet& macros, bool character);

/// The id of a "macro_<id>_text|name|icon" config key, 0 for any other key.
uint32_t macroKeyId(const std::string& key);

/// Reads one config line's key and value into the set; false when the key is
/// not a macro key. Text values carry newlines as \n and backslashes as \\.
bool readMacroKey(const std::string& key, const std::string& value, MacroSet& into);

/// Writes every macro as config keys, ascending by id.
void writeMacroKeys(std::ostream& out, const MacroSet& macros);

/// What happened to one account macro found in a character's config.
struct MacroMergeStep {
    enum class Outcome {
        Present,   ///< the account already holds it under the same id
        Added,     ///< taken into the account under its id
        Joined,    ///< the account holds the same macro under another id
        Moved,     ///< its id held a different macro; it got a new id
        Deferred,  ///< left in the character's config
    };
    uint32_t from = 0;
    uint32_t to = 0;   ///< the id it is known by now; 0 when deferred
    Outcome outcome = Outcome::Deferred;
};

/// Merges the account macros found in one character's config into the
/// account's set, in ascending id order.
///
/// A macro the account holds identically under its own id, or whose id is free,
/// keeps its id: every button of every character naming it keeps working.
/// A macro that cannot keep its id (another macro holds it, the same macro sits
/// under another id, or the account half is full) changes id only when
/// `ownCharacter` is given - that character is logging in, so its buttons can
/// follow. It then joins the identical macro, or takes the next account id, or
/// when the account half is full the next id of `ownCharacter`'s half. With no
/// room anywhere, or without `ownCharacter`, it is deferred.
std::vector<MacroMergeStep> mergeAccountMacros(MacroSet& account, const MacroSet& found,
                                               uint32_t accountSlots, uint32_t characterSlots,
                                               MacroSet* ownCharacter);

const char* macroMergeOutcomeName(MacroMergeStep::Outcome outcome);

}  // namespace wowee::game
