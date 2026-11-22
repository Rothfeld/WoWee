// Entity, Unit, Player, GameObject, EntityManager tests
#include <catch_amalgamated.hpp>
#include "game/entity.hpp"
#include "game/game_object_reach.hpp"
#include "game/remote_jump.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/animation/ground_direction.hpp"
#include <cmath>
#include <limits>
#include <optional>
#include <algorithm>
#include <memory>

using namespace wowee::game;

TEST_CASE("Stationary creatures track selected targets while players preserve facing", "[entity][target-facing]") {
    UpdateFieldTable fields;
    fields.setIndex(UF::UNIT_FIELD_TARGET_LO, 16);
    fields.setIndex(UF::UNIT_FIELD_TARGET_HI, 17);
    fields.setIndex(UF::UNIT_FIELD_BYTES_1, 159);
    struct RestoreTable {
        const UpdateFieldTable* previous;
        ~RestoreTable() { setActiveUpdateFieldTable(previous); }
    } restore{getActiveUpdateFieldTable()};
    setActiveUpdateFieldTable(&fields);
    EntityManager entities;
    constexpr uint64_t targetGuid = 0xf130000123000456ULL;
    auto target = std::make_shared<Player>(targetGuid);
    entities.addEntity(targetGuid, target);
    Unit creature(77);
    creature.setHealth(100);
    creature.setPosition(0, 0, 0, 0.7f);
    creature.setField(fieldIndex(UF::UNIT_FIELD_TARGET_LO), static_cast<uint32_t>(targetGuid));
    creature.setField(fieldIndex(UF::UNIT_FIELD_TARGET_HI), static_cast<uint32_t>(targetGuid >> 32));
    target->setPosition(0, 5, 0, 0);
    REQUIRE(creatureTargetFacing(creature, entities).value() == Catch::Approx(-1.5707963f));
    target->setPosition(-5, 0, 0, 0);
    REQUIRE(std::abs(creatureTargetFacing(creature, entities).value()) == Catch::Approx(3.1415927f));
    REQUIRE(creature.getOrientation() == Catch::Approx(0.7f));

    creature.startMoveTo(10, 0, 0, 0, 2);
    REQUIRE_FALSE(creatureTargetFacing(creature, entities).has_value());
    creature.setPosition(0, 0, 0, 0.7f);
    creature.setHealth(0);
    REQUIRE_FALSE(creatureTargetFacing(creature, entities).has_value());
    Player player(78);
    player.setHealth(100);
    player.setField(fieldIndex(UF::UNIT_FIELD_TARGET_LO), static_cast<uint32_t>(targetGuid));
    player.setField(fieldIndex(UF::UNIT_FIELD_TARGET_HI), static_cast<uint32_t>(targetGuid >> 32));
    REQUIRE_FALSE(creatureTargetFacing(player, entities).has_value());
    creature.setHealth(100);
    // Native: a seated or stunned Marshal Dughan keeps his heading.
    creature.setUnitFlags(0x00040000);
    REQUIRE_FALSE(creatureTargetFacing(creature, entities).has_value());
    creature.setUnitFlags(0);
    creature.setField(fieldIndex(UF::UNIT_FIELD_BYTES_1), 1);
    REQUIRE_FALSE(creatureTargetFacing(creature, entities).has_value());
    creature.setField(fieldIndex(UF::UNIT_FIELD_BYTES_1), 0);
    REQUIRE(creatureTargetFacing(creature, entities).has_value());
    entities.removeEntity(targetGuid);
    REQUIRE_FALSE(creatureTargetFacing(creature, entities).has_value());
}

TEST_CASE("Entity default construction", "[entity]") {
    Entity e;
    REQUIRE(e.getGuid() == 0);
    REQUIRE(e.getType() == ObjectType::OBJECT);
    REQUIRE(e.getX() == 0.0f);
    REQUIRE(e.getY() == 0.0f);
    REQUIRE(e.getZ() == 0.0f);
    REQUIRE(e.getOrientation() == 0.0f);
}

TEST_CASE("Entity GUID constructor", "[entity]") {
    Entity e(0xDEADBEEF);
    REQUIRE(e.getGuid() == 0xDEADBEEF);
}

TEST_CASE("Entity position set/get", "[entity]") {
    Entity e;
    e.setPosition(1.0f, 2.0f, 3.0f, 1.57f);
    REQUIRE(e.getX() == Catch::Approx(1.0f));
    REQUIRE(e.getY() == Catch::Approx(2.0f));
    REQUIRE(e.getZ() == Catch::Approx(3.0f));
    REQUIRE(e.getOrientation() == Catch::Approx(1.57f));
}

TEST_CASE("Entity ignores positive-duration movement without displacement",
          "[entity][movement]") {
    Entity e;
    e.setPosition(10.0f, 20.0f, 30.0f, 0.0f);

    e.startMoveTo(10.0f, 20.0f, 30.0f, 1.25f, 0.5f);

    REQUIRE_FALSE(e.isEntityMoving());
    REQUIRE_FALSE(e.isActivelyMoving());
    REQUIRE(e.getOrientation() == Catch::Approx(1.25f));
}

TEST_CASE("Entity still interpolates meaningful movement", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);

    e.startMoveTo(5.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(e.isActivelyMoving());

    e.updateMovement(0.5f);
    REQUIRE(e.getX() == Catch::Approx(2.5f));
    REQUIRE(e.isActivelyMoving());
}

