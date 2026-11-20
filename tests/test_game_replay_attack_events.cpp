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

TEST_CASE("acquireAttackTarget selects nearest enemy in cone and respects target states", "[replay][combat][autotarget]") {
    constexpr uint64_t kEnemyNear = 0xF130000000003001ull;
    constexpr uint64_t kEnemyFar  = 0xF130000000003002ull;
    constexpr uint64_t kEnemyOff  = 0xF130000000003003ull;
    constexpr uint64_t kDeadMob   = 0xF130000000003004ull;
    constexpr uint64_t kFriend    = 0xF130000000003005ull;
    Session s("tbc");
    auto& gh = s.handler();
    gh.movementInfoRef().x = 0.0f;
    gh.movementInfoRef().y = 0.0f;
    gh.movementInfoRef().z = 0.0f;
    gh.movementInfoRef().orientation = 0.0f;  // facing +X

    // Spawn units: near enemy at (8, 0, 0), far enemy at (25, 0, 0), off-angle enemy at (0, 15, 0)
    auto addUnit = [&](uint64_t guid, float x, float y, uint32_t hp, bool hostile, uint32_t faction) {
        auto u = std::make_shared<wowee::game::Unit>(guid);
        u->setPosition(x, y, 0.0f, 0.0f);
        u->setHealth(hp);
        u->setMaxHealth(100);
        u->setHostile(hostile);
        u->setFactionTemplate(faction);
        gh.getEntityManager().addEntity(guid, u);
    };
    addUnit(kEnemyNear, 8.0f, 0.0f, 100, true, 14);
    addUnit(kEnemyFar, 25.0f, 0.0f, 100, true, 14);
    addUnit(kEnemyOff, 0.0f, 15.0f, 100, true, 14);
    addUnit(kDeadMob, 5.0f, 0.0f, 0, true, 14);
    // Friendly / non-attackable unit at (5, 2, 0) - closer than kEnemyNear, must be skipped.
    addUnit(kFriend, 5.0f, 2.0f, 100, false, 11);
    std::static_pointer_cast<wowee::game::Unit>(gh.getEntityManager().getEntity(kFriend))
        ->setUnitFlags(wowee::game::UNIT_FLAG_NON_ATTACKABLE);

    std::vector<std::string> errors;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        if (e == "UI_ERROR_MESSAGE" && !args.empty()) errors.push_back(args[0]);
    });

    // 1. No target -> acquires nearest live enemy in 30-deg cone (skips corpse and non-attackable).
    gh.clearTarget();
    CHECK(gh.acquireAttackTarget(0) == kEnemyNear);
    CHECK(gh.getTargetGuid() == kEnemyNear);

    // 2. Dead corpse selected -> refuses without switching, keeps corpse targeted.
    gh.setTarget(kDeadMob);
    CHECK(gh.acquireAttackTarget(0) == 0);
    CHECK(gh.getTargetGuid() == kDeadMob);

    // 3. Self selected -> switches to nearest live enemy in cone.
    gh.setTarget(Session::kPlayerGuid);
    CHECK(gh.acquireAttackTarget(0) == kEnemyNear);
    CHECK(gh.getTargetGuid() == kEnemyNear);

    // 4. Friendly / non-attackable NPC selected -> switches to nearest live enemy in cone.
    gh.setTarget(kFriend);
    CHECK(gh.acquireAttackTarget(0) == kEnemyNear);
    CHECK(gh.getTargetGuid() == kEnemyNear);

    // 5. Facing away (180 deg) with no target -> returns 0 and stays nil.
    gh.movementInfoRef().orientation = 3.14159265f;
    gh.clearTarget();
    CHECK(gh.acquireAttackTarget(0) == 0);
    CHECK(gh.getTargetGuid() == 0);

    // 6. Facing away with self selected -> returns 0 and keeps self selected.
    gh.setTarget(Session::kPlayerGuid);
    CHECK(gh.acquireAttackTarget(0) == 0);
    CHECK(gh.getTargetGuid() == Session::kPlayerGuid);
}
