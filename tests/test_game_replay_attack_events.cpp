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

TEST_CASE("ACTIONBAR_UPDATE_STATE fires after same-frame cast fail, cancel, and channel stop",
          "[replay][actionbar][fishing]") {
    constexpr uint32_t kFishingSpellId = 33095u;
    Session s("tbc");
    auto& gh = s.handler();
    gh.spellNameCacheRef()[kFishingSpellId].name = "Fishing";
    gh.spellNameCacheLoadedRef() = true;
    {
        wowee::network::Packet learned(wowee::game::wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(kFishingSpellId);
        s.receive(learned);
    }

    std::vector<std::string> events;
    gh.setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { events.push_back(e); });

    auto sendCastFailed = [&](uint32_t spellId, uint8_t reason) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CAST_FAILED));
        p.writeUInt32(spellId);
        p.writeUInt8(reason);
        p.writeUInt8(1);  // castCount
        s.receive(p);
    };

    // 1. Same-frame 0 -> 33095 -> 0 pulse (PostClick checked button on pendingCastSpellId_,
    //    then SMSG_CAST_FAILED arrived before SpellHandler::updateTimers()).
    events.clear();
    gh.castSpell(kFishingSpellId, 0);
    CHECK(gh.getActiveActionSpellId() == kFishingSpellId);
    sendCastFailed(kFishingSpellId, 2);  // SPELL_FAILED_BAD_TARGETS
    CHECK(gh.getActiveActionSpellId() == 0);
    gh.getSpellHandler()->updateTimers(0.016f);
    CHECK(count(events, "ACTIONBAR_UPDATE_STATE") == 1);
    CHECK(count(events, "CURRENT_SPELL_CAST_CHANGED") == 1);

    // 2. Same-frame castSpell -> cancelCast() before SMSG_SPELL_START clears pendingCastSpellId_.
    events.clear();
    gh.castSpell(kFishingSpellId, 0);
    CHECK(gh.getActiveActionSpellId() == kFishingSpellId);
    gh.cancelCast();
    CHECK(gh.getActiveActionSpellId() == 0);
    gh.getSpellHandler()->updateTimers(0.016f);
    CHECK(count(events, "ACTIONBAR_UPDATE_STATE") == 1);

    // 3. Channel lifecycle: MSG_CHANNEL_START clears pendingCastSpellId_ and MSG_CHANNEL_UPDATE(0)
    //    clears active spell and fires ACTIONBAR_UPDATE_STATE.
    events.clear();
    gh.castSpell(kFishingSpellId, 0);
    wowee::network::Packet chStart(wowee::game::wireOpcode(Opcode::MSG_CHANNEL_START));
    chStart.writePackedGuid(Session::kPlayerGuid);
    chStart.writeUInt32(kFishingSpellId);
    chStart.writeUInt32(22000u);
    s.receive(chStart);
    CHECK(gh.getActiveActionSpellId() == kFishingSpellId);
    gh.getSpellHandler()->updateTimers(0.016f);
    CHECK(count(events, "ACTIONBAR_UPDATE_STATE") == 1);

    events.clear();
    wowee::network::Packet chStop(wowee::game::wireOpcode(Opcode::MSG_CHANNEL_UPDATE));
    chStop.writePackedGuid(Session::kPlayerGuid);
    chStop.writeUInt32(0u);
    s.receive(chStop);
    CHECK(gh.getActiveActionSpellId() == 0);
    gh.getSpellHandler()->updateTimers(0.016f);
    CHECK(count(events, "ACTIONBAR_UPDATE_STATE") == 1);
}

TEST_CASE("remote melee stance clears on lethal swing and dead/despawned victim",
          "[replay][combat][stance]") {
    using wowee::game::isLiveMeleeVictim;
    using wowee::game::UNIT_DYNFLAG_DEAD;

    CHECK(isLiveMeleeVictim(true, false, 100, 100, 0));
    CHECK_FALSE(isLiveMeleeVictim(false, false, 100, 100, 0));
    CHECK_FALSE(isLiveMeleeVictim(true, true, 100, 100, 0));
    CHECK_FALSE(isLiveMeleeVictim(true, false, 0, 100, 0));
    CHECK_FALSE(isLiveMeleeVictim(true, false, 100, 100, UNIT_DYNFLAG_DEAD));

    constexpr uint64_t kRemotePlayer = 0x0000000000000068ull;
    constexpr uint64_t kMobA         = 0xF130003F125970DAull;
    constexpr uint64_t kMobB         = 0xF130003F2759704Bull;

    Session s("tbc");
    auto& gh = s.handler();

    auto remote = std::make_shared<wowee::game::Player>(kRemotePlayer);
    remote->setHealth(10000);
    remote->setMaxHealth(10000);
    gh.getEntityManager().addEntity(kRemotePlayer, remote);

    auto mobA = std::make_shared<wowee::game::Unit>(kMobA);
    mobA->setHealth(5000);
    mobA->setMaxHealth(5000);
    gh.getEntityManager().addEntity(kMobA, mobA);

    auto mobB = std::make_shared<wowee::game::Unit>(kMobB);
    mobB->setHealth(3000);
    mobB->setMaxHealth(3000);
    gh.getEntityManager().addEntity(kMobB, mobB);

    auto tbcSwing = [](uint64_t attacker, uint64_t victim, int32_t damage) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ATTACKERSTATEUPDATE));
        p.writeUInt32(0x2u);           // HITINFO_NORMALSWING2
        p.writePackedGuid(attacker);
        p.writePackedGuid(victim);
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt8(1u);              // subDamageCount
        p.writeUInt32(1u);             // schoolMask (physical)
        p.writeFloat(static_cast<float>(damage));
        p.writeUInt32(static_cast<uint32_t>(damage));
        p.writeUInt32(0u);             // absorbed
        p.writeUInt32(0u);             // resisted
        p.writeUInt32(1u);             // victimState = HIT
        p.writeUInt32(0u);             // unknown
        p.writeUInt32(0u);             // spellId
        p.writeUInt32(0u);             // blocked
        return p;
    };

    // 1. Non-lethal swing arms meleeVictimGuid for a remote attacker.
    s.receive(tbcSwing(kRemotePlayer, kMobA, 1000));
    CHECK(remote->getMeleeVictimGuid() == kMobA);

    // 2. Killing blow clears meleeVictimGuid even without a subsequent SMSG_ATTACKSTOP.
    s.receive(tbcSwing(kRemotePlayer, kMobA, 5000));
    CHECK(remote->getMeleeVictimGuid() == 0);

    // 3. One-shot killing blow from idle leaves meleeVictimGuid == 0.
    s.receive(tbcSwing(kRemotePlayer, kMobB, 4000));
    CHECK(remote->getMeleeVictimGuid() == 0);

    // 4. Late swing arriving after victim health is already 0 leaves meleeVictimGuid == 0.
    mobB->setHealth(0);
    s.receive(tbcSwing(kRemotePlayer, kMobB, 100));
    CHECK(remote->getMeleeVictimGuid() == 0);
}