TEST_CASE("Entity field set/get/has", "[entity]") {
    Entity e;
    REQUIRE_FALSE(e.hasField(10));

    e.setField(10, 0xCAFE);
    REQUIRE(e.hasField(10));
    REQUIRE(e.getField(10) == 0xCAFE);

    // Overwrite
    e.setField(10, 0xBEEF);
    REQUIRE(e.getField(10) == 0xBEEF);

    // Non-existent returns 0
    REQUIRE(e.getField(999) == 0);
}

TEST_CASE("Unit construction and type", "[entity]") {
    Unit u;
    REQUIRE(u.getType() == ObjectType::UNIT);

    Unit u2(0x123);
    REQUIRE(u2.getGuid() == 0x123);
    REQUIRE(u2.getType() == ObjectType::UNIT);
}

TEST_CASE("Unit name", "[entity]") {
    Unit u;
    REQUIRE(u.getName().empty());
    u.setName("Hogger");
    REQUIRE(u.getName() == "Hogger");
}

TEST_CASE("Unit health", "[entity]") {
    Unit u;
    REQUIRE(u.getHealth() == 0);
    REQUIRE(u.getMaxHealth() == 0);

    u.setHealth(500);
    u.setMaxHealth(1000);
    REQUIRE(u.getHealth() == 500);
    REQUIRE(u.getMaxHealth() == 1000);
}

TEST_CASE("Unit power by type", "[entity]") {
    Unit u;
    u.setPowerType(0); // mana
    u.setPower(200);
    u.setMaxPower(500);

    REQUIRE(u.getPower() == 200);
    REQUIRE(u.getMaxPower() == 500);
    REQUIRE(u.getPowerByType(0) == 200);
    REQUIRE(u.getMaxPowerByType(0) == 500);

    // Set rage (type 1)
    u.setPowerByType(1, 50);
    u.setMaxPowerByType(1, 100);
    REQUIRE(u.getPowerByType(1) == 50);
    REQUIRE(u.getMaxPowerByType(1) == 100);

    // Out of bounds clamps
    REQUIRE(u.getPowerByType(7) == 0);
    REQUIRE(u.getMaxPowerByType(7) == 0);
}

TEST_CASE("Unit level, entry, displayId", "[entity]") {
    Unit u;
    REQUIRE(u.getLevel() == 1); // default
    u.setLevel(80);
    REQUIRE(u.getLevel() == 80);

    u.setEntry(1234);
    REQUIRE(u.getEntry() == 1234);

    u.setDisplayId(5678);
    REQUIRE(u.getDisplayId() == 5678);
}

TEST_CASE("Unit flags", "[entity]") {
    Unit u;
    u.setUnitFlags(0x01);
    REQUIRE(u.getUnitFlags() == 0x01);

    REQUIRE_FALSE(u.hasCreepVisibility());
    u.setVisibilityFlags(UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.getVisibilityFlags() == UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.hasCreepVisibility());
    u.clearCreepVisibility();
    REQUIRE_FALSE(u.hasCreepVisibility());
    REQUIRE((u.getVisibilityFlags() & UNIT_VIS_FLAG_CREEP) == 0);

    u.setDynamicFlags(0x02);
    REQUIRE(u.getDynamicFlags() == 0x02);

    u.setNpcFlags(0x04);
    REQUIRE(u.getNpcFlags() == 0x04);
    REQUIRE(u.isInteractable());

    u.setNpcFlags(0);
    REQUIRE_FALSE(u.isInteractable());
}

TEST_CASE("Corpse state uses the real dynamic dead bit", "[entity][corpse]") {
    REQUIRE(isUnitCorpseState(0, 100, 0));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_DEAD));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_LOOTABLE));

    REQUIRE_FALSE(isUnitCorpseState(100, 100, 0));
    // Regression: 0x08 is tapped-by-player, not dead.
    REQUIRE_FALSE(isUnitCorpseState(100, 100, UNIT_DYNFLAG_TAPPED_BY_PLAYER));
    REQUIRE(UNIT_DYNFLAG_DEAD == 0x00000020);
}

TEST_CASE("Dead pose state covers corpse, feign death, stand-state dead, and emote state dead", "[entity][corpse]") {
    // Normal dead unit (health == 0).
    REQUIRE(isUnitDeadPose(0, 100, 0, 0, 0));
    // Permanent Feign Death (29266, e.g. Madrigosa) sets UNIT_DYNFLAG_DEAD with health > 0.
    REQUIRE(isUnitDeadPose(100, 100, UNIT_DYNFLAG_DEAD, 0, 0));
    // UNIT_STAND_STATE_DEAD (7) with health > 0 and no dynamic flags.
    REQUIRE(isUnitDeadPose(100, 100, 0, UNIT_STAND_STATE_DEAD, 0));
    // UNIT_NPC_EMOTESTATE == EMOTE_STATE_DEAD (65) with health > 0 and no dynamic flags.
    REQUIRE(isUnitDeadPose(100, 100, 0, 0, EMOTE_STATE_DEAD));
    // Living standing unit.
    REQUIRE_FALSE(isUnitDeadPose(100, 100, 0, 0, 0));
    REQUIRE_FALSE(isUnitDeadPose(100, 100, UNIT_DYNFLAG_TAPPED, 1, 0));
}

