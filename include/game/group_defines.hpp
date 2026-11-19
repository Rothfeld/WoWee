#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/**
 * Party/Group member data
 */
struct GroupMember {
    std::string name;
    uint64_t guid = 0;
    uint8_t isOnline = 0;      // 0 = offline, 1 = online
    uint8_t subGroup = 0;      // Raid subgroup (0 for party)
    uint8_t flags = 0;         // Assistant, main tank, etc.
    uint8_t roles = 0;         // LFG roles (3.3.5a)

    // Party member stats (from SMSG_PARTY_MEMBER_STATS)
    uint32_t curHealth = 0;
    uint32_t maxHealth = 0;
    uint8_t powerType = 0;
    uint16_t curPower = 0;
    uint16_t maxPower = 0;
    uint16_t level = 0;
    uint16_t zoneId = 0;
    int16_t posX = 0;
    int16_t posY = 0;
    uint16_t onlineStatus = 0;   // GROUP_UPDATE_FLAG_STATUS bitmask
    bool hasPartyStats = false;  // true once we've received stats
    uint64_t petGuid = 0;        // GROUP_UPDATE_FLAG_PET_GUID, 0 for no pet
};

/**
 * Full group/party data from SMSG_GROUP_LIST
 */
/// What a loot method is called, as it is written to chat.
///
/// Five, not the three the field's comment used to name. The party window and
/// the /loot command each wrote the five out; a parallel table of lowercase
/// tokens lives in lua_api_helpers.hpp for parsing the command's argument, and
/// the two are in the same order on purpose - the token's index is the wire
/// value this names.
inline const char* lootMethodName(uint8_t method) {
    static constexpr const char* kByMethod[] = {
        "Free for All", "Round Robin", "Master Looter",
        "Group Loot", "Need Before Greed",
    };
    return method < 5 ? kByMethod[method] : "Unknown";
}

/// What an instance difficulty is called, or null for a value that is not one.
///
/// Three places wrote the four words out, and they disagreed about the answer
/// for a value outside the four: two said "Normal" and one said "Unknown".
/// Null here so each keeps its own - the Lua bindings hand this to FrameXML,
/// which compares it against names, while the minimap is only labelling a
/// corner and can say it does not know.
inline const char* instanceDifficultyName(uint32_t difficulty) {
    static constexpr const char* kByDifficulty[] = {
        "Normal", "Heroic", "25 Normal", "25 Heroic",
    };
    return difficulty < 4 ? kByDifficulty[difficulty] : nullptr;
}

/// How many raid target markers there are.
///
/// GameHandler and SocialHandler each declared their own kRaidMarkCount and
/// the icon loader a third, kRaidTargetIconCount. All three are the count of
/// the marks the server indexes, so it is written here and the others name it.
inline constexpr uint32_t kRaidMarkCount = 8;

/// What a raid target marker is called, or null for a value that is not one.
///
/// Written out capitalised for the minimap's tooltip and lowercase for what
/// /mark matches, plus a third time inside that command's help line - one set
/// of eight words under two spellings. The display spelling lives here and
/// the command lowercases what it compares against it.
inline const char* raidMarkName(uint8_t mark) {
    static constexpr const char* kByMark[] = {
        "Star", "Circle", "Diamond", "Triangle",
        "Moon", "Square", "Cross", "Skull",
    };
    return mark < kRaidMarkCount ? kByMark[mark] : nullptr;
}

/// Whether a group's type says raid.
///
/// The field changed meaning between expansions and the client tested it as a
/// number. 1.12 writes 0 for a party and 1 for a raid; 3.3.5 writes a set of
/// flags - 0x01 battleground, 0x02 raid, 0x04 and 0x08 for the dungeon finder -
/// so `== 1` read a WotLK raid as no raid at all, and every test for a party
/// written as `== 0` read a finder group (0x0C) as no party either. A dungeon
/// group had neither kind of frame.
///
/// The low two bits cover both conventions: 1.12's raid is 1, WotLK's is 2, and
/// a battleground group is a raid as well. The finder's bits mean neither.
inline bool groupIsRaid(uint8_t groupType) {
    return (groupType & 0x03) != 0;
}