TEST_CASE("UNIT_FLAG_NOT_SELECTABLE units (e.g. Void Zone 16697) cannot be auto-targeted, targeted, or focused",
          "[replay][combat][selectable]") {
    constexpr uint64_t kVoidZone = 0xF130004139000001ull;
    constexpr uint64_t kBoss     = 0xF130003EBD000002ull;

    Session s("tbc");
    auto& gh = s.handler();

    auto voidZone = std::make_shared<wowee::game::Unit>(kVoidZone);
    voidZone->setName("Void Zone");
    voidZone->setHealth(10000);
    voidZone->setMaxHealth(10000);
    voidZone->setUnitFlags(wowee::game::UNIT_FLAG_NOT_SELECTABLE);
    gh.getEntityManager().addEntity(kVoidZone, voidZone);

    auto boss = std::make_shared<wowee::game::Unit>(kBoss);
    boss->setName("Lady Blaumeux");
    boss->setHealth(500000);
    boss->setMaxHealth(500000);
    boss->setUnitFlags(0);
    gh.getEntityManager().addEntity(kBoss, boss);

    auto tbcSpellDmg = [](uint64_t target, uint64_t attacker, uint32_t spellId, uint32_t damage) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
        p.writePackedGuid(target);
        p.writePackedGuid(attacker);
        p.writeUInt32(spellId);
        p.writeUInt32(damage);
        p.writeUInt8(0x20u);  // shadow school
        p.writeUInt32(0u);    // absorbed
        p.writeUInt32(0u);    // resisted
        p.writeUInt8(0u);     // periodicLog
        p.writeUInt8(0u);     // unused
        p.writeUInt32(0u);    // blocked
        p.writeUInt32(0u);    // flags
        p.writeUInt8(0u);     // debug/padding
        return p;
    };

    // 1. Direct setTarget / setFocus on UNIT_FLAG_NOT_SELECTABLE unit is refused.
    gh.clearTarget();
    gh.setTarget(kVoidZone);
    CHECK(gh.getTargetGuid() == 0);
    gh.setFocus(kVoidZone);
    CHECK(gh.getFocusGuid() == 0);

    // 2. Spell damage or melee attack start from Void Zone must NOT auto-select Void Zone.
    s.receive(tbcSpellDmg(Session::kPlayerGuid, kVoidZone, 28865u, 987u));
    CHECK(gh.getTargetGuid() == 0);
    s.receive(attackStart(kVoidZone, Session::kPlayerGuid));
    CHECK(gh.getTargetGuid() == 0);

    // 3. Spell damage from a selectable boss (Lady Blaumeux) auto-selects it when untargeted.
    s.receive(tbcSpellDmg(Session::kPlayerGuid, kBoss, 28833u, 576u));
    CHECK(gh.getTargetGuid() == kBoss);

    // 4. If the currently targeted unit gains UNIT_FLAG_NOT_SELECTABLE via VALUES update, target clears.
    const uint16_t flagsIdx = wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_FLAGS);
    REQUIRE(flagsIdx != 0xFFFF);
    wowee::network::Packet upd(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
    upd.writeUInt32(1u);  // blockCount
    upd.writeUInt8(0u);   // hasTransport
    upd.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::VALUES));
    upd.writePackedGuid(kBoss);
    const uint8_t maskBlocks = static_cast<uint8_t>((flagsIdx / 32) + 1);
    upd.writeUInt8(maskBlocks);
    for (uint8_t b = 0; b < maskBlocks; ++b) {
        uint32_t mask = (b == flagsIdx / 32) ? (1u << (flagsIdx % 32)) : 0u;
        upd.writeUInt32(mask);
    }
    upd.writeUInt32(wowee::game::UNIT_FLAG_NOT_SELECTABLE);
    s.receive(upd);
    CHECK(gh.getTargetGuid() == 0);
}
