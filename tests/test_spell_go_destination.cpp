// TBC SMSG_SPELL_GO with a destination, as cmangos-tbc Spell::SendSpellGo
// writes it: SpellCastTargets after the hit and miss lists, and
// TARGET_FLAG_DEST_LOCATION (0x40) followed by x, y, z. Summon Boar Spirit
// (8286, implicit target 32 TARGET_LOCATION_UNIT_MINION_POSITION) carries the
// spot the spirit appears at, which is where SpellVisual 878's InstantAreaKit
// (9209) plays.
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/packet_parsers.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

wowee::network::Packet spellGo(bool withDestination) {
    wowee::network::Packet p(0);
    p.writePackedGuid(0xF130001148000001ull);  // caster object
    p.writePackedGuid(0xF130001148000001ull);  // caster unit
    p.writeUInt32(8286);
    p.writeUInt16(0x100);                      // cast flags
    p.writeUInt32(123456);                     // timestamp
    p.writeUInt8(0);                           // hits
    p.writeUInt8(0);                           // misses
    p.writeUInt32(withDestination ? 0x40u : 0u);
    if (withDestination) {
        p.writeFloat(2010.5f);
        p.writeFloat(1550.25f);
        p.writeFloat(72.0f);
    }
    p.setReadPos(0);
    return p;
}

}  // namespace

TEST_CASE("TBC spell go reads its destination", "[spell_go]") {
    TbcPacketParsers parsers;
    auto p = spellGo(true);
    SpellGoData data;
    REQUIRE(parsers.parseSpellGo(p, data));
    CHECK(data.spellId == 8286u);
    REQUIRE(data.hasDestination);
    CHECK(data.destX == 2010.5f);
    CHECK(data.destY == 1550.25f);
    CHECK(data.destZ == 72.0f);
}

TEST_CASE("TBC spell go without a destination has none", "[spell_go]") {
    TbcPacketParsers parsers;
    auto p = spellGo(false);
    SpellGoData data;
    REQUIRE(parsers.parseSpellGo(p, data));
    CHECK_FALSE(data.hasDestination);
}
