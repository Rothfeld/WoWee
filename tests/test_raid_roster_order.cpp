// Raid indices as build 8606 numbers them: the other members in the order
// SMSG_GROUP_LIST lists them, then the player last.
//
// Measured on a pair (_experiments/20261118T000639Z.raid_roster in the
// workspace): server slots Edrikc (leader), Edricc, Aedricc; Edrikc's client
// answered raid1 Edricc, raid2 Aedricc, raid3 Edrikc - neither name order
// (Aedricc first) nor guid order (Edrikc 0x15 before Aedricc 0x20).
#include <catch_amalgamated.hpp>

#include "game/group_defines.hpp"

using namespace wowee::game;

namespace {

GroupListData raid(std::initializer_list<uint64_t> others) {
    GroupListData group;
    group.groupType = 1;
    for (uint64_t guid : others) {
        GroupMember member;
        member.guid = guid;
        group.members.push_back(member);
    }
    group.memberCount = static_cast<uint32_t>(group.members.size()) + 1;
    return group;
}

}  // namespace

TEST_CASE("raid roster lists the others in roster order and the player last", "[raid]") {
    constexpr uint64_t edricc = 0x14, edrikc = 0x15, aedricc = 0x20;
    const auto group = raid({edricc, aedricc});
    REQUIRE(raidRosterGuids(group, edrikc) == std::vector<uint64_t>{edricc, aedricc, edrikc});
    CHECK(raidRosterIndex(group, edrikc, edricc) == 1);
    CHECK(raidRosterIndex(group, edrikc, aedricc) == 2);
    CHECK(raidRosterIndex(group, edrikc, edrikc) == 3);
    CHECK(raidRosterIndex(group, edrikc, 0x99) == 0);
    CHECK(raidRosterIndex(group, edrikc, 0) == 0);
}

TEST_CASE("a party has no raid roster", "[raid]") {
    auto group = raid({0x14});
    group.groupType = 0;
    CHECK(raidRosterGuids(group, 0x15).empty());
    CHECK(raidRosterIndex(group, 0x15, 0x15) == 0);
    CHECK(raidRosterGuids(GroupListData{}, 0x15).empty());
}
