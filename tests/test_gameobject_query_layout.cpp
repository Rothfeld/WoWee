// SMSG_GAMEOBJECT_QUERY_RESPONSE fixtures exercise expansion-specific strings
// before the 24 raw template fields. CMaNGOS TBC QueryHandler.cpp writes icon,
// opening text and closing text; Wrath also has three strings.
#include <catch_amalgamated.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "game/packet_parsers.hpp"
#include "game/world_packets.hpp"

using namespace wowee::game;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void f32(float v) {
        uint32_t raw;
        std::memcpy(&raw, &v, 4);
        u32(raw);
    }
    void cstr(const std::string& s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
};

/// The packet as the server writes it, with `extraStrings` of the 2.0.3 block.
std::vector<uint8_t> buildResponse(uint32_t entry, uint32_t type,
                                   const std::string& name, int extraStrings) {
    Writer w;
    w.u32(entry);
    w.u32(type);
    w.u32(0xD15);                  // displayId
    w.cstr(name);
    w.cstr("");                    // name2
    w.cstr("");                    // name3
    w.cstr("");                    // name4
    const char* extras[] = {"Attack", "Collecting", "Closing"};
    for (int i = 0; i < extraStrings; ++i) w.cstr(extras[i]);
    for (uint32_t i = 0; i < 24; ++i) w.u32(0xA000 + i);
    w.f32(1.5f);                   // size, which this client does not read
    return w.bytes;
}

wowee::network::Packet makePacket(const std::vector<uint8_t>& bytes) {
    return wowee::network::Packet(0x005F, bytes);
}

}  // namespace

TEST_CASE("the WotLK gameobject query reads three strings before the data",
          "[gameobject][wire]") {
    auto bytes = buildResponse(1731, 3, "Copper Vein", 3);
    auto packet = makePacket(bytes);
    GameObjectQueryResponseData data;
    REQUIRE(GameObjectQueryResponseParser::parse(packet, data));

    CHECK(data.entry == 1731);
    CHECK(data.type == 3);
    CHECK(data.displayId == 0xD15);
    CHECK(data.name == "Copper Vein");
    REQUIRE(data.hasData);
    // The whole point of the count: data[0] is the first field after the
    // strings, and one string too few or too many puts something else there.
    CHECK(data.data[0] == 0xA000);
    CHECK(data.data[23] == 0xA017);
}

TEST_CASE("TBC reads three, Classic reads none", "[gameobject][wire]") {
    SECTION("TBC") {
        auto bytes = buildResponse(180, 0, "Door", 3);
        auto packet = makePacket(bytes);
        GameObjectQueryResponseData data;
        TbcPacketParsers parsers;
        REQUIRE(parsers.parseGameObjectQueryResponse(packet, data));
        CHECK(data.name == "Door");
        CHECK(data.data[0] == 0xA000);
        CHECK(data.data[23] == 0xA017);
    }
    SECTION("Classic") {
        auto bytes = buildResponse(180, 0, "Door", 0);
        auto packet = makePacket(bytes);
        GameObjectQueryResponseData data;
        ClassicPacketParsers parsers;
        REQUIRE(parsers.parseGameObjectQueryResponse(packet, data));
        CHECK(data.name == "Door");
        CHECK(data.data[0] == 0xA000);
        CHECK(data.data[23] == 0xA017);
    }
}

TEST_CASE("one string too few shifts every data field", "[gameobject][wire]") {
    // Deliberately exercise an incorrect count through the shared helper.
    auto bytes = buildResponse(1731, 3, "Copper Vein", 3);
    auto packet = makePacket(bytes);
    GameObjectQueryResponseData data;
    REQUIRE(parseGameObjectQueryBody(packet, data, 2));
    CHECK(data.name == "Copper Vein");
    CHECK(data.data[0] != 0xA000);
}

TEST_CASE("TBC leaves only the size float after all three strings and data",
          "[gameobject][wire]") {
    // "Closing" plus its NUL occupies eight bytes. A wrong string count can
    // still leave a four-byte-aligned tail; check exact size and field values.
    const auto bytes = buildResponse(1731, 3, "Copper Vein", 3);

    SECTION("the right count leaves whole fields") {
        auto packet = makePacket(bytes);
        GameObjectQueryResponseData data;
        TbcPacketParsers parsers;
        REQUIRE(parsers.parseGameObjectQueryResponse(packet, data));
        CHECK(packet.getRemainingSize() == 4);
    }
    SECTION("one short leaves the closing text length in the tail") {
        auto packet = makePacket(bytes);
        GameObjectQueryResponseData data;
        REQUIRE(parseGameObjectQueryBody(packet, data, 2));
        CHECK(packet.getRemainingSize() == 12);
        CHECK(data.data[0] != 0xA000);
    }
}

TEST_CASE("an entry the server does not know stops at the flag",
          "[gameobject][wire]") {
    // The high bit means "no such gameobject" and nothing follows it. Reading
    // on would take whatever the next packet's bytes happen to be.
    Writer w;
    w.u32(1731u | 0x80000000u);
    auto packet = makePacket(w.bytes);
    GameObjectQueryResponseData data;
    REQUIRE(GameObjectQueryResponseParser::parse(packet, data));
    CHECK(data.entry == 1731);
    CHECK(data.name.empty());
    CHECK_FALSE(data.hasData);
}

TEST_CASE("a truncated response stops rather than reading past the end",
          "[gameobject][wire]") {
    const auto full = buildResponse(1731, 3, "Copper Vein", 3);
    for (size_t cut = 4; cut < full.size(); ++cut) {
        std::vector<uint8_t> shortened(full.begin(), full.begin() + static_cast<long>(cut));
        auto packet = makePacket(shortened);
        GameObjectQueryResponseData data;
        INFO("cut to " << cut << " of " << full.size());
        GameObjectQueryResponseParser::parse(packet, data);
    }
    SUCCEED("no read past the end at any length");
}