TEST_CASE("Clearing UNIT_DYNFLAG_DEAD only respawns living units", "[entity][corpse]") {
    // Killing a feigning NPC (e.g. Warder Corpse with aura 29266) drops health to 0
    // and strips UNIT_DYNFLAG_DEAD in the same update: must NOT trigger respawn.
    REQUIRE_FALSE(unitDynDeadClearIsRespawn(UNIT_DYNFLAG_DEAD, 0, 0, 10000));
    REQUIRE_FALSE(unitDynDeadClearIsRespawn(UNIT_DYNFLAG_DEAD, UNIT_DYNFLAG_LOOTABLE, 0, 10000));
    // Still lootable even with non-zero health placeholder: not a living respawn.
    REQUIRE_FALSE(unitDynDeadClearIsRespawn(UNIT_DYNFLAG_DEAD, UNIT_DYNFLAG_LOOTABLE, 100, 100));
    // Cancelling Feign Death while alive or resurrecting with health > 0: IS a respawn.
    REQUIRE(unitDynDeadClearIsRespawn(UNIT_DYNFLAG_DEAD, 0, 10000, 10000));
    // No transition when flag was not cleared.
    REQUIRE_FALSE(unitDynDeadClearIsRespawn(0, 0, 10000, 10000));
    REQUIRE_FALSE(unitDynDeadClearIsRespawn(UNIT_DYNFLAG_DEAD, UNIT_DYNFLAG_DEAD, 10000, 10000));
}

TEST_CASE("Tap flags follow each expansion's dynamic flag layout", "[entity][tap]") {
    // CMaNGOS-TBC: 0x04 TAPPED only for viewers without the tap, 0x08 ROOTED.
    REQUIRE(unitTappedByOther(UNIT_DYNFLAG_TAPPED, true));
    REQUIRE(unitTappedByOther(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_ROOTED_LEGACY, true));
    REQUIRE_FALSE(unitTappedByOther(UNIT_DYNFLAG_ROOTED_LEGACY, true));
    REQUIRE_FALSE(unitIsTappedByPlayer(UNIT_DYNFLAG_TAPPED | 0x08, true));
    REQUIRE_FALSE(unitIsTappedByAllThreatList(0x88, true));
    // WotLK: 0x08 TAPPED_BY_PLAYER, 0x80 TAPPED_BY_ALL_THREAT_LIST.
    REQUIRE(unitTappedByOther(UNIT_DYNFLAG_TAPPED, false));
    REQUIRE_FALSE(unitTappedByOther(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER, false));
    REQUIRE_FALSE(unitTappedByOther(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST, false));
    REQUIRE(unitIsTappedByPlayer(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER, false));
}

TEST_CASE("Unit faction and hostility", "[entity]") {
    Unit u;
    u.setFactionTemplate(14); // Undercity
    REQUIRE(u.getFactionTemplate() == 14);

    REQUIRE_FALSE(u.isHostile());
    u.setHostile(true);
    REQUIRE(u.isHostile());
}

TEST_CASE("Unit mount display ID", "[entity]") {
    Unit u;
    REQUIRE(u.getMountDisplayId() == 0);
    u.setMountDisplayId(14374);
    REQUIRE(u.getMountDisplayId() == 14374);
}

TEST_CASE("Player inherits Unit", "[entity]") {
    Player p(0xABC);
    REQUIRE(p.getType() == ObjectType::PLAYER);
    REQUIRE(p.getGuid() == 0xABC);

    // Player inherits Unit name - regression test for the shadowed-field fix
    p.setName("Arthas");
    REQUIRE(p.getName() == "Arthas");

    p.setLevel(80);
    REQUIRE(p.getLevel() == 80);
}

TEST_CASE("GameObject construction", "[entity]") {
    GameObject go(0x999);
    REQUIRE(go.getType() == ObjectType::GAMEOBJECT);
    REQUIRE(go.getGuid() == 0x999);

    go.setName("Mailbox");
    REQUIRE(go.getName() == "Mailbox");

    go.setEntry(42);
    REQUIRE(go.getEntry() == 42);

    go.setDisplayId(100);
    REQUIRE(go.getDisplayId() == 100);
}

TEST_CASE("EntityManager add/get/has/remove", "[entity]") {
    EntityManager mgr;
    REQUIRE(mgr.getEntityCount() == 0);

    auto unit = std::make_shared<Unit>(1);
    unit->setName("TestUnit");
    mgr.addEntity(1, unit);

    REQUIRE(mgr.getEntityCount() == 1);
    REQUIRE(mgr.hasEntity(1));
    REQUIRE_FALSE(mgr.hasEntity(2));

    auto retrieved = mgr.getEntity(1);
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->getGuid() == 1);

    mgr.removeEntity(1);
    REQUIRE_FALSE(mgr.hasEntity(1));
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager clear", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, std::make_shared<Entity>(1));
    mgr.addEntity(2, std::make_shared<Entity>(2));
    REQUIRE(mgr.getEntityCount() == 2);

    mgr.clear();
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager null entity rejected", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, nullptr);
    // Null should be rejected (logged warning, not stored)
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager getEntities returns all", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(10, std::make_shared<Unit>(10));
    mgr.addEntity(20, std::make_shared<Player>(20));
    mgr.addEntity(30, std::make_shared<GameObject>(30));

    const auto& all = mgr.getEntities();
    REQUIRE(all.size() == 3);
    REQUIRE(all.count(10) == 1);
    REQUIRE(all.count(20) == 1);
    REQUIRE(all.count(30) == 1);
}

