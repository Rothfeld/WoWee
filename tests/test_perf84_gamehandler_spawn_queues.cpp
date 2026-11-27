#include <catch2/catch_amalgamated.hpp>

#include "core/entity_spawner_audio_budget.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "pipeline/asset_manager.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace wowee::pipeline {
AssetManager::AssetManager() = default;
AssetManager::~AssetManager() = default;
std::vector<uint8_t> AssetManager::readFile(const std::string&) const { return {}; }
std::vector<uint8_t> AssetManager::readFileOptional(const std::string&) const { return {}; }
} // namespace wowee::pipeline

using namespace wowee;

namespace {

pipeline::M2Model makeTestUnitAudioModel() {
    pipeline::M2Model model;
    model.sequences.resize(4);
    // seq 0: Stand (1000 ms, zero audio timestamps)
    model.sequences[0].id = 0;
    model.sequences[0].duration = 1000;
    // seq 1: Run (800 ms, footsteps at 200 & 600 ms, armor foley at 200 ms)
    model.sequences[1].id = 5;
    model.sequences[1].duration = 800;
    // seq 2: Attack1H (1000 ms, combat swing at 350 ms)
    model.sequences[2].id = 16;
    model.sequences[2].duration = 1000;
    // seq 3: Death (1200 ms, death thud at 700 ms)
    model.sequences[3].id = 1;
    model.sequences[3].duration = 1200;

    pipeline::M2Model::AudioEvent fsd{};
    fsd.type = pipeline::M2AudioEventType::Footstep;
    fsd.times = {{}, {200u, 600u}, {}, {}};
    model.audioEvents.push_back(fsd);

    pipeline::M2Model::AudioEvent trd{};
    trd.type = pipeline::M2AudioEventType::ArmorFoley;
    trd.times = {{}, {200u}, {}, {}};
    model.audioEvents.push_back(trd);

    pipeline::M2Model::AudioEvent css{};
    css.type = pipeline::M2AudioEventType::CombatSwing;
    css.times = {{}, {}, {350u}, {}};
    model.audioEvents.push_back(css);

    pipeline::M2Model::AudioEvent dth{};
    dth.type = pipeline::M2AudioEventType::DeathThud;
    dth.times = {{}, {}, {}, {700u}};
    model.audioEvents.push_back(dth);

    return model;
}

} // namespace

