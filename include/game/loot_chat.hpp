#pragma once

// The chat lines a TBC 2.4.3 client prints for group loot rolls and received
// items, and the decoding of the packets that cause them.
//
// The patterns are GlobalStrings.lua's (enUS, build 8606), copied verbatim:
// the client formats them itself and raises the result as CHAT_MSG_LOOT, so no
// FrameXML file names them. test_loot_chat compares every one against
// interface/framexml/globalstrings.lua.
//
// Example lines from a two-player roll on Aegis of the Scarlet Commander, as
// the original client shows them to the player who passed:
//   You passed on: [Aegis of the Scarlet Commander]
//   Graulberon has selected Greed for: [Aegis of the Scarlet Commander]
//   Greed Roll - 15 for [Aegis of the Scarlet Commander] by Graulberon
//   Graulberon won: [Aegis of the Scarlet Commander]
//   Graulberon receives loot: [Aegis of the Scarlet Commander].

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace game {
namespace loot_chat {

struct Pattern {
    const char* name;   ///< GlobalStrings key
    const char* text;   ///< its enUS value
};

inline constexpr Pattern LOOT_ROLL_ALL_PASSED{"LOOT_ROLL_ALL_PASSED", "Everyone passed on: %s"};
inline constexpr Pattern LOOT_ROLL_GREED{"LOOT_ROLL_GREED", "%s has selected Greed for: %s"};
inline constexpr Pattern LOOT_ROLL_GREED_SELF{"LOOT_ROLL_GREED_SELF", "You have selected Greed for: %s"};
inline constexpr Pattern LOOT_ROLL_NEED{"LOOT_ROLL_NEED", "%s has selected Need for: %s"};
inline constexpr Pattern LOOT_ROLL_NEED_SELF{"LOOT_ROLL_NEED_SELF", "You have selected Need for: %s"};
inline constexpr Pattern LOOT_ROLL_PASSED{"LOOT_ROLL_PASSED", "%s passed on: %s"};
inline constexpr Pattern LOOT_ROLL_PASSED_AUTO{"LOOT_ROLL_PASSED_AUTO",
    "%s automatically passed on: %s because he cannot loot that item."};
inline constexpr Pattern LOOT_ROLL_PASSED_AUTO_FEMALE{"LOOT_ROLL_PASSED_AUTO_FEMALE",
    "%s automatically passed on: %s because she cannot loot that item."};
inline constexpr Pattern LOOT_ROLL_PASSED_SELF{"LOOT_ROLL_PASSED_SELF", "You passed on: %s"};
inline constexpr Pattern LOOT_ROLL_PASSED_SELF_AUTO{"LOOT_ROLL_PASSED_SELF_AUTO",
    "You automatically passed on: %s because you cannot loot that item."};
inline constexpr Pattern LOOT_ROLL_ROLLED_GREED{"LOOT_ROLL_ROLLED_GREED", "Greed Roll - %d for %s by %s"};
inline constexpr Pattern LOOT_ROLL_ROLLED_NEED{"LOOT_ROLL_ROLLED_NEED", "Need Roll - %d for %s by %s"};
inline constexpr Pattern LOOT_ROLL_WON{"LOOT_ROLL_WON", "%s won: %s"};
inline constexpr Pattern LOOT_ROLL_WON_NO_SPAM_GREED{"LOOT_ROLL_WON_NO_SPAM_GREED",
    "%1$s won: %3$s |cff818181(Greed - %2$d)|r"};
inline constexpr Pattern LOOT_ROLL_WON_NO_SPAM_NEED{"LOOT_ROLL_WON_NO_SPAM_NEED",
    "%1$s won: %3$s |cff818181(Need - %2$d)|r"};
inline constexpr Pattern LOOT_ROLL_YOU_WON{"LOOT_ROLL_YOU_WON", "You won: %s"};
inline constexpr Pattern LOOT_ROLL_YOU_WON_NO_SPAM_GREED{"LOOT_ROLL_YOU_WON_NO_SPAM_GREED",
    "You won: %2$s |cff818181(Greed - %1$d)|r"};
inline constexpr Pattern LOOT_ROLL_YOU_WON_NO_SPAM_NEED{"LOOT_ROLL_YOU_WON_NO_SPAM_NEED",
    "You won: %2$s |cff818181(Need - %1$d)|r"};
inline constexpr Pattern LOOT_ITEM{"LOOT_ITEM", "%s receives loot: %s."};
inline constexpr Pattern LOOT_ITEM_MULTIPLE{"LOOT_ITEM_MULTIPLE", "%s receives loot: %sx%d."};
inline constexpr Pattern LOOT_ITEM_SELF{"LOOT_ITEM_SELF", "You receive loot: %s."};
inline constexpr Pattern LOOT_ITEM_SELF_MULTIPLE{"LOOT_ITEM_SELF_MULTIPLE", "You receive loot: %sx%d."};
inline constexpr Pattern LOOT_ITEM_PUSHED_SELF{"LOOT_ITEM_PUSHED_SELF", "You receive item: %s."};
inline constexpr Pattern LOOT_ITEM_PUSHED_SELF_MULTIPLE{"LOOT_ITEM_PUSHED_SELF_MULTIPLE",
    "You receive item: %sx%d."};
inline constexpr Pattern LOOT_ITEM_CREATED_SELF{"LOOT_ITEM_CREATED_SELF", "You create: %s."};
inline constexpr Pattern LOOT_ITEM_CREATED_SELF_MULTIPLE{"LOOT_ITEM_CREATED_SELF_MULTIPLE",
    "You create: %sx%d."};

inline constexpr Pattern kAllPatterns[] = {
    LOOT_ROLL_ALL_PASSED, LOOT_ROLL_GREED, LOOT_ROLL_GREED_SELF, LOOT_ROLL_NEED,
    LOOT_ROLL_NEED_SELF, LOOT_ROLL_PASSED, LOOT_ROLL_PASSED_AUTO,
    LOOT_ROLL_PASSED_AUTO_FEMALE, LOOT_ROLL_PASSED_SELF, LOOT_ROLL_PASSED_SELF_AUTO,
    LOOT_ROLL_ROLLED_GREED, LOOT_ROLL_ROLLED_NEED, LOOT_ROLL_WON,
    LOOT_ROLL_WON_NO_SPAM_GREED, LOOT_ROLL_WON_NO_SPAM_NEED, LOOT_ROLL_YOU_WON,
    LOOT_ROLL_YOU_WON_NO_SPAM_GREED, LOOT_ROLL_YOU_WON_NO_SPAM_NEED, LOOT_ITEM,
    LOOT_ITEM_MULTIPLE, LOOT_ITEM_SELF, LOOT_ITEM_SELF_MULTIPLE, LOOT_ITEM_PUSHED_SELF,
    LOOT_ITEM_PUSHED_SELF_MULTIPLE, LOOT_ITEM_CREATED_SELF, LOOT_ITEM_CREATED_SELF_MULTIPLE,
};

/// printf-style substitution of the conversions these patterns use: %s, %d
/// and their positional forms %N$s / %N$d. Arguments arrive already as text;
/// a %d argument must be a decimal integer. Anything else is a programming
/// error and asserts.
inline std::string format(const Pattern& pattern, const std::vector<std::string>& args) {
    const std::string_view p = pattern.text;
    std::string out;
    size_t nextArg = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] != '%') { out += p[i]; continue; }
        assert(i + 1 < p.size() && "dangling % in loot pattern");
        size_t j = i + 1;
        size_t argIndex = nextArg;
        if (j + 2 < p.size() && p[j] >= '1' && p[j] <= '9' && p[j + 1] == '$') {
            argIndex = static_cast<size_t>(p[j] - '1');
            j += 2;
        } else {
            ++nextArg;
        }
        const char conv = p[j];
        assert((conv == 's' || conv == 'd') && "unsupported conversion in loot pattern");
        assert(argIndex < args.size() && "loot pattern needs more arguments");
        if (conv == 'd') {
            assert(!args[argIndex].empty() &&
                   args[argIndex].find_first_not_of("-0123456789") == std::string::npos);
        }
        out += args[argIndex];
        i = j;
    }
    return out;
}