TEST_CASE("EntityManager spatial query filters nearby entities", "[entity][spatial]") {
    EntityManager mgr;
    auto origin = std::make_shared<Unit>(1);
    origin->setPosition(-10.0f, -10.0f, 0.0f, 0.0f);
    auto nearby = std::make_shared<Unit>(2);
    nearby->setPosition(20.0f, 15.0f, 0.0f, 0.0f);
    auto distant = std::make_shared<Unit>(3);
    distant->setPosition(500.0f, 500.0f, 0.0f, 0.0f);
    mgr.addEntity(1, origin);
    mgr.addEntity(2, nearby);
    mgr.addEntity(3, distant);

    const auto result = mgr.getEntitiesNear(0.0f, 0.0f, 50.0f);
    REQUIRE(result.size() == 2);
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 1; }));
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 2; }));
    REQUIRE_FALSE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 3; }));
}

TEST_CASE("Remote ground movement continues across heartbeat gaps", "[entity][movement]") {
    Player p;
    p.setPosition(0, 0, 0, 0);
    p.updateRemoteGroundMotion({0, 0, 0}, 0, {7, 0});
    for (int i = 1; i <= 40; ++i) {
        p.updateMovement(0.025f);
        REQUIRE(p.getX() == Catch::Approx(i * 0.175f).margin(0.0001f));
        REQUIRE(p.isActivelyMoving());
        if (i == 20) p.updateRemoteGroundMotion({3.5f, 0, 0}, 0, {7, 0});
    }
    // Stop at the observed position, with no half-second catch-up teleport.
    p.updateRemoteGroundMotion({7, 0, 0}, 0, {0, 0});
    REQUIRE_FALSE(p.isActivelyMoving());
    p.updateMovement(0.5f);
    REQUIRE(p.getX() == Catch::Approx(7));
}

TEST_CASE("Remote ground correction blends without restarting motion", "[entity][movement]") {
    Player p;
    p.setPosition(0, 0, 0, 0);
    p.updateRemoteGroundMotion({0, 0, 0}, 0, {7, 0});
    p.updateMovement(0.5f);
    p.updateRemoteGroundMotion({3.6f, 0, 0}, 0, {7, 0});
    REQUIRE(p.getX() == Catch::Approx(3.5f));
    p.updateMovement(0.05f);
    REQUIRE(p.getX() == Catch::Approx(3.9f));
    p.updateMovement(0.05f);
    REQUIRE(p.getX() == Catch::Approx(4.3f));
    p.updateMovement(0.1f);
    REQUIRE(p.getX() == Catch::Approx(5.0f));
}

// A player running while it turns: its velocity, set from the packet's facing
// and movement flags, turns with the facing until the next heartbeat.
TEST_CASE("Remote ground movement turns with a keyboard turn", "[entity][movement][remote-turn]") {
    // MovementHandler's velocity for running forward at server facing s.
    const auto forward = [](float serverFacing) {
        return glm::vec2(std::sin(serverFacing) * 7.0f, std::cos(serverFacing) * 7.0f);
    };
    Player p;
    p.setPosition(0, 0, 0, 0);
    p.updateRemoteGroundMotion({0, 0, 0}, -1.5707963f, forward(0.0f));
    p.updateRemoteFacing(-1.5707963f, -1.5707963f, 3.141593f, 0.0f);
    p.updateMovement(0.0f);  // the frame that read the packet
    for (int i = 0; i < 25; ++i) p.updateMovement(0.02f);
    // Half a second at pi rad/s: a quarter circle of radius 7/pi.
    constexpr float radius = 7.0f / 3.141593f;
    CHECK(p.getOrientation() == Catch::Approx(0.0f).margin(1e-4));
    const glm::vec2 serverEnd(radius, radius);  // server (x, y) after a quarter turn left from facing 0
    CHECK(p.getX() == Catch::Approx(serverEnd.y).margin(0.02f));
    CHECK(p.getY() == Catch::Approx(serverEnd.x).margin(0.02f));
    p.updateRemoteGroundMotion({p.getX(), p.getY(), 0}, 0.0f, forward(1.5707963f));
    CHECK(p.getOrientation() == 0.0f);
}

TEST_CASE("Remote prediction stops on stale connection without rewinding", "[entity][movement]") {
    Player p;
    p.setPosition(0, 0, 0, 0);
    p.updateRemoteGroundMotion({0, 0, 0}, 0, {7, 0});
    p.updateMovement(3.0f);
    REQUIRE(p.getX() == Catch::Approx(14));
    REQUIRE_FALSE(p.isActivelyMoving());
    p.updateMovement(1.0f);
    REQUIRE(p.getX() == Catch::Approx(14));
    p.setPosition(1, 2, 3, 0);
    p.updateMovement(0.5f);
    REQUIRE(p.getX() == Catch::Approx(1));
}

TEST_CASE("Directional ground pose preserves facing while aligning legs", "[movement][animation]") {
    using wowee::rendering::groundDirection;
    constexpr float halfPi = 1.57079632679f;
    const auto left = groundDirection(false, false, true, false);
    REQUIRE(left.modelYawOffset == Catch::Approx(halfPi));
    REQUIRE(left.torsoYawOffset == Catch::Approx(-halfPi));
    REQUIRE_FALSE(left.backwards);
    const auto right = groundDirection(false, false, false, true);
    REQUIRE(right.modelYawOffset == Catch::Approx(-halfPi));
    const auto backward = groundDirection(false, true, false, false);
    REQUIRE(backward.modelYawOffset == Catch::Approx(0));
    REQUIRE(backward.backwards);
    const auto forwardLeft = groundDirection(true, false, true, false);
    REQUIRE(forwardLeft.modelYawOffset == Catch::Approx(halfPi / 2));
    const auto backwardLeft = groundDirection(false, true, true, false);
    REQUIRE(backwardLeft.modelYawOffset == Catch::Approx(-halfPi / 2));
    REQUIRE(backwardLeft.backwards);
    REQUIRE(backwardLeft.modelYawOffset + backwardLeft.torsoYawOffset == Catch::Approx(0));
    const auto stopped = groundDirection(false, false, false, false);
    REQUIRE(stopped.modelYawOffset == 0);
    REQUIRE(stopped.torsoYawOffset == 0);
}

