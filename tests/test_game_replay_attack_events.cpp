// PLAYER_ENTER_COMBAT / PLAYER_LEAVE_COMBAT follow the server's
// SMSG_ATTACKSTART / SMSG_ATTACKSTOP, not the local request. Original 2.4.3,
// measured with a macro "/run S() /startattack /run S()": IsCurrentAction on
// the Attack button is true in the same frame, PLAYER_ENTER_COMBAT arrives one
// round trip later; /stopattack fires PLAYER_LEAVE_COMBAT once, one round trip
// later; /stopattack while not attacking fires nothing.
// Evidence: _experiments/20261117T210203Z.pair3_autoattack_on_ability/.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using wowee::game::Opcode;

namespace {

long count(const std::vector<std::string>& events, const char* name) {
    return std::count(events.begin(), events.end(), name);
}

wowee::network::Packet attackStart(uint64_t attacker, uint64_t victim) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTART));
    p.writeUInt64(attacker);
    p.writeUInt64(victim);
    return p;
}

wowee::network::Packet attackStop(uint64_t attacker, uint64_t victim) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTOP));
    p.writePackedGuid(attacker);
    p.writePackedGuid(victim);
    p.writeUInt32(0);
    return p;
}

}  // namespace

TEST_CASE("combat enter/leave events wait for the server", "[replay][combat]") {
    constexpr uint64_t kEnemy = 0xF130000000002002ull;
    const uint64_t player = Session::kPlayerGuid;
    Session s("tbc");
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { events.push_back(e); });

    s.handler().startAutoAttack(kEnemy);
    CHECK(s.handler().hasAutoAttackIntent());
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    CHECK(count(events, "PLAYER_ENTER_COMBAT") == 0);

    s.receive(attackStart(player, kEnemy));
    CHECK(count(events, "PLAYER_ENTER_COMBAT") == 1);

    s.handler().stopAutoAttack();
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSTOP).size() == 1);
    CHECK(count(events, "PLAYER_LEAVE_COMBAT") == 0);

    s.receive(attackStop(player, kEnemy));
    CHECK(count(events, "PLAYER_LEAVE_COMBAT") == 1);

    // A stop answered while not attacking is silent.
    s.receive(attackStop(player, 0));
    CHECK(count(events, "PLAYER_LEAVE_COMBAT") == 1);
}