/// What one SMSG_LOOT_ROLL says about one player.
///
/// The (rollNumber, rollType) pairs are the ones CMaNGOS TBC writes in
/// GroupLootRoll::PlayerVote and SendLootRollWon:
///   (128, 128)  passed - also sent at the end for a player who never chose
///   (  0,   0)  selected Need
///   (128,   2)  selected Greed
///   (  n,   1)  the Need roll, n in 1..100, sent when the roll ends
///   (  n,   2)  the Greed roll, n in 1..100, sent when the roll ends
/// 128 is "chosen, not rolled yet" in rollNumber; it is not a roll result.
enum class RollEcho { Passed, NeedSelected, GreedSelected, NeedRolled, GreedRolled };

inline std::optional<RollEcho> decodeRollEcho(uint8_t rollNumber, uint8_t rollType) {
    if (rollNumber == 128 && rollType == 128) return RollEcho::Passed;
    if (rollNumber == 0 && rollType == 0) return RollEcho::NeedSelected;
    if (rollNumber == 128 && rollType == 2) return RollEcho::GreedSelected;
    if (rollNumber >= 1 && rollNumber <= 100 && rollType == 1) return RollEcho::NeedRolled;
    if (rollNumber >= 1 && rollNumber <= 100 && rollType == 2) return RollEcho::GreedRolled;
    return std::nullopt;
}

