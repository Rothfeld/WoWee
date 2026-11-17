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