TEST_CASE("Legs keep the takeoff direction until landing", "[movement][animation]") {
    using wowee::rendering::airborneLegDirection;
    using wowee::rendering::groundDirection;
    constexpr float halfPi = 1.57079632679f;
    const auto left = groundDirection(false, false, true, false);
    const auto right = groundDirection(false, false, false, true);
    const auto forwardRight = groundDirection(true, false, false, true);
    const auto none = groundDirection(false, false, false, false);
    std::optional<wowee::rendering::GroundDirection> held;

    // Strafing right on the ground, jump, switch to left in the air, release.
    REQUIRE(airborneLegDirection(held, false, right).modelYawOffset == Catch::Approx(-halfPi));
    REQUIRE(airborneLegDirection(held, true, right).modelYawOffset == Catch::Approx(-halfPi));
    REQUIRE(airborneLegDirection(held, true, left).modelYawOffset == Catch::Approx(-halfPi));
    REQUIRE(airborneLegDirection(held, true, forwardRight).modelYawOffset == Catch::Approx(-halfPi));
    REQUIRE(airborneLegDirection(held, true, none).modelYawOffset == Catch::Approx(-halfPi));
    // Landing takes the keys.
    REQUIRE(airborneLegDirection(held, false, left).modelYawOffset == Catch::Approx(halfPi));

    // A standing jump takes the first direction pressed in the air, then holds it.
    REQUIRE_FALSE(airborneLegDirection(held, true, none).moving);
    REQUIRE(airborneLegDirection(held, true, left).modelYawOffset == Catch::Approx(halfPi));
    REQUIRE(airborneLegDirection(held, true, right).modelYawOffset == Catch::Approx(halfPi));
    REQUIRE(airborneLegDirection(held, false, none).modelYawOffset == 0);
}

TEST_CASE("Strafe jump unwinds SpineLow torso counter-rotation while airborne", "[movement][animation]") {
    using wowee::rendering::stepLegYawOffset;
    using wowee::rendering::strafeTorsoYawTarget;
    constexpr float halfPi = 1.57079632679f;
    constexpr float quarterPi = 0.78539816339f;

    // Grounded strafe counter-rotates the torso to preserve gameplay facing.
    REQUIRE(strafeTorsoYawTarget(-halfPi, false, false) == Catch::Approx(halfPi));
    REQUIRE(strafeTorsoYawTarget(halfPi, false, false) == Catch::Approx(-halfPi));
    REQUIRE(strafeTorsoYawTarget(-quarterPi, false, false) == Catch::Approx(quarterPi));
    REQUIRE(strafeTorsoYawTarget(0.0f, false, false) == Catch::Approx(0.0f));

    // Airborne jump or mount clears SpineLow counter-rotation so the whole body
    // aligns with the jump direction without twisting SpineLow.
    REQUIRE(strafeTorsoYawTarget(-halfPi, true, false) == Catch::Approx(0.0f));
    REQUIRE(strafeTorsoYawTarget(halfPi, true, false) == Catch::Approx(0.0f));
    REQUIRE(strafeTorsoYawTarget(-quarterPi, true, false) == Catch::Approx(0.0f));
    REQUIRE(strafeTorsoYawTarget(-halfPi, false, true) == Catch::Approx(0.0f));

    // Stepping from grounded strafe right (+90 deg torso counter-yaw) into an
    // airborne jump smoothly unwinds torso yaw to 0 without a 1-frame snap.
    float torso = halfPi;
    for (int i = 0; i < 20; ++i) {
        torso = stepLegYawOffset(torso, strafeTorsoYawTarget(-halfPi, true, false), 1.0f / 60.0f);
    }
    REQUIRE(std::abs(torso) < 0.01f);
}

