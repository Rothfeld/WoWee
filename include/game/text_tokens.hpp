#pragma once

/**
 * text_tokens.hpp - the $-tokens WoW's server text is written with.
 *
 * Gossip, a quest giver's greeting, quest text and a book's page arrive from
 * the server with the reader left as a blank: "$N, I need your help" and
 * "$gsir:madam;". The 2.4.3 client fills them in where its Lua getters hand
 * the text out - GetGossipText, GetGreetingText, GetQuestText,
 * GetObjectiveText, GetProgressText, GetRewardText, GetQuestLogQuestText and
 * ItemTextGetText. Titles, gossip options, quest log objective lines and NPC
 * chat keep their tokens: native prints "$N" in those as the two characters.
 *
 * The rule, measured on build 8606 (_experiments/20261118T203000Z.text_tokens):
 *
 *   one pass, left to right; "$", optional decimal digits, one letter:
 *   $n $N  name                  $c / $C  class, lower-cased / as UnitClass
 *   $b $B  "\n"                  $r / $R  race,  lower-cased / as UnitRace
 *   $g $G  male:female;          $t / $T  honour rank name, lower-cased / not,
 *                                         taking a male:female; it ignores
 *   $<id>w $<id>W  world state <id>'s value, 0 when the client has none
 *   $d $D  "0 |4minute:minutes;"  $e $E  "0"   (see resolveTextTokens)
 *
 * Any other letter, or none, leaves the "$" in place and carries on at the
 * character after it, dropping only digits that were read: "$$n" reads
 * "$Edrik", "$1x" reads "$x", "$o" stays "$o".
 */

#include <cstdint>
#include <functional>
#include <string>

namespace wowee {
namespace game {

class GameHandler;

/// Everything the substitution needs to know about the reader.
struct TextSubject {
    std::string name;
    std::string className;   // UnitClass("player"), first value
    std::string raceName;    // UnitRace("player"), first value
    bool female = false;
    std::string pvpRankName; // what $T writes: PVP_RANK_<rank>_<team>[_FEMALE]
    /// The value of a world state, for $<id>w. Empty: every state reads 0.
    std::function<uint32_t(uint32_t)> worldState;
};

/// The substitution itself.
std::string resolveTextTokens(const std::string& text, const TextSubject& subject);

/// The logged-in character: name, class, race, gender and world states.
/// pvpRankName comes from the interface's global strings, which the game
/// layer does not hold; the Lua getters fill it in (lua_api_helpers).
TextSubject textSubjectFor(const GameHandler& gameHandler);

}  // namespace game
}  // namespace wowee