struct GroupListData {
    uint8_t groupType = 0;       // 0 = party, 1 = raid
    uint8_t subGroup = 0;
    uint8_t flags = 0;
    uint8_t roles = 0;
    uint8_t lootMethod = 0;      ///< 0..4; see lootMethodName
    uint64_t looterGuid = 0;
    uint8_t lootThreshold = 0;
    uint8_t difficultyId = 0;
    uint8_t raidDifficultyId = 0;
    uint32_t memberCount = 0;
    std::vector<GroupMember> members;
    uint64_t leaderGuid = 0;

    [[nodiscard]] bool isValid() const { return true; }
    [[nodiscard]] bool isEmpty() const { return memberCount == 0; }
};

/// The raid roster as build 8606 numbers it: raid1..raidN-1 are the other
/// members in the order SMSG_GROUP_LIST lists them, and the player is always
/// last, raidN.
///
/// Measured on a pair (_experiments/20261118T000639Z.raid_roster): with the
/// server's slots Edrikc, Edricc, Aedricc, Edrikc's client numbers Edricc,
/// Aedricc, Edrikc - neither name nor guid order, and the player moves to the
/// end when someone joins. Subgroup moves and leader changes do not reorder.
/// The list never contains the player; it is skipped here anyway so a
/// malformed packet cannot number them twice. Empty when not in a raid.
inline std::vector<uint64_t> raidRosterGuids(const GroupListData& group, uint64_t playerGuid) {
    std::vector<uint64_t> guids;
    if (!groupIsRaid(group.groupType) || group.isEmpty()) return guids;
    guids.reserve(group.members.size() + 1);
    for (const auto& member : group.members)
        if (member.guid != playerGuid) guids.push_back(member.guid);
    guids.push_back(playerGuid);
    return guids;
}

/// One-based raid index of `guid` in raidRosterGuids order, 0 when it is not
/// in the raid.
inline int raidRosterIndex(const GroupListData& group, uint64_t playerGuid, uint64_t guid) {
    if (guid == 0) return 0;
    const auto guids = raidRosterGuids(group, playerGuid);
    for (size_t i = 0; i < guids.size(); ++i)
        if (guids[i] == guid) return static_cast<int>(i) + 1;
    return 0;
}

/// The group unit tokens `guid` answers to, as an event's unit argument:
/// "raidN" in a raid, and "partyN" whenever it shares the player's party -
/// in a raid that is the player's own subgroup, as resolveUnitGuid counts
/// "party1". PartyMemberFrame listens for "partyN" alone, raid or not; firing
/// only "raidN" in a raid left the party frame showing a member alive and
/// carrying old debuffs after death (Serpentshrine, raid of two).
inline std::vector<std::string> groupUnitTokens(const GroupListData& group, uint64_t playerGuid,
                                                uint64_t guid) {
    std::vector<std::string> tokens;
    if (guid == 0 || guid == playerGuid) return tokens;
    const bool raid = groupIsRaid(group.groupType);
    if (raid) {
        if (const int index = raidRosterIndex(group, playerGuid, guid))
            tokens.push_back("raid" + std::to_string(index));
    }
    int found = 0;
    for (const auto& m : group.members) {
        if (m.guid == playerGuid) continue;
        if (raid && m.subGroup != group.subGroup) continue;
        ++found;
        if (m.guid == guid) {
            if (found <= 4) tokens.push_back("party" + std::to_string(found));
            break;
        }
    }
    return tokens;
}

/**
 * Party command types
 */
enum class PartyCommand : uint32_t {
    INVITE = 0,
    UNINVITE = 1,
    LEAVE = 2,
    // 3 is not an operation. The server's PARTY_OP_SWAP is 4, and SWAP was
    // declared 3, so the operation field of SMSG_PARTY_COMMAND_RESULT could
    // never be recognised as a swap.
    SWAP = 4
};

/**
 * Party command result codes
 */
enum class PartyResult : uint32_t {
    OK = 0,
    BAD_PLAYER_NAME = 1,
    TARGET_NOT_IN_GROUP = 2,
    TARGET_NOT_IN_INSTANCE = 3,
    GROUP_FULL = 4,
    ALREADY_IN_GROUP = 5,
    NOT_IN_GROUP = 6,
    NOT_LEADER = 7,
    PLAYER_WRONG_FACTION = 8,
    IGNORING_YOU = 9,
    LFG_PENDING = 12,
    INVITE_RESTRICTED = 13
};

} // namespace game
} // namespace wowee