TEST_CASE("Strafe and diagonal leg yaw slews smoothly through front", "[movement][animation]") {
    using wowee::rendering::kLegYawDecayPerSec;
    using wowee::rendering::stepLegYawOffset;
    constexpr float pi = 3.14159265358979323846f;
    constexpr float halfPi = 0.5f * pi;
    constexpr float quarterPi = 0.25f * pi;
    constexpr float dt60 = 1.0f / 60.0f;
    const float halfLifeSec = std::log(2.0f) / kLegYawDecayPerSec; // ~40.16 ms

    // Native TBC 2.4.3 at 60 Hz multiplies remaining error by 0.75 per frame:
    // Q (+90 deg) -> E (-90 deg) steps +90 -> +45 -> +11.25 -> -14.0625 -> -33.046875 ...
    float yaw = halfPi;
    const float expectedDeg60[] = {
        45.0f, 11.25f, -14.0625f, -33.046875f, -47.28515625f,
        -57.96386719f, -65.97290039f, -71.97967529f, -76.48475647f, -79.86356735f,
    };
    for (float expDeg : expectedDeg60) {
        const float next = stepLegYawOffset(yaw, -halfPi, dt60);
        REQUIRE(next < yaw);
        REQUIRE(next >= -halfPi);
        REQUIRE(next <= halfPi);
        REQUIRE(next * (180.0f / pi) == Catch::Approx(expDeg).margin(1e-3f));
        yaw = next;
    }

    // Crosses 0 (front) at t_1/2 = ln(2) / k (~40.16 ms), never taking the 180 deg back arc.
    REQUIRE(halfLifeSec == Catch::Approx(0.040157f).margin(1e-5f));
    REQUIRE(stepLegYawOffset(halfPi, -halfPi, halfLifeSec) == Catch::Approx(0.0f).margin(1e-4f));
    REQUIRE(stepLegYawOffset(-halfPi, halfPi, halfLifeSec) == Catch::Approx(0.0f).margin(1e-4f));

    // Diagonal F+L (+45 deg) <-> F+R (-45 deg) and Back+L (-45 deg) <-> Back+R (+45 deg):
    // frame 1 @ 60 Hz hits +-22.5 deg; crosses 0 at the same 40.16 ms half-life.
    REQUIRE(stepLegYawOffset(quarterPi, -quarterPi, dt60) == Catch::Approx(0.5f * quarterPi).margin(1e-4f));
    REQUIRE(stepLegYawOffset(quarterPi, -quarterPi, halfLifeSec) == Catch::Approx(0.0f).margin(1e-4f));
    REQUIRE(stepLegYawOffset(-quarterPi, quarterPi, halfLifeSec) == Catch::Approx(0.0f).margin(1e-4f));

    // Stand (0) -> Strafe L (+90 deg): +22.5 deg on frame 1, +45 deg at half-life.
    REQUIRE(stepLegYawOffset(0.0f, halfPi, dt60) == Catch::Approx(0.5f * quarterPi).margin(1e-4f));
    REQUIRE(stepLegYawOffset(0.0f, halfPi, halfLifeSec) == Catch::Approx(quarterPi).margin(1e-4f));

    // Strafe R (-90 deg) -> Stop (0) unwinds with the exact same exponential decay:
    // after 10 frames @ 60 Hz (166.7 ms), -90 * 0.75^10 = -5.068 deg (Native measured: -5.12 deg).
    float stopYaw = -halfPi;
    for (int i = 0; i < 10; ++i) stopYaw = stepLegYawOffset(stopYaw, 0.0f, dt60);
    REQUIRE(stopYaw * (180.0f / pi) == Catch::Approx(-90.0f * std::pow(0.75f, 10)).margin(1e-2f));

    // Exact semigroup frame-rate invariance across 15, 30, 60, 120, and 240 Hz over 100 ms.
    const auto runFor = [&](int hz) {
        float y = halfPi;
        const int steps = hz / 10;
        for (int i = 0; i < steps; ++i) y = stepLegYawOffset(y, -halfPi, 1.0f / float(hz));
        return y;
    };
    const float ref60 = runFor(60);
    REQUIRE(runFor(30) == Catch::Approx(ref60).margin(1e-4f));
    REQUIRE(runFor(120) == Catch::Approx(ref60).margin(1e-4f));
    REQUIRE(runFor(240) == Catch::Approx(ref60).margin(1e-4f));

    // Non-positive / non-finite dt is a no-op; out-of-range inputs clamp; large dt snaps cleanly to target.
    REQUIRE(stepLegYawOffset(halfPi, -halfPi, 0.0f) == Catch::Approx(halfPi));
    REQUIRE(stepLegYawOffset(halfPi, -halfPi, -0.1f) == Catch::Approx(halfPi));
    REQUIRE(stepLegYawOffset(halfPi, -halfPi, std::numeric_limits<float>::infinity()) == Catch::Approx(halfPi));
    REQUIRE(stepLegYawOffset(halfPi, -halfPi, 1.0f) == Catch::Approx(-halfPi));
    REQUIRE(stepLegYawOffset(3.0f, -3.0f, 1.0f) == Catch::Approx(-halfPi));
}

TEST_CASE("Remote ground stop clears active motion immediately while heartbeats stay continuous", "[entity][movement]") {
    Player p;
    p.setPosition(10.0f, 20.0f, 0.0f, 0.0f);
    // Remote starts moving forward at 7 yd/s
    p.updateRemoteGroundMotion({10.0f, 20.0f, 0.0f}, 0.0f, {7.0f, 0.0f});
    for (int i = 0; i < 30; ++i) {
        p.updateMovement(1.0f / 60.0f);
        REQUIRE(p.isActivelyMoving());
    }
    // Explicit MSG_MOVE_STOP (zero velocity) stops active motion on the exact frame
    p.updateRemoteGroundMotion({p.getX(), p.getY(), p.getZ()}, 0.0f, {0.0f, 0.0f});
    REQUIRE_FALSE(p.isActivelyMoving());
    p.updateMovement(1.0f / 60.0f);
    REQUIRE_FALSE(p.isActivelyMoving());
}

