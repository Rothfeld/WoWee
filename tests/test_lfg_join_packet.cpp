// CMSG_LFG_JOIN's field widths and its slot list.
//
// Every field of this packet was wrong, and none of it reported anything. The
// server reads
//
//     uint32 Roles, uint8 NoPartialClear, uint8 Achievements,
//     uint8 slotCount, uint32 slots[], uint8 needsCount, uint8 needs[3],
//     string Comment
//
// while the client wrote the roles as a single byte and left the needs out
// altogether. The four bytes taken for Roles therefore swallowed the roles,
// both flags and the slot count; the slot list came out empty; and
// HandleLfgJoinOpcode returns without a word when it is - so every attempt to
// queue for a dungeon was discarded in silence.
#include <catch_amalgamated.hpp>

#include "test_support.hpp"
#include "game/world_packets.hpp"

using namespace wowee::game;

namespace {
}

TEST_CASE("CMSG_LFG_JOIN carries every slot at the server's widths",
          "[lfg][packet]") {
    SECTION("one dungeon") {
        auto p = LfgJoinPacket::build({285}, 3);
        const auto& b = p.getData();
        CHECK(wowee::test::readU32(b, 0) == 3u);      // Roles, four bytes
        CHECK(b[4] == 0);                // NoPartialClear
        CHECK(b[5] == 0);                // Achievements
        CHECK(b[6] == 1);                // slot count
        CHECK(wowee::test::readU32(b, 7) == 285u);    // the dungeon
        CHECK(b[11] == 3);               // needs count
        CHECK(b[12] == 0);
        CHECK(b[13] == 0);
        CHECK(b[14] == 0);
        CHECK(b[15] == 0);               // empty comment's terminator
        CHECK(b.size() == 16);
    }

    SECTION("several dungeons, all of them, in the order given") {
        auto p = LfgJoinPacket::build({258, 285, 300}, 7);
        const auto& b = p.getData();
        CHECK(wowee::test::readU32(b, 0) == 7u);
        CHECK(b[6] == 3);
        CHECK(wowee::test::readU32(b, 7) == 258u);
        CHECK(wowee::test::readU32(b, 11) == 285u);
        CHECK(wowee::test::readU32(b, 15) == 300u);
        CHECK(b[19] == 3);               // needs still follow the whole list
    }

    SECTION("the roles field is wide enough for every role bit") {
        // Tank, healer and damage together, which does not fit the byte this
        // used to send if anything above it were ever set.
        auto p = LfgJoinPacket::build({285}, 0x00000007);
        CHECK(wowee::test::readU32(p.getData(), 0) == 0x00000007u);
    }
}

#include "game/tbc_lfg.hpp"

TEST_CASE("TBC LFM results decode packed players and their party", "[lfg][tbc]") {
    wowee::network::Packet packet;
    packet.writeUInt32(1); packet.writeUInt32(12);
    packet.writeUInt32(1); packet.writeUInt32(1);
    packet.writePackedGuid(0x12340056);
    packet.writeUInt32(21); packet.writeUInt32(1519); packet.writeUInt8(1);
    packet.writeUInt32(0x0100000c); packet.writeUInt32(0x01000000); packet.writeUInt32(0x01000000);
    packet.writeString("Stockades group"); packet.writeUInt32(1);
    packet.writePackedGuid(0x1234); packet.writeUInt32(22);
    TbcLfgState state;
    readTbcLfgResults(packet, state);
    REQUIRE(state.results.size() == 1);
    CHECK(state.queryType == 1); CHECK(state.queryEntry == 12);
    CHECK(state.results[0].guid == 0x12340056);
    CHECK(state.results[0].level == 21); CHECK(state.results[0].zone == 1519);
    CHECK(state.results[0].lfm); CHECK(state.results[0].comment == "Stockades group");
    REQUIRE(state.results[0].members.size() == 1);
    CHECK(state.results[0].members[0].guid == 0x1234);
    CHECK(state.results[0].members[0].level == 22);
    CHECK(packet.getRemainingSize() == 0);
    const auto bytes = packet.getData();
    for (size_t size = 0; size < bytes.size(); ++size) {
        wowee::network::Packet truncated(0, std::vector<uint8_t>(bytes.begin(), bytes.begin() + size));
        CHECK_THROWS_AS(readTbcLfgResults(truncated, state), std::runtime_error);
        REQUIRE(state.results.size() == 1);
        CHECK(state.results[0].comment == "Stockades group");
    }
}

#include "game/packet_parsers.hpp"
TEST_CASE("TBC names include realm before uint32 race gender class", "[lfg][tbc]") {
    TbcPacketParsers parser;
    for (const std::string realm : {"", "Test Realm"}) {
        wowee::network::Packet packet;
        packet.writeUInt64(3); packet.writeString("Graulberto"); packet.writeString(realm);
        packet.writeUInt32(1); packet.writeUInt32(0); packet.writeUInt32(4); packet.writeUInt8(0);
        NameQueryResponseData data;
        REQUIRE(parser.parseNameQueryResponse(packet, data));
        CHECK(data.guid == 3); CHECK(data.name == "Graulberto"); CHECK(data.realmName == realm);
        CHECK(data.race == 1); CHECK(data.gender == 0); CHECK(data.classId == 4);
        CHECK(packet.getRemainingSize() == 0);
    }
}
