// TBC 2.4.3 grounded-entity vertical placement and floor arbitration parity tests.
#include "rendering/entity_floor_arbitration.hpp"
#include <catch_amalgamated.hpp>

using wowee::rendering::arbitrateEntityFloor;
using wowee::rendering::kEntityFloorTerrainExactTolerance;

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

TEST_CASE("stationary creature inside non-collidable web tent grounds to terrain within 3 yd",
          "[collision][floor][tbc]") {
    // Bloodmyst Isle Webbed Creature guid 63432 inside nastyspiderweb_yellow.m2
    // (0 authored collision triangles): serverZ = 138.527, terrain = 136.308 (+2.219 yd).
    // Verified on Native TBC 2.4.3: grounds to 136.308 so the cocoon stays enclosed
    // under the web tent canopy instead of poking 2 yd through its roof; spawns more
    // than 3 yd above terrain without support preserve authoritative server Z.
    const glm::vec3 tentPos(-10672.3f, -1514.5f, 138.527f);
    const auto inTent = arbitrateEntityFloor(
        tentPos, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/136.308f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    REQUIRE(inTent.has_value());
    CHECK(*inTent == Catch::Approx(136.308f));

    const glm::vec3 highElevated(-10672.3f, -1514.5f, 140.0f);
    const auto preserved = arbitrateEntityFloor(
        highElevated, std::nullopt, /*isSplineOrMoving=*/false,
        /*terrainHeight=*/136.308f, /*wmoHeight=*/std::nullopt, /*m2Height=*/std::nullopt);
    CHECK_FALSE(preserved.has_value());
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
