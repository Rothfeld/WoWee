// Regression tests for TBC 2.4.3 creature M2 geometry, spline visibility, and spawn pose parity:
// - Ghaz'an (18105, HydraOutland.m2) active multi-minute patrol spline position & spatial indexing
// - Ragnaros (11502, Ragnaros.m2) Birth (127) gating on CREATE_OBJECT2 vs CREATE_OBJECT range entry
// - HeadlessHorseman.m2 reversed vertexBox ordering and embedded skin batch index clamping
// - Swimming immersion depth fallback across the 228 zero-CollisionHeight TBC CreatureModelData rows
#include <catch_amalgamated.hpp>

#include "core/creature_spawn_pose.hpp"
#include "game/entity.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/m2_model_classifier.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

std::filesystem::path resolveTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p)) return p;
    }
    const std::filesystem::path workspaceDefault(
        "/workspace/azerothcore/client/wowee-data/expansions/tbc");
    if (std::filesystem::exists(workspaceDefault)) return workspaceDefault;
    return {};
}

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

} // namespace

TEST_CASE("Active server patrol spline reports live interpolated position in getLatest and spatial queries",
          "[entity][spline][ghazan]") {
    using namespace wowee::game;

    // Ghaz'an (18105) spawned at (-1634.8, -8118.0, 133.5) with a 2-node 3,387.035 s
    // waypoint spline toward (-446.206, 278.241, 40.6396). While actively moving along
    // the spline, getLatestX/Y/Z and EntityManager::getEntitiesNear must report the
    // live interpolated position near spawn rather than jumping 7,800 yards to moveEnd.
    auto ghazan = std::make_shared<Unit>(0xf1300046b9598af2ULL);
    ghazan->setPosition(-1634.8f, -8118.0f, 133.5f, 0.0f);
    ghazan->startServerSpline(
        {{-1634.8f, -8118.0f, 133.5f}, {-446.206f, 278.241f, 40.6396f}},
        0.0f,
        3387.035f,
        /*smooth=*/false,
        /*followPathFacing=*/true,
        /*splineFlags=*/0x00000100u);
    ghazan->updateMovement(0.4f);

    REQUIRE(ghazan->isEntityMoving());
    REQUIRE(ghazan->isActivelyMoving());
    CHECK(ghazan->getLatestX() == Catch::Approx(ghazan->getX()).margin(1e-4f));
    CHECK(ghazan->getLatestY() == Catch::Approx(ghazan->getY()).margin(1e-4f));
    CHECK(ghazan->getLatestZ() == Catch::Approx(ghazan->getZ()).margin(1e-4f));
    CHECK(ghazan->getLatestX() < -1630.0f);
    CHECK(ghazan->getLatestY() < -8110.0f);

    EntityManager mgr;
    mgr.addEntity(ghazan->getGuid(), ghazan);
    const auto nearby = mgr.getEntitiesNear(-1634.8f, -8096.0f, 150.0f);
    REQUIRE(nearby.size() == 1);
    CHECK(nearby.front()->getGuid() == ghazan->getGuid());
}

