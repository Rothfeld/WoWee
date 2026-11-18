// Entity, Unit, Player, GameObject, EntityManager tests
#include <catch_amalgamated.hpp>
#include "game/entity.hpp"
#include "game/remote_jump.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/animation/ground_direction.hpp"
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
    CHECK(LootSparkles::objectScale(0.0f) == 1.0f);    // Bush_Sansam.m2, no box
    CHECK(LootSparkles::objectScale(0.55f) == 1.0f);   // RuinedSteamTankGear01.m2
    CHECK(LootSparkles::objectScale(3.78f) == 3.78f);  // GnomeBigGear.m2
    CHECK(LootSparkles::objectScale(5.79f) == 5.79f);  // AllianceVeteranBanner02.m2
}