/// The line for one SMSG_LOOT_ROLL. `self` is whether the roller is the
/// player; `female` only matters for an automatic pass by someone else.
inline std::string rollEchoLine(RollEcho echo, uint8_t rollNumber, bool autoPass, bool self,
                                bool female, const std::string& name,
                                const std::string& itemLink) {
    switch (echo) {
        case RollEcho::Passed:
            if (self)
                return format(autoPass ? LOOT_ROLL_PASSED_SELF_AUTO : LOOT_ROLL_PASSED_SELF,
                              {itemLink});
            if (autoPass)
                return format(female ? LOOT_ROLL_PASSED_AUTO_FEMALE : LOOT_ROLL_PASSED_AUTO,
                              {name, itemLink});
            return format(LOOT_ROLL_PASSED, {name, itemLink});
        case RollEcho::NeedSelected:
            return self ? format(LOOT_ROLL_NEED_SELF, {itemLink})
                        : format(LOOT_ROLL_NEED, {name, itemLink});
        case RollEcho::GreedSelected:
            return self ? format(LOOT_ROLL_GREED_SELF, {itemLink})
                        : format(LOOT_ROLL_GREED, {name, itemLink});
        case RollEcho::NeedRolled:
            return format(LOOT_ROLL_ROLLED_NEED, {std::to_string(rollNumber), itemLink, name});
        case RollEcho::GreedRolled:
            return format(LOOT_ROLL_ROLLED_GREED, {std::to_string(rollNumber), itemLink, name});
    }
    assert(false && "unhandled RollEcho");
    return {};
}

/// The winner line of SMSG_LOOT_ROLL_WON. rollType is the winning vote,
/// 1 Need or 2 Greed. `detailed` is the showLootSpam CVar ("Detailed Loot
/// Information"); unchecked, the individual roll lines are hidden and the
/// winner line carries the roll instead.
inline std::string rollWonLine(uint8_t rollNumber, uint8_t rollType, bool self, bool detailed,
                               const std::string& name, const std::string& itemLink) {
    assert(rollType == 1 || rollType == 2);
    if (detailed)
        return self ? format(LOOT_ROLL_YOU_WON, {itemLink})
                    : format(LOOT_ROLL_WON, {name, itemLink});
    const std::string number = std::to_string(rollNumber);
    if (self)
        return format(rollType == 1 ? LOOT_ROLL_YOU_WON_NO_SPAM_NEED
                                    : LOOT_ROLL_YOU_WON_NO_SPAM_GREED,
                      {number, itemLink});
    return format(rollType == 1 ? LOOT_ROLL_WON_NO_SPAM_NEED : LOOT_ROLL_WON_NO_SPAM_GREED,
                  {name, number, itemLink});
}

/// How SMSG_ITEM_PUSH_RESULT says an item arrived. The flags are the
/// packet's `received` (0 looted, 1 from an NPC) and `created` words.
enum class ItemPushKind { Looted, Received, Created };

inline ItemPushKind itemPushKind(uint32_t received, uint32_t created) {
    if (created != 0) return ItemPushKind::Created;
    if (received != 0) return ItemPushKind::Received;
    return ItemPushKind::Looted;
}

/// The chat line of one SMSG_ITEM_PUSH_RESULT, or nothing when no
/// GlobalStrings line exists for it: another group member's item is only
/// announced for loot (LOOT_ITEM), which is the only push the server
/// broadcasts.
inline std::optional<std::string> itemPushLine(ItemPushKind kind, bool self, uint32_t count,
                                               const std::string& name,
                                               const std::string& itemLink) {
    assert(count >= 1);
    const bool many = count > 1;
    const std::string n = std::to_string(count);
    if (!self) {
        if (kind != ItemPushKind::Looted) return std::nullopt;
        return many ? format(LOOT_ITEM_MULTIPLE, {name, itemLink, n})
                    : format(LOOT_ITEM, {name, itemLink});
    }
    switch (kind) {
        case ItemPushKind::Looted:
            return many ? format(LOOT_ITEM_SELF_MULTIPLE, {itemLink, n})
                        : format(LOOT_ITEM_SELF, {itemLink});
        case ItemPushKind::Received:
            return many ? format(LOOT_ITEM_PUSHED_SELF_MULTIPLE, {itemLink, n})
                        : format(LOOT_ITEM_PUSHED_SELF, {itemLink});
        case ItemPushKind::Created:
            return many ? format(LOOT_ITEM_CREATED_SELF_MULTIPLE, {itemLink, n})
                        : format(LOOT_ITEM_CREATED_SELF, {itemLink});
    }
    assert(false && "unhandled ItemPushKind");
    return std::nullopt;
}

} // namespace loot_chat
} // namespace game
} // namespace wowee