TEST_CASE("Linear movement getLatest tracks live interpolation and clamps post-arrival dead-reckoning overrun",
          "[entity][movement]") {
    using namespace wowee::game;

    Entity unit(42);
    unit.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    unit.startMoveTo(10.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    // Mid-segment active interpolation: getLatest matches live getX (5.0), not destination (10.0).
    unit.updateMovement(0.5f);
    REQUIRE(unit.isActivelyMoving());
    CHECK(unit.getX() == Catch::Approx(5.0f));
    CHECK(unit.getLatestX() == Catch::Approx(5.0f));

    // Post-arrival dead-reckoning overrun window (elapsed = 1.4 s > duration = 1.0 s):
    // getX extrapolates past 10.0 while getLatest clamps to authoritative moveEnd (10.0).
    unit.updateMovement(0.9f);
    REQUIRE(unit.isEntityMoving());
    REQUIRE_FALSE(unit.isActivelyMoving());
    CHECK(unit.getX() > 10.0f);
    CHECK(unit.getLatestX() == Catch::Approx(10.0f));
}

TEST_CASE("Creature Birth and Spawn animations play only on newly spawned CREATE_OBJECT2 units",
          "[spawner][ragnaros][animation]") {
    using wowee::core::selectCreatureSpawnPose;
    using wowee::rendering::SpellVisualSystem;
    namespace anim = wowee::rendering::anim;

    // Freshly spawned Ragnaros (CREATE_OBJECT2, unitNewlySpawned = true) plays Birth (127) -> Stand (0).
    const auto freshRagnaros = selectCreatureSpawnPose(
        /*displayChanged=*/false,
        /*unitNewlySpawned=*/true,
        /*hasBirthAnim=*/true,
        /*hasSpawnAnim=*/false,
        /*idleAnim=*/anim::STAND,
        /*auraStateAnim=*/SpellVisualSystem::kNoAnimation,
        /*auraStateNonLooping=*/false);
    CHECK(freshRagnaros.animationId == anim::BIRTH);
    CHECK_FALSE(freshRagnaros.loop);
    CHECK(freshRagnaros.nextAnimationId == anim::STAND);
    CHECK_FALSE(freshRagnaros.holdFinalPose);

    // Already-standing Ragnaros streaming into range (CREATE_OBJECT, unitNewlySpawned = false)
    // starts immediately in Stand (0) instead of plunging underground in Birth (127).
    const auto rangeEntryRagnaros = selectCreatureSpawnPose(
        /*displayChanged=*/false,
        /*unitNewlySpawned=*/false,
        /*hasBirthAnim=*/true,
        /*hasSpawnAnim=*/false,
        /*idleAnim=*/anim::STAND,
        /*auraStateAnim=*/SpellVisualSystem::kNoAnimation,
        /*auraStateNonLooping=*/false);
    CHECK(rangeEntryRagnaros.animationId == anim::STAND);
    CHECK(rangeEntryRagnaros.loop);
    CHECK_FALSE(rangeEntryRagnaros.holdFinalPose);

    // Ragnaros carrying Ragnaros Submerge Visual (20567 -> Submerged 202, non-looping)
    // holds Submerged (202) final pose on both fresh spawn and range entry.
    for (bool newlySpawned : {false, true}) {
        const auto submerged = selectCreatureSpawnPose(
            /*displayChanged=*/false,
            newlySpawned,
            /*hasBirthAnim=*/true,
            /*hasSpawnAnim=*/false,
            /*idleAnim=*/anim::SUBMERGED,
            /*auraStateAnim=*/anim::SUBMERGED,
            /*auraStateNonLooping=*/true);
        CHECK(submerged.animationId == anim::SUBMERGED);
        CHECK_FALSE(submerged.loop);
        CHECK(submerged.holdFinalPose);
    }

    // Spawn (145) models behave identically: play on CREATE_OBJECT2, skip on range entry and display swap.
    const auto freshSpawnModel = selectCreatureSpawnPose(
        false, true, false, true, anim::STAND, SpellVisualSystem::kNoAnimation, false);
    CHECK(freshSpawnModel.animationId == anim::SPAWN);
    CHECK_FALSE(freshSpawnModel.loop);
    CHECK(freshSpawnModel.nextAnimationId == anim::STAND);

    const auto swappedDisplay = selectCreatureSpawnPose(
        true, true, true, true, anim::STAND, SpellVisualSystem::kNoAnimation, false);
    CHECK(swappedDisplay.animationId == anim::STAND);
    CHECK(swappedDisplay.loop);
}

TEST_CASE("Swimming immersion depth falls back safely when CreatureModelData CollisionHeight is zero",
          "[spawner][swimming]") {
    using wowee::core::resolveUnitSwimmingDepth;

    // HydraOutland.m2 (CreatureModelData 1951): CollisionHeight = 7.77778, scale = 1.0.
    CHECK(resolveUnitSwimmingDepth(7.77778f, 10.2f, 1.0f) == Catch::Approx(0.75f * 7.77778f));

    // Infernal.m2 (CreatureModelData 169): DBC CollisionHeight = 0.0, M2 boundMax.z = 2.0313.
    CHECK(resolveUnitSwimmingDepth(0.0f, 2.0313f, 1.5f) == Catch::Approx(0.75f * 2.0313f * 1.5f));

    // Degenerate zero DBC height and unloaded/flat M2 bounds still return a positive depth.
    CHECK(resolveUnitSwimmingDepth(0.0f, 0.0f, 1.0f) == Catch::Approx(1.5f));
}

TEST_CASE("Shipped TBC Ragnaros, HydraOutland, and HeadlessHorseman M2 models load valid geometry and ordered bounds",
          "[m2][assets][ragnaros][ghazan]") {
    const auto root = resolveTbcDataDir();
    if (root.empty()) SKIP("TBC data tree not available");

    SECTION("Ragnaros.m2 has intact embedded skin geometry, Stand (0), and Birth (127)") {
        const auto bytes = readBytes(root / "creature/ragnaros/ragnaros.m2");
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.isValid());
        CHECK(model.version >= 260);
        CHECK(model.version < 264);
        CHECK(model.vertices.size() == 2172);
        CHECK_FALSE(model.indices.empty());
        CHECK(model.batches.size() == 18);
        CHECK(model.vertexBoxMin.x <= model.vertexBoxMax.x);
        CHECK(model.vertexBoxMin.y <= model.vertexBoxMax.y);
        CHECK(model.vertexBoxMin.z <= model.vertexBoxMax.z);
        for (const auto& b : model.batches) {
            CHECK(static_cast<size_t>(b.indexStart) + b.indexCount <= model.indices.size());
        }
        const bool hasStand = std::any_of(model.sequences.begin(), model.sequences.end(),
            [](const auto& s) { return s.id == wowee::rendering::anim::STAND; });
        const bool hasBirth = std::any_of(model.sequences.begin(), model.sequences.end(),
            [](const auto& s) { return s.id == wowee::rendering::anim::BIRTH; });
        CHECK(hasStand);
        CHECK(hasBirth);
    }

    SECTION("HydraOutland.m2 (Ghaz'an 18105 / Markaru 20775) has intact geometry and locomotion sequences") {
        const auto bytes = readBytes(root / "creature/hydraoutland/hydraoutland.m2");
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.isValid());
        CHECK(model.vertices.size() > 1000);
        CHECK_FALSE(model.indices.empty());
        CHECK_FALSE(model.batches.empty());
        CHECK(model.vertexRadius > 1.0f);
        CHECK(model.vertexBoxMin.x <= model.vertexBoxMax.x);
        CHECK(model.vertexBoxMin.y <= model.vertexBoxMax.y);
        CHECK(model.vertexBoxMin.z <= model.vertexBoxMax.z);
        for (const auto& b : model.batches) {
            CHECK(static_cast<size_t>(b.indexStart) + b.indexCount <= model.indices.size());
        }
        const bool hasStand = std::any_of(model.sequences.begin(), model.sequences.end(),
            [](const auto& s) { return s.id == wowee::rendering::anim::STAND; });
        const bool hasWalk = std::any_of(model.sequences.begin(), model.sequences.end(),
            [](const auto& s) { return s.id == wowee::rendering::anim::WALK; });
        CHECK(hasStand);
        CHECK(hasWalk);
    }

    SECTION("HeadlessHorseman.m2 reversed header vertexBox Z corners are normalized to min <= max") {
        const auto bytes = readBytes(root / "creature/headlesshorseman/headlesshorseman.m2");
        REQUIRE_FALSE(bytes.empty());
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.isValid());
        CHECK(model.vertices.size() == 1695);
        CHECK_FALSE(model.indices.empty());
        CHECK_FALSE(model.batches.empty());
        CHECK(model.vertexBoxMin.x <= model.vertexBoxMax.x);
        CHECK(model.vertexBoxMin.y <= model.vertexBoxMax.y);
        CHECK(model.vertexBoxMin.z < 0.0f);
        CHECK(model.vertexBoxMax.z > 0.0f);
        CHECK(model.vertexBoxMin.z <= model.vertexBoxMax.z);
    }
}
