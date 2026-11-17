// Death and resurrection through GameHandler's own dispatch, TBC as CMaNGOS
// writes it. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::UF;

namespace {

struct Events {
    std::vector<std::string> names;
    explicit Events(Session& s) {
        s.handler().setAddonEventCallback(
            [this](const std::string& e, const std::vector<std::string>&) { names.push_back(e); });
    }
    [[nodiscard]] bool has(const std::string& e) const {
        return std::find(names.begin(), names.end(), e) != names.end();
    }
};

void spawnAlivePlayer(Session& s) {
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
}

// Player::KillPlayer: health to zero and, outside an instance, the
// RELEASE_TIMER byte flag, in one values update.
void kill(Session& s, uint32_t playerFieldBytes) {
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_HEALTH, 0},
                                           {UF::PLAYER_FIELD_BYTES, playerFieldBytes}}));
}

// Player::SendInitWorldStates, TBC: map, zone, area, count, pairs.
wowee::network::Packet initWorldStates(uint32_t map, uint32_t zone, uint32_t area) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_INIT_WORLD_STATES));
    p.writeUInt32(map);
    p.writeUInt32(zone);
    p.writeUInt32(area);
    p.writeUInt16(1);
    p.writeUInt32(2264);
    p.writeUInt32(0);
    return p;
}

}  // namespace

TEST_CASE("an open-world death counts down six minutes to release", "[replay][death]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    CHECK(s.handler().getReleaseTimeRemainingSec() == 0);
    Events events(s);
    kill(s, 0x08);
    REQUIRE(events.has("PLAYER_DEAD"));
    const double remaining = s.handler().getReleaseTimeRemainingSec();
    CHECK(remaining > 359.0);
    CHECK(remaining <= 360.0);
}

TEST_CASE("an instance death has no release timer", "[replay][death]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    kill(s, 0x00);
    CHECK(s.handler().getReleaseTimeRemainingSec() == -1);
}

TEST_CASE("world states resent on resurrection raise no zone event", "[replay][death][zone]") {
    Session s("tbc");
    spawnAlivePlayer(s);
    Events events(s);
    // Ashenvale / Astranaar area 415: the 13:47 pair's .revive, where
    // ResurrectPlayer -> UpdateZone(force) resent this packet.
    s.receive(initWorldStates(1, 331, 415));
    s.receive(initWorldStates(1, 148, 148));
    CHECK_FALSE(events.has("ZONE_CHANGED_NEW_AREA"));
    CHECK_FALSE(events.has("ZONE_CHANGED"));
    CHECK(s.handler().getWorldStates().at(2264) == 0);
}

TEST_CASE("a map change raises no zone event before the new map is loaded", "[replay][zone]") {
    // Ledger #100. SMSG_NEW_WORLD (CMaNGOS Player::TeleportTo far branch):
    // map, position, orientation. Razorfen Kraul, map 47.
    Session s("tbc");
    spawnAlivePlayer(s);
    Events events(s);
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NEW_WORLD));
    p.writeUInt32(47);
    p.writeFloat(1943.0f);
    p.writeFloat(1544.0f);
    p.writeFloat(82.0f);
    p.writeFloat(0.0f);
    s.receive(p);
    CHECK(events.has("PLAYER_ENTERING_WORLD"));
    // GetZoneText still names the old zone here; the renderer announces the
    // new one while the load resolves it.
    CHECK_FALSE(events.has("ZONE_CHANGED_NEW_AREA"));
}