TEST_CASE("Remote jump follows an arc across heartbeats and lands", "[entity][movement]") {
    Player p;
    constexpr float launch = 7.955547f;
    p.setPosition(0, 0, 100, 0);
    p.updateRemoteAirMotion({0, 0, 100}, 0, {0, 0, launch});
    REQUIRE(p.isActivelyMoving());
    REQUIRE(p.isRemoteAirborne());
    for (int i = 0; i < 20; ++i) p.updateMovement(0.025f);
    const float apexSample = 100 + launch * 0.5f - 0.5f * Entity::remoteGravity * 0.25f;
    REQUIRE(p.getZ() == Catch::Approx(apexSample).margin(0.0001f));
    p.updateRemoteAirMotion({0, 0, apexSample}, 0,
                           {0, 0, launch - Entity::remoteGravity * 0.5f});
    p.updateMovement(0.25f);
    REQUIRE(p.getZ() == Catch::Approx(100 + launch * 0.75f - 0.5f * Entity::remoteGravity * 0.75f * 0.75f).margin(0.0001f));
    p.updateRemoteGroundMotion({0, 0, 100}, 0, {0, 0});
    p.updateMovement(0.1f);
    REQUIRE_FALSE(p.isRemoteAirborne());
    REQUIRE_FALSE(p.isActivelyMoving());
    REQUIRE(p.getZ() == Catch::Approx(100));
}

TEST_CASE("Native stationary water jump stays vertical between heartbeats", "[entity][movement]") {
    MovementInfo info;
    info.flags = 4096;
    info.jumpXYSpeed = 4.72222f;
    Player player;
    player.setPosition(898.263f, -8807.09f, 94.2062f, 0.0f);
    const float speed = remoteJumpHorizontalSpeed(info, false);
    player.updateRemoteAirMotion({898.263f, -8807.09f, 94.2062f}, 0.0f,
                                {-0.635923f * speed, 0.771753f * speed, 9.09675f});
    for (int frame = 0; frame < 20; ++frame) player.updateMovement(0.025f);
    CHECK(player.getX() == 898.263f);
    CHECK(player.getY() == -8807.09f);
    CHECK(player.getZ() > 96.0f);
}

TEST_CASE("Remote moving jump keeps momentum below takeoff height", "[entity][movement]") {
    Player p;
    p.setPosition(0, 0, 100, 0);
    p.updateRemoteAirMotion({0, 0, 100}, 0, {3, 4, 7.955547f});
    for (int i = 0; i < 40; ++i) p.updateMovement(0.025f);
    REQUIRE(p.getX() == Catch::Approx(3));
    REQUIRE(p.getY() == Catch::Approx(4));
    REQUIRE(p.getZ() == Catch::Approx(98.30999f).margin(0.0001f));
    REQUIRE(p.isRemoteAirborne());
    p.setPosition(0, 0, 100, 0);
    REQUIRE_FALSE(p.isRemoteAirborne());
}

TEST_CASE("Linear patrols follow corners without cutting across obstacles", "[entity][spline]") {
    Entity entity(1);
    entity.setPosition(0, 0, 0, 0);
    entity.startMoveAlongPath({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}}, 0, 8,
                             false, true);
    entity.updateMovement(2);
    CHECK(entity.getX() == Catch::Approx(5));
    CHECK(entity.getY() == Catch::Approx(0));
    CHECK(entity.getOrientation() == Catch::Approx(0));
    entity.updateMovement(2);
    CHECK(entity.getX() == Catch::Approx(10));
    CHECK(entity.getY() == Catch::Approx(0));
    entity.updateMovement(2);
    CHECK(entity.getX() == Catch::Approx(10));
    CHECK(entity.getY() == Catch::Approx(5));
    CHECK(entity.getOrientation() == Catch::Approx(-1.570796327f));
    entity.updateMovement(2);
    CHECK(entity.getX() == Catch::Approx(10));
    CHECK(entity.getY() == Catch::Approx(10));
}

TEST_CASE("Quest game objects sparkle by their SPARKLE bit, scaled by collision height", "[entity][sparkle]") {
    // CMaNGOS Object::BuildValuesUpdate: ACTIVATE|SPARKLE for a needed chest,
    // goober, generic or spell focus; ACTIVATE alone for a quest giver; zero
    // once nothing needs it.
    CHECK(gameObjectSparkles(GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE));
    CHECK_FALSE(gameObjectSparkles(GO_DYNFLAG_LO_ACTIVATE));
    CHECK_FALSE(gameObjectSparkles(0));
    // WotLK path progress in the high half does not count.
    CHECK_FALSE(gameObjectSparkles(0xFFFF0000u));

    using wowee::rendering::LootSparkles;
    CHECK(LootSparkles::objectScale(0.0f) == 1.0f);                // Bush_Sansam.m2, no box
    CHECK(LootSparkles::objectScale(0.55f, 0.11f) == 1.0f);        // RuinedSteamTankGear01.m2
    CHECK(LootSparkles::objectScale(3.78f) == 3.78f);              // GnomeBigGear.m2
    CHECK(LootSparkles::objectScale(5.79f, 3.16f) == 5.79f);       // AllianceVeteranBanner02.m2
    // Wide solid chest: Gordok Tribute (G_ChestTitan.m2, height 2.34, radius 2.84)
    // must scale past its horizontal walls so LootFX is not occluded inside the lid.
    CHECK(LootSparkles::objectScale(2.34f, 2.84f) == Catch::Approx(4.26f));

    // Unit corpse sparkles: normal mobs stay at 1.0, large boss corpses (e.g.
    // Entropius at scale 6, selection radius 5.03 yd) scale up with the corpse.
    CHECK(LootSparkles::corpseScale(0.0f) == 1.0f);
    CHECK(LootSparkles::corpseScale(0.84f) == 1.0f);
    CHECK(LootSparkles::corpseScale(5.03f) == Catch::Approx(5.03f));
}