TEST_CASE("Unit audio step budget skips Stand sequences and non-crossing Run ticks",
          "[perf84][entity_spawner][audio]") {
    const auto model = makeTestUnitAudioModel();
    int prevSeq = -1;
    float prevTimeMs = 0.0f;

    // Stand (seq 0) has zero character audio events across the model's event tracks.
    for (float t : {0.0f, 16.0f, 350.0f, 900.0f}) {
        const auto mask = core::evaluateUnitLayerAudioStep(
            model, /*seqIdx=*/0, t, /*reversed=*/false, /*isUpperLayer=*/false,
            /*skipFootstep=*/false, /*hasCustomBridge=*/true, /*muteEvents=*/false,
            prevSeq, prevTimeMs);
        CHECK_FALSE(mask.sequenceHasEvents);
        CHECK_FALSE(mask.crossedAny);
        CHECK_FALSE(mask.needsSurface);
        CHECK_FALSE(mask.needsBridge);
        CHECK(prevSeq == -1);
    }

    // Enter Run (seq 1) near t=50 ms: sequenceHasEvents is true, no timestamp crossed yet.
    auto m0 = core::evaluateUnitLayerAudioStep(
        model, 1, 50.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    CHECK(m0.sequenceHasEvents);
    CHECK_FALSE(m0.crossedAny);
    CHECK_FALSE(m0.needsSurface);
    CHECK_FALSE(m0.needsBridge);
    CHECK(prevSeq == 1);

    // Advance 50 -> 190 ms: still before the 200 ms footstep, no surface query needed.
    auto m1 = core::evaluateUnitLayerAudioStep(
        model, 1, 190.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    CHECK(m1.sequenceHasEvents);
    CHECK_FALSE(m1.crossedAny);
    CHECK_FALSE(m1.needsSurface);

    // Advance 190 -> 210 ms: crosses Footstep + ArmorFoley at 200 ms -> needsSurface = true.
    auto m2 = core::evaluateUnitLayerAudioStep(
        model, 1, 210.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    CHECK(m2.sequenceHasEvents);
    CHECK(m2.crossedAny);
    CHECK(m2.needsSurface);
    CHECK_FALSE(m2.needsBridge);

    // Advance 210 -> 590 ms in 20 ms ticks: 19 consecutive ticks need zero surface raycasts.
    for (float t = 230.0f; t <= 590.0f; t += 20.0f) {
        auto step = core::evaluateUnitLayerAudioStep(
            model, 1, t, false, false, false, true, false, prevSeq, prevTimeMs);
        CHECK(step.sequenceHasEvents);
        CHECK_FALSE(step.crossedAny);
        CHECK_FALSE(step.needsSurface);
    }

    // Advance 590 -> 610 ms: crosses second Footstep at 600 ms.
    auto m3 = core::evaluateUnitLayerAudioStep(
        model, 1, 610.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    CHECK(m3.crossedAny);
    CHECK(m3.needsSurface);

    // Wrap 790 -> 20 ms across loop boundary: neither 200 nor 600 is in (790..800] U [0..20].
    (void)core::evaluateUnitLayerAudioStep(
        model, 1, 790.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    auto mWrap = core::evaluateUnitLayerAudioStep(
        model, 1, 20.0f, false, false, false, true, false, prevSeq, prevTimeMs);
    CHECK_FALSE(mWrap.crossedAny);
    CHECK_FALSE(mWrap.needsSurface);
}

TEST_CASE("Unit audio step budget respects skipFootstep, DeathThud, upper layer, reversed, and mid-entry sync",
          "[perf84][entity_spawner][audio]") {
    const auto model = makeTestUnitAudioModel();

    SECTION("skipFootstep suppresses Footstep surface raycasts while keeping ArmorFoley and DeathThud") {
        int prevSeq = 1;
        float prevTimeMs = 580.0f;
        // At 600 ms only Footstep exists (no ArmorFoley); with skipFootstep=true nothing fires.
        auto fsdOnly = core::evaluateUnitLayerAudioStep(
            model, 1, 610.0f, false, false, /*skipFootstep=*/true, true, false,
            prevSeq, prevTimeMs);
        CHECK(fsdOnly.sequenceHasEvents);
        CHECK_FALSE(fsdOnly.crossedAny);
        CHECK_FALSE(fsdOnly.needsSurface);

        // At 200 ms ArmorFoley also exists: crossedAny is true, but needsSurface stays false!
        prevTimeMs = 180.0f;
        auto fsdAndTrd = core::evaluateUnitLayerAudioStep(
            model, 1, 210.0f, false, false, /*skipFootstep=*/true, true, false,
            prevSeq, prevTimeMs);
        CHECK(fsdAndTrd.crossedAny);
        CHECK_FALSE(fsdAndTrd.needsSurface);

        // DeathThud (seq 3 at 700 ms) still requires surface sample even when skipFootstep=true.
        prevSeq = 3;
        prevTimeMs = 680.0f;
        auto deathStep = core::evaluateUnitLayerAudioStep(
            model, 3, 710.0f, false, false, /*skipFootstep=*/true, true, false,
            prevSeq, prevTimeMs);
        CHECK(deathStep.crossedAny);
        CHECK(deathStep.needsSurface);
    }

    SECTION("Upper-body overlay suppresses Footstep and ArmorFoley but dispatches CombatSwing bridge") {
        int prevSeq = 1;
        float prevTimeMs = 180.0f;
        auto upperRun = core::evaluateUnitLayerAudioStep(
            model, 1, 210.0f, false, /*isUpperLayer=*/true, false, true, false,
            prevSeq, prevTimeMs);
        CHECK_FALSE(upperRun.crossedAny);
        CHECK_FALSE(upperRun.needsSurface);

        prevSeq = 2;
        prevTimeMs = 330.0f;
        auto upperSwing = core::evaluateUnitLayerAudioStep(
            model, 2, 360.0f, false, /*isUpperLayer=*/true, false, /*hasCustomBridge=*/true, false,
            prevSeq, prevTimeMs);
        CHECK(upperSwing.crossedAny);
        CHECK(upperSwing.needsBridge);
        CHECK_FALSE(upperSwing.needsSurface);
    }

    SECTION("Mid-sequence entry (>250 ms) syncs cursor without retroactively firing past events") {
        int prevSeq = -1;
        float prevTimeMs = 0.0f;
        auto midEntry = core::evaluateUnitLayerAudioStep(
            model, 1, 450.0f, false, false, false, true, false, prevSeq, prevTimeMs);
        CHECK(midEntry.sequenceHasEvents);
        CHECK_FALSE(midEntry.crossedAny);
        CHECK(prevSeq == 1);
        CHECK(prevTimeMs == Catch::Approx(450.0f));
    }

    SECTION("Reversed backpedal maps timestamps against sequence duration") {
        // Duration = 800 ms; raw footstep at 600 ms maps to 200 ms from the end,
        // which crosses as reversed playback steps toMs from 620 ms down to 580 ms.
        int prevSeq = 1;
        float prevTimeMs = 620.0f;
        auto revStep = core::evaluateUnitLayerAudioStep(
            model, 1, 580.0f, /*reversed=*/true, false, false, true, false,
            prevSeq, prevTimeMs);
        CHECK(revStep.crossedAny);
        CHECK(revStep.needsSurface);
    }

    SECTION("Muted distant units (>65 yd) advance cursor without crossing or surface queries") {
        int prevSeq = 1;
        float prevTimeMs = 180.0f;
        auto muted = core::evaluateUnitLayerAudioStep(
            model, 1, 220.0f, false, false, false, true, /*muteEvents=*/true,
            prevSeq, prevTimeMs);
        CHECK(muted.sequenceHasEvents);
        CHECK_FALSE(muted.crossedAny);
        CHECK_FALSE(muted.needsSurface);
        CHECK(prevTimeMs == Catch::Approx(220.0f));
    }
}

TEST_CASE("Position sample cache invalidates only past configured yard threshold",
          "[perf84][entity_spawner][audio]") {
    const glm::vec3 origin(-8772.12f, -174.23f, 82.66f);
    CHECK(core::shouldRefreshCachedPositionSample(false, origin, origin, 0.20f));
    CHECK_FALSE(core::shouldRefreshCachedPositionSample(
        true, origin, origin + glm::vec3(0.10f, 0.10f, 0.0f), 0.20f));
    CHECK(core::shouldRefreshCachedPositionSample(
        true, origin, origin + glm::vec3(0.21f, 0.0f, 0.0f), 0.20f));
    CHECK_FALSE(core::shouldRefreshCachedPositionSample(
        true, origin, origin + glm::vec3(0.30f, 0.30f, 0.0f), 0.50f));
    CHECK(core::shouldRefreshCachedPositionSample(
        true, origin, origin + glm::vec3(0.51f, 0.0f, 0.0f), 0.50f));
}

TEST_CASE("Transport render horizon culls cross-continent ships while preserving harbor and ridden hulls",
          "[perf84][transport][game_handler]") {
    // Shattrath viewer vs Exodar boat entry=181646 at Azuremyst (~16,950 yards away on map 530).
    const glm::vec3 shattrath(-1838.16f, 5301.79f, -12.43f);
    const glm::vec3 azuremystDock(-4265.0f, -11472.0f, 0.0f);
    CHECK(game::shouldCullTransportRenderPush(
        azuremystDock, shattrath, /*hasViewerPos=*/true, /*carryingRider=*/false));

    // Northshire viewer vs Grom'gol zeppelin entry=176495 (~3,650 yards away on map 0).
    const glm::vec3 northshire(-8921.09f, -125.63f, 82.05f);
    const glm::vec3 gromgol(-12432.0f, 206.0f, 31.0f);
    CHECK(game::shouldCullTransportRenderPush(
        gromgol, northshire, /*hasViewerPos=*/true, /*carryingRider=*/false));

    // Harbor approach within 2,500 yards (exceeds 2x max farclip 1277 yd) stays visible.
    const glm::vec3 harborApproach = northshire + glm::vec3(2400.0f, 0.0f, 0.0f);
    CHECK_FALSE(game::shouldCullTransportRenderPush(
        harborApproach, northshire, /*hasViewerPos=*/true, /*carryingRider=*/false));

    // Ridden transport is NEVER culled regardless of viewer distance.
    CHECK_FALSE(game::shouldCullTransportRenderPush(
        azuremystDock, shattrath, /*hasViewerPos=*/true, /*carryingRider=*/true));

    // Unknown viewer position fails open (never hides hull).
    CHECK_FALSE(game::shouldCullTransportRenderPush(
        azuremystDock, glm::vec3(0.0f), /*hasViewerPos=*/false, /*carryingRider=*/false));
}

TEST_CASE("Transport transform deduplication skips unchanged dock poses and detects movement",
          "[perf84][transport][game_handler]") {
    const glm::mat4 base = glm::translate(glm::mat4(1.0f), glm::vec3(-4265.0f, 11472.0f, 0.0f));
    CHECK(game::transportTransformUnchanged(base, base));

    const glm::mat4 subEpsilonShift =
        glm::translate(glm::mat4(1.0f), glm::vec3(-4265.0f + 1e-5f, 11472.0f, 0.0f));
    CHECK(game::transportTransformUnchanged(base, subEpsilonShift));

    const glm::mat4 moved =
        glm::translate(glm::mat4(1.0f), glm::vec3(-4264.95f, 11472.0f, 0.0f));
    CHECK_FALSE(game::transportTransformUnchanged(base, moved));

    const glm::mat4 rotated = glm::rotate(base, 0.001f, glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_FALSE(game::transportTransformUnchanged(base, rotated));
}

TEST_CASE("Vehicle MO_TRANSPORT displays skip origin TransportAnimation inference (Deeprun 176085)",
          "[perf84][transport][entity_spawner]") {
    game::TransportPathRepository repo;
    // Deeprun Tram 176085 local-offset path starts at (0, 0, 0), matching every
    // MO_TRANSPORT CREATE_OBJECT spawn position (0, 0, 0) sent by MaNGOS.
    std::vector<math::SplineKey> tramKeys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {71666, {-2481.65f, 0.0f, -1.02749f}},
        {143333, {0.0f, 0.0f, 0.0f}},
    };
    repo.storePath(176085u, game::PathEntry(
        math::CatmullRomSpline(std::move(tramKeys), false), 176085u,
        /*zOnly=*/false, /*dbc=*/true, /*worldCoords=*/false));

    // Unguarded spawn inference at (0, 0, 0) matches 176085 at distance 0.
    REQUIRE(repo.inferDbcPathForSpawn(glm::vec3(0.0f), 1200.0f, false) == 176085u);

    // Every ship, zeppelin, and Naxxramas MO_TRANSPORT display on Map 0 / Map 530
    // is classified as a vehicle transport and therefore bypasses inferDbcPathForSpawn.
    for (uint32_t displayId : {3015u, 3031u, 6637u, 7087u, 7446u, 7546u, 7636u}) {
        INFO("vehicle displayId=" << displayId);
        CHECK(game::isVehicleTransportDisplay(displayId));
        CHECK(repo.pickFallbackMovingPath(999999u, displayId) == 0u);
    }

    // Non-vehicle local transports (Deeprun subway car 3831, lifts 455 / 462) remain eligible.
    CHECK_FALSE(game::isVehicleTransportDisplay(3831u));
    CHECK_FALSE(game::isVehicleTransportDisplay(455u));
    CHECK_FALSE(game::isVehicleTransportDisplay(462u));
}
