// TBC 2.4.3 grounded-entity vertical placement and floor arbitration parity tests.
#include "rendering/entity_floor_arbitration.hpp"
#include <catch_amalgamated.hpp>

using wowee::rendering::arbitrateEntityFloor;
using wowee::rendering::kEntityFloorTerrainExactTolerance;
using wowee::rendering::kMoveFlagFalling;
using wowee::rendering::kMoveFlagFallingFar;
using wowee::rendering::kMoveFlagFlying;
using wowee::rendering::kMoveFlagHover;
using wowee::rendering::kMoveFlagLevitating;
using wowee::rendering::kMoveFlagOnTransport;
using wowee::rendering::kMoveFlagSwimming;
using wowee::rendering::kSplineFlagFalling;
using wowee::rendering::kSplineFlagFlying;
using wowee::rendering::unitSuppressesFloorSnap;

TEST_CASE("stationary creature on M2 mound stands on mound top rather than sinking to terrain",
          "[collision][floor][tbc]") {
    // Bloodmyst Isle Webbed Creature (entry 17680, guid 63434):
    // serverZ = 132.747, terrain = 132.039 (+0.708 yd), spidereggsground_yellow M2 top = 132.766.
    const glm::vec3 serverPos(-10682.4f, -1608.59f, 132.747f);
    const auto chosen = arbitrateEntityFloor(
        serverPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/132.039f, /*wmoHeight=*/std::nullopt, /*m2Height=*/132.766f);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(132.766f));

    SECTION("low M2 mound (+0.327 yd above terrain, guid 63410) exceeds terrain exact fast-path") {
        // guid 63410: serverZ = 99.798, terrain = 99.471 (+0.327 yd), M2 mound = 99.805.
        CHECK(std::abs(99.471f - 99.798f) > kEntityFloorTerrainExactTolerance);
        const glm::vec3 lowMoundPos(-10738.1f, -1616.14f, 99.798f);
        const auto lowChosen = arbitrateEntityFloor(
            lowMoundPos, std::nullopt, /*isSplineOrMoving=*/false,
            /*terrainHeight=*/99.471f, /*wmoHeight=*/std::nullopt, /*m2Height=*/99.805f);
        REQUIRE(lowChosen.has_value());
        CHECK(*lowChosen == Catch::Approx(99.805f));
    }
}