TEST_CASE("GameObject 3D quaternion rotation, WotLK packing, and local reach frame", "[entity][gorotation]") {
    GameObject go(42);
    CHECK_FALSE(go.hasRotation());
    const float serverYaw = -1.94604f;
    const float canonicalYaw = wowee::core::coords::serverToCanonicalYaw(serverYaw);
    go.setPosition(10.0f, 20.0f, 30.0f, canonicalYaw);

    // Without an explicit quaternion, effectiveRotation derives Z-yaw from canonical orientation.
    const glm::quat derived = go.effectiveRotation();
    const glm::quat expectedZ(std::cos(serverYaw * 0.5f), 0.0f, 0.0f, std::sin(serverYaw * 0.5f));
    CHECK(std::abs(glm::dot(derived, expectedZ)) == Catch::Approx(1.0f).margin(1e-5));

    // Local offset via quaternion must match the Z-yaw overload identically.
    const glm::vec3 sampleOffset(3.2f, -1.7f, 0.85f);
    const glm::vec3 localYaw = gameObjectLocalOffset(sampleOffset, canonicalYaw);
    const glm::vec3 localQuat = gameObjectLocalOffset(sampleOffset, derived);
    CHECK(localQuat.x == Catch::Approx(localYaw.x).margin(1e-5));
    CHECK(localQuat.y == Catch::Approx(localYaw.y).margin(1e-5));
    CHECK(localQuat.z == Catch::Approx(localYaw.z).margin(1e-5));

    // Explicit 3D quaternion (SFK cell lever 18900): local +X = world +Z, local +Z = wall normal.
    const glm::quat leverQuat(0.69469738f, -0.13189232f, -0.69469738f, -0.13189232f);
    go.setRotation(leverQuat);
    CHECK(go.hasRotation());
    // A point 2 yards straight above the lever (canonical +Z = (0, 0, 2)) lies at local (+2, 0, 0).
    const glm::vec3 aboveLocal = gameObjectLocalOffset(glm::vec3(0.0f, 0.0f, 2.0f), go.effectiveRotation());
    CHECK(aboveLocal.x == Catch::Approx(2.0f).margin(1e-3));
    CHECK(aboveLocal.y == Catch::Approx(0.0f).margin(1e-3));
    CHECK(aboveLocal.z == Catch::Approx(0.0f).margin(1e-3));

    // WotLK packed 64-bit quaternion unpack (negative components + w reconstruction).
    auto packQuat = [](const glm::quat& qIn) -> int64_t {
        glm::quat q = glm::normalize(qIn);
        if (q.w < 0.0f) q = -q;
        const uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(std::llround(q.x * 2097152.0f))) & ((uint64_t(1) << 22) - 1);
        const uint64_t uy = static_cast<uint64_t>(static_cast<int64_t>(std::llround(q.y * 1048576.0f))) & ((uint64_t(1) << 21) - 1);
        const uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(std::llround(q.z * 1048576.0f))) & ((uint64_t(1) << 21) - 1);
        return static_cast<int64_t>(ux | (uy << 22) | (uz << 43));
    };
    const glm::quat unpacked = unpackGameObjectRotation(packQuat(leverQuat));
    CHECK(std::abs(glm::dot(unpacked, glm::normalize(leverQuat))) == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("Unit pet name query triggers on freshly tamed pets with timestamp 0 and on renames",
          "[entity][pet]") {
    Unit u(0xf140002ddf000043ULL);

    // Wild creature / non-pet summon (petNumber == 0): never queries.
    CHECK_FALSE(u.needsPetNameQuery(/*petNumber=*/0u, /*petNameTimestamp=*/0u));
    CHECK_FALSE(u.needsPetNameQuery(/*petNumber=*/0u, /*petNameTimestamp=*/1700000000u));

    // Freshly tamed hunter pet (CMaNGOS Pet::CreateBaseAtCreature):
    // UNIT_FIELD_PETNUMBER != 0 and UNIT_FIELD_PET_NAME_TIMESTAMP == 0 -> must query ("Cat").
    CHECK(u.needsPetNameQuery(11743u, 0u));
    u.queriedPetNumber = 11743u;
    u.queriedPetNameTimestamp = 0u;

    // Repeated VALUES update with same petNumber and timestamp 0 must not re-query.
    CHECK_FALSE(u.needsPetNameQuery(11743u, 0u));

    // Renaming the pet stamps a non-zero UNIT_FIELD_PET_NAME_TIMESTAMP -> must re-query.
    CHECK(u.needsPetNameQuery(11743u, 1795384655u));
    u.queriedPetNameTimestamp = 1795384655u;
    CHECK_FALSE(u.needsPetNameQuery(11743u, 1795384655u));

    // A second rename stamps a newer timestamp -> must re-query again.
    CHECK(u.needsPetNameQuery(11743u, 1795384700u));
    u.queriedPetNameTimestamp = 1795384700u;
    CHECK_FALSE(u.needsPetNameQuery(11743u, 1795384700u));

    // Warlock demon / saved pet loaded from DB arrives with both petNumber and timestamp non-zero.
    Unit demon(0xf140001234000001ULL);
    CHECK(demon.needsPetNameQuery(42u, 1795384600u));
    demon.queriedPetNumber = 42u;
    demon.queriedPetNameTimestamp = 1795384600u;
    CHECK_FALSE(demon.needsPetNameQuery(42u, 1795384600u));
}