TEST_CASE("stationary creature recovers onto streamed M2/WMO floor after initial terrain frame",
          "[collision][floor][tbc]") {
    // Frame 1: creature spawns at ~90 yd before ADT M2 doodad finishes async upload,
    // temporarily grounding onto raw terrain (132.039).
    const glm::vec3 serverPos(-10682.4f, -1608.59f, 132.747f);
    const auto frame1 = arbitrateEntityFloor(
        serverPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/132.039f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(frame1.has_value());
    CHECK(*frame1 == Catch::Approx(132.039f));

    // Frame 2: M2 doodad collision finishes loading at 132.766 (matching serverZ within 0.02 yd).
    // Stationary unit must lift onto the authored M2 support instead of staying locked to terrain.
    const glm::vec3 stalePrevious(serverPos.x, serverPos.y, *frame1);
    const auto frame2 = arbitrateEntityFloor(
        serverPos, stalePrevious, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/132.039f, /*wmoHeight=*/std::nullopt, /*m2Height=*/132.766f);
    REQUIRE(frame2.has_value());
    CHECK(*frame2 == Catch::Approx(132.766f));
}

TEST_CASE("stationary creature inside non-collidable web tent grounds to terrain",
          "[collision][floor][tbc]") {
    // Bloodmyst Isle Webbed Creature guid 63432 inside nastyspiderweb_yellow.m2
    // (0 authored collision triangles): serverZ = 138.527, terrain = 136.308 (+2.219 yd).
    const glm::vec3 tentPos(-10672.3f, -1514.5f, 138.527f);
    const auto inTent = arbitrateEntityFloor(
        tentPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/136.308f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(inTent.has_value());
    CHECK(*inTent == Catch::Approx(136.308f));

    // Unloaded deep-pit WMO interiors (> kEntityFloorMaxDrop = 50 yd above raw terrain,
    // such as Auchindoun Levixus at -8.92 over -101.40 pit) preserve server Z while WMO streams.
    const glm::vec3 auchindounUnloadedWmo(-3565.73f, 4943.27f, -8.92f);
    const auto preserved = arbitrateEntityFloor(
        auchindounUnloadedWmo, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/-101.40f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    CHECK_FALSE(preserved.has_value());
}

TEST_CASE("elevated non-levitating TBC creature spawns snap down to ground matching Native 2.4.3",
          "[collision][floor][tbc]") {
    // 1. Nexus-King Salhadaar (entry 20454, guid 72462, Ethereum Staging Grounds):
    // InhabitType = 3 (GROUND | WATER), moveFlags = 0x100 (WALKING), serverZ = 115.753,
    // ADT terrain = 108.194 (+7.559 yd). Verified side-by-side on WOWEE_PAIR=9:
    // Native TBC 2.4.3 snaps immediately to ground (108.194).
    CHECK_FALSE(unitSuppressesFloorSnap(/*moveFlags=*/0x100u));
    const glm::vec3 salhadaarPos(2494.05f, 3881.92f, 115.753f);
    const auto salhadaarFloor = arbitrateEntityFloor(
        salhadaarPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/108.194f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(salhadaarFloor.has_value());
    CHECK(*salhadaarFloor == Catch::Approx(108.194f));

    // 2. Shimmerwing Moth (entry 20611, guid 72870 & 72866, Eco-Dome Midrealm):
    // moveFlags = 0x100, serverZ = 144.247 (+14.043 yd) and 174.280 (+19.727 yd).
    const glm::vec3 moth14Pos(3115.23f, 3522.40f, 144.247f);
    const auto moth14Floor = arbitrateEntityFloor(
        moth14Pos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/130.204f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(moth14Floor.has_value());
    CHECK(*moth14Floor == Catch::Approx(130.204f));

    // When an already-grounded moth starts a ground spline whose initial server node still
    // carries stale elevated Z, continuity with previousRenderPos keeps it on terrain.
    const glm::vec3 prevGroundedMoth(3115.23f, 3522.40f, 130.204f);
    const auto mothSplineStart = arbitrateEntityFloor(
        moth14Pos, prevGroundedMoth, /*isSplineOrMoving=*/true,
        /*terrainHeight=*/130.204f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(mothSplineStart.has_value());
    CHECK(*mothSplineStart == Catch::Approx(130.204f));

    const glm::vec3 moth20Pos(2766.27f, 3438.47f, 174.280f);
    const auto moth20Floor = arbitrateEntityFloor(
        moth20Pos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/154.553f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(moth20Floor.has_value());
    CHECK(*moth20Floor == Catch::Approx(154.553f));

    // 3. Mana Snapper (entry 18883, guid 67733, Netherstorm Crumbling Waste):
    // serverZ = 110.971 (+4.47 yd above netherstorm_overhangrock_small_03.m2 at 106.50,
    // while raw ADT terrain abyss is -1.984). Must ground onto the collidable M2 rock.
    const glm::vec3 manaSnapperPos(2494.45f, 2833.43f, 110.971f);
    const auto snapperFloor = arbitrateEntityFloor(
        manaSnapperPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/-1.984f, /*wmoHeight=*/std::nullopt, /*m2Height=*/106.50f);
    REQUIRE(snapperFloor.has_value());
    CHECK(*snapperFloor == Catch::Approx(106.50f));
}

TEST_CASE("airborne, levitating, and hovering movement flags suppress floor snap",
          "[collision][floor][tbc]") {
    // Ground walking (0x100) and ground CAN_FLY (0x01000100) still snap to floor.
    CHECK_FALSE(unitSuppressesFloorSnap(0x0u));
    CHECK_FALSE(unitSuppressesFloorSnap(0x100u));
    CHECK_FALSE(unitSuppressesFloorSnap(0x01000100u));

    // LEVITATING (0x400), HOVER (0x40000000, e.g. Ethereum Prisoner Tyralius 20825
    // 0x40000100 or Priest Levitate 1706), FLYING, FALLING, SWIMMING, ONTRANSPORT suppress snap.
    CHECK(unitSuppressesFloorSnap(kMoveFlagLevitating));
    CHECK(unitSuppressesFloorSnap(kMoveFlagHover | 0x100u));
    CHECK(unitSuppressesFloorSnap(kMoveFlagFlying));
    CHECK(unitSuppressesFloorSnap(kMoveFlagFalling));
    CHECK(unitSuppressesFloorSnap(kMoveFlagFallingFar));
    CHECK(unitSuppressesFloorSnap(kMoveFlagSwimming));
    CHECK(unitSuppressesFloorSnap(kMoveFlagOnTransport));

    // Active airborne splines (Flying 0x200 / Falling 0x002) suppress snap only while moving.
    CHECK(unitSuppressesFloorSnap(0x100u, kSplineFlagFlying, /*activelyMoving=*/true));
    CHECK(unitSuppressesFloorSnap(0x100u, kSplineFlagFalling, /*activelyMoving=*/true));
    CHECK_FALSE(unitSuppressesFloorSnap(0x100u, kSplineFlagFlying, /*activelyMoving=*/false));
}

TEST_CASE("elevated bridge mover preserves Z across transient 1-frame seam miss",
          "[collision][floor][tbc]") {
    const glm::vec3 prevBridge(500.0f, 600.0f, 115.0f);
    const glm::vec3 bridgeStep(500.2f, 600.0f, 115.0f);
    const auto seamFrame = arbitrateEntityFloor(
        bridgeStep, prevBridge, /*isSplineOrMoving=*/true,
        /*terrainHeight=*/100.0f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    CHECK_FALSE(seamFrame.has_value());
}

TEST_CASE("ordinary stationary creature on uneven terrain grounds across heightmap quantization",
          "[collision][floor][tbc]") {
    // Coldridge Valley Ragged Young Wolf (guid 402): serverZ = 386.733, terrain = 386.131 (+0.602 yd).
    const glm::vec3 wolfPos(395.645f, -6245.87f, 386.733f);
    const auto chosen = arbitrateEntityFloor(
        wolfPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/386.131f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(386.131f));
}

TEST_CASE("stationary creature under overhead structure stays on ground",
          "[collision][floor][tbc]") {
    const glm::vec3 underArch(10.0f, 20.0f, 50.0f);
    const auto chosen = arbitrateEntityFloor(
        underArch, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/49.25f, /*wmoHeight=*/50.65f, /*m2Height=*/std::nullopt);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(49.25f));
}

TEST_CASE("MoveCharge spline +1 yd lift next to low M2 fence stays on grass",
          "[collision][floor][tbc]") {
    // CMaNGOS MoveCharge adds +1.0 yd to spline Z (renderPos.z = 11.0 while grass is 10.0).
    // A nearby 0.95 yd M2 fence post overlaps the query box; continuity with previous grass Z
    // must keep the charging/arrived unit on grass (10.0) instead of popping onto the fence.
    const glm::vec3 prevGrass(100.0f, 200.0f, 10.0f);
    const glm::vec3 chargePos(100.1f, 200.0f, 11.0f);
    const auto chosen = arbitrateEntityFloor(
        chargePos, prevGrass, /*isSplineOrMoving=*/true,
        /*terrainHeight=*/10.0f, /*wmoHeight=*/std::nullopt, /*m2Height=*/10.95f);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(10.0f));
}

TEST_CASE("mover in WMO tunnel ignores high terrain roof above",
          "[collision][floor][tbc]") {
    const glm::vec3 prevTunnel(0.0f, 0.0f, 40.0f);
    const glm::vec3 tunnelPos(0.5f, 0.0f, 40.15f);
    const auto chosen = arbitrateEntityFloor(
        tunnelPos, prevTunnel, /*isSplineOrMoving=*/true,
        /*terrainHeight=*/95.0f, /*wmoHeight=*/40.02f, /*m2Height=*/std::nullopt);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(40.02f));
}

TEST_CASE("corpse resting on low M2 stone ledge snaps onto ledge even after approaching on grass",
          "[collision][floor][tbc]") {
    // Wetlands graveyard lochmodanstonefence01.m2: grass terrain at 18.71, stone wall top at 19.94.
    // Once dead (isSplineOrMoving = false), a corpse whose server Z is on the stone ledge must
    // rest on the M2 stone ledge rather than staying locked to previous grass Z.
    const glm::vec3 prevGrass(-3302.0f, -2449.0f, 18.71f);
    const glm::vec3 corpseOnWall(-3302.1f, -2449.0f, 19.94f);
    const auto chosen = arbitrateEntityFloor(
        corpseOnWall, prevGrass, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/18.71f, /*wmoHeight=*/std::nullopt, /*m2Height=*/19.94f);
    REQUIRE(chosen.has_value());
    CHECK(*chosen == Catch::Approx(19.94f));
}

TEST_CASE("grounded creature spawned above WMO dungeon floor drops onto floor within drop reach",
          "[collision][floor][tbc]") {
    // Auchenai Crypts (WMO-only interior, no ADT terrain): Exarch Maladaar's Stolen Soul
    // can spawn at serverZ = 28.6193 (+2.03 yd above the WMO floor at 26.5893).
    const glm::vec3 spawnedPos(63.1268f, -381.365f, 28.6193f);

    // First frame (no previousRenderPos): must ground onto the WMO floor below it.
    const auto firstFrame = arbitrateEntityFloor(
        spawnedPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/std::nullopt, /*wmoHeight=*/26.5893f, /*m2Height=*/std::nullopt);
    REQUIRE(firstFrame.has_value());
    CHECK(*firstFrame == Catch::Approx(26.5893f));

    // Subsequent frame if WMO collision arrived after the first frame: must still ground.
    const auto lateWmoFrame = arbitrateEntityFloor(
        spawnedPos, spawnedPos, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/std::nullopt, /*wmoHeight=*/26.5893f, /*m2Height=*/std::nullopt);
    REQUIRE(lateWmoFrame.has_value());
    CHECK(*lateWmoFrame == Catch::Approx(26.5893f));

    // Spawns more than kEntityFloorMaxDrop (50.0 yd) above the WMO floor preserve server Z.
    const glm::vec3 tooHigh(63.1268f, -381.365f, 80.0f);
    const auto preserved = arbitrateEntityFloor(
        tooHigh, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/std::nullopt, /*wmoHeight=*/26.5893f, /*m2Height=*/std::nullopt);
    CHECK_FALSE(preserved.has_value());

    // Overhead WMO ceiling above the unit must never pull the unit upward.
    const glm::vec3 underCeiling(63.1268f, -381.365f, 26.5893f);
    const auto ceilingRejected = arbitrateEntityFloor(
        underCeiling, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/std::nullopt, /*wmoHeight=*/27.8f, /*m2Height=*/std::nullopt);
    CHECK_FALSE(ceilingRejected.has_value());
}
