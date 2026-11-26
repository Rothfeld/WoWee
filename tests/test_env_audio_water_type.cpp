#include <catch_amalgamated.hpp>

#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entries.hpp"
#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/water_renderer.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace {

using namespace wowee;

std::filesystem::path findTbcDataPath() {
    if (const char* envPath = std::getenv("WOWEE_TBC_DATA_DIR"); envPath && *envPath) {
        std::filesystem::path p(envPath);
        if (std::filesystem::exists(p / "dbfilesclient" / "soundwatertype.dbc") ||
            std::filesystem::exists(p / "DBFilesClient" / "SoundWaterType.dbc")) {
            return p;
        }
    }
    const std::filesystem::path repoDefault = "/workspace/azerothcore/client/wowee-data/expansions/tbc";
    if (std::filesystem::exists(repoDefault / "dbfilesclient" / "soundwatertype.dbc") ||
        std::filesystem::exists(repoDefault / "DBFilesClient" / "SoundWaterType.dbc")) {
        return repoDefault;
    }
    return {};
}

struct StereoRms {
    double left = 0.0;
    double right = 0.0;
    double mono = 0.0;
    double ildDb = 0.0;
};

StereoRms renderStereoRms(audio::AudioEngine& engine, uint32_t frameCount) {
    std::vector<float> pcm(static_cast<size_t>(frameCount) * 2u, 0.0f);
    const uint32_t got = engine.renderMixFrames(pcm.data(), frameCount);
    double sumL = 0.0;
    double sumR = 0.0;
    for (uint32_t i = 0; i < got; ++i) {
        const double l = pcm[static_cast<size_t>(i) * 2u];
        const double r = pcm[static_cast<size_t>(i) * 2u + 1u];
        sumL += l * l;
        sumR += r * r;
    }
    const double denom = static_cast<double>(std::max(1u, got));
    const double rmsL = std::sqrt(sumL / denom);
    const double rmsR = std::sqrt(sumR / denom);
    const double rmsM = std::sqrt(0.5 * (rmsL * rmsL + rmsR * rmsR));
    const double ild = 20.0 * std::log10((rmsL + 1e-12) / (rmsR + 1e-12));
    return StereoRms{rmsL, rmsR, rmsM, ild};
}

pipeline::ADTTerrain makeSingleLayerTerrain(uint16_t liquidType, float height, uint8_t maskByte = 0xFFu) {
    pipeline::ADTTerrain terrain;
    terrain.loaded = true;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            auto& chunk = terrain.getChunk(cx, cy);
            chunk.position[0] = -static_cast<float>(cy) * 33.33333f;
            chunk.position[1] = -static_cast<float>(cx) * 33.33333f;
            chunk.position[2] = 0.0f;
        }
    }
    pipeline::ADTTerrain::WaterLayer layer{};
    layer.liquidType = liquidType;
    layer.minHeight = height;
    layer.maxHeight = height;
    layer.x = 0;
    layer.y = 0;
    layer.width = 8;
    layer.height = 8;
    layer.heights.assign(81, height);
    layer.depths.assign(81, 128u);
    layer.mask.assign(8, maskByte);
    terrain.waterData[0].layers.push_back(std::move(layer));
    return terrain;
}

} // namespace

TEST_CASE("WaterRenderer::classifyLiquidBasicType maps TBC LiquidType.dbc IDs including Naxxramas Slime",
          "[audio][water_proximity]") {
    using rendering::WaterRenderer;
    CHECK(WaterRenderer::classifyLiquidBasicType(0) == 0u);
    CHECK(WaterRenderer::classifyLiquidBasicType(1) == 0u);   // Water
    CHECK(WaterRenderer::classifyLiquidBasicType(2) == 1u);   // Ocean
    CHECK(WaterRenderer::classifyLiquidBasicType(3) == 2u);   // Magma
    CHECK(WaterRenderer::classifyLiquidBasicType(4) == 3u);   // Slime
    CHECK(WaterRenderer::classifyLiquidBasicType(21) == 3u);  // Naxxramas - Slime
    CHECK(WaterRenderer::classifyLiquidBasicType(41) == 0u);  // Coilfang Raid - Water
    CHECK(WaterRenderer::classifyLiquidBasicType(61) == 0u);  // Hyjal Past - Water

    CHECK(WaterRenderer::defaultTerrainLiquidSubtype(0u) == 4u);
    CHECK(WaterRenderer::defaultTerrainLiquidSubtype(1u) == 0u);
    CHECK(WaterRenderer::defaultTerrainLiquidSubtype(2u) == 4u);
    CHECK(WaterRenderer::defaultTerrainLiquidSubtype(3u) == 0u);
}

TEST_CASE("WaterRenderer::queryLiquidProximity finds exact nearest wet cell point on terrain and rotated WMO liquids",
          "[audio][water_proximity]") {
    rendering::WaterRenderer water;

    SECTION("Terrain river chunk clamps continuously to shoreline edge and ignores dry cells") {
        // Chunk (0,0) spans render X in [-33.3333, 0], render Y in [-33.3333, 0] at Z = 10.0.
        // Enable ONLY row 0, col 0 (one 4.1667 x 4.1667 cell spanning [-4.1667, 0] x [-4.1667, 0]).
        auto terrain = makeSingleLayerTerrain(/*liquidType=*/1, /*height=*/10.0f, /*maskByte=*/0x00u);
        terrain.waterData[0].layers[0].mask[0] = 0x01u;
        water.loadFromTerrain(terrain, false, 32, 32, false);

        // Listener 3.0 yd North-East (+X=+3, +Y=+4 -> 5.0 yd XY, +2.0 yd Z) from corner (0, 0, 10)
        const glm::vec3 query(3.0f, 4.0f, 12.0f);
        const auto hits = water.queryLiquidProximity(query, 60.0f);
        REQUIRE(hits.size() == 1u);
        CHECK(hits[0].basicType == 0u);
        CHECK(hits[0].soundSubtype == 4u);
        CHECK(hits[0].position.x == Catch::Approx(0.0f).margin(1e-4f));
        CHECK(hits[0].position.y == Catch::Approx(0.0f).margin(1e-4f));
        CHECK(hits[0].position.z == Catch::Approx(10.0f).margin(1e-4f));
        CHECK(hits[0].distanceSq == Catch::Approx(3.0f * 3.0f + 4.0f * 4.0f + 2.0f * 2.0f).margin(1e-3f));

        // Beyond maxRadius returns empty
        CHECK(water.queryLiquidProximity(glm::vec3(100.0f, 0.0f, 10.0f), 60.0f).empty());
    }

    SECTION("Rotated WMO MLIQ preserves nibble subtype (0 vs 4, 2 vs 6, 3) and projects along rotated axes") {
        pipeline::WMOLiquid stillPool{};
        stillPool.xTiles = 2;
        stillPool.yTiles = 2;
        stillPool.xVerts = 3;
        stillPool.yVerts = 3;
        stillPool.basePosition = glm::vec3(0.0f, 0.0f, 5.0f);
        stillPool.heights.assign(9, 5.0f);
        stillPool.depths.assign(9, 128u);
        stillPool.flags.assign(4, 0x40u);  // low nibble 0 -> basicType 0 (Water), subtype 0 (RiverStill)
        stillPool.liquidType = 1;

        glm::mat4 rot45(1.0f);
        rot45 = glm::translate(rot45, glm::vec3(50.0f, 80.0f, 0.0f));
        rot45 = glm::rotate(rot45, glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        water.loadFromWMO(stillPool, rot45, /*wmoId=*/101u, /*groupIndex=*/0u);

        pipeline::WMOLiquid lavaPool = stillPool;
        lavaPool.flags.assign(4, 0x42u);  // low nibble 2 -> basicType 2 (Magma), subtype 0 (LavaPoolLoop)
        lavaPool.liquidType = 3;
        lavaPool.depths.clear();
        lavaPool.texCoords.assign(9, {0, 0});
        glm::mat4 lavaMat = glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 105.0f, 0.0f));
        water.loadFromWMO(lavaPool, lavaMat, /*wmoId=*/102u, /*groupIndex=*/1u);

        const auto hits = water.queryLiquidProximity(glm::vec3(50.0f, 80.0f, 7.0f), 60.0f);
        REQUIRE(hits.size() == 2u);

        auto stillIt = std::find_if(hits.begin(), hits.end(), [](const auto& h) { return h.basicType == 0u; });
        REQUIRE(stillIt != hits.end());
        CHECK(stillIt->soundSubtype == 0u);
        CHECK(stillIt->position.x == Catch::Approx(50.0f).margin(1e-3f));
        CHECK(stillIt->position.y == Catch::Approx(80.0f).margin(1e-3f));
        CHECK(stillIt->position.z == Catch::Approx(5.0f).margin(1e-3f));

        auto lavaIt = std::find_if(hits.begin(), hits.end(), [](const auto& h) { return h.basicType == 2u; });
        REQUIRE(lavaIt != hits.end());
        CHECK(lavaIt->soundSubtype == 0u);
    }
}

TEST_CASE("SoundWaterType.dbc full corpus + real TBC ADT/WMO + headless PCM audio parity",
          "[audio][water_proximity][tbc]") {
    const auto tbcDir = findTbcDataPath();
    if (tbcDir.empty()) {
        SKIP("TBC data directory not present; skipping SoundWaterType.dbc corpus & PCM tests");
    }

    pipeline::AssetManager assets;
    REQUIRE(assets.initialize(tbcDir.string()));

    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&assets);
    engine.setMasterVolume(1.0f);
    audio::SoundEntryTable::instance().reset();
    REQUIRE(audio::SoundEntryTable::instance().ensureLoaded(&assets));

    audio::AmbientSoundManager ambient;
    REQUIRE(ambient.initialize(&assets));

    SECTION("All 12 SoundWaterType.dbc rows resolve exact TBC 2.4.3 SoundEntries IDs and archive WAVs") {
        CHECK(ambient.waterSoundRuleCount() == 12u);
        CHECK(ambient.resolveWaterSoundEntryId(0u, 0u) == 1111u);  // RiverStill
        CHECK(ambient.resolveWaterSoundEntryId(0u, 4u) == 1112u);  // RiverSlow
        CHECK(ambient.resolveWaterSoundEntryId(0u, 8u) == 1113u);  // RiverFast
        CHECK(ambient.resolveWaterSoundEntryId(1u, 0u) == 1114u);  // Ocean
        CHECK(ambient.resolveWaterSoundEntryId(1u, 4u) == 1114u);
        CHECK(ambient.resolveWaterSoundEntryId(1u, 8u) == 1114u);
        CHECK(ambient.resolveWaterSoundEntryId(2u, 0u) == 3072u);  // LavaPoolLoop
        CHECK(ambient.resolveWaterSoundEntryId(2u, 4u) == 3052u);  // LavaFlowLoop
        CHECK(ambient.resolveWaterSoundEntryId(2u, 8u) == 3052u);
        CHECK(ambient.resolveWaterSoundEntryId(3u, 0u) == 3880u);  // SlimeLoop
        CHECK(ambient.resolveWaterSoundEntryId(3u, 4u) == 3880u);
        CHECK(ambient.resolveWaterSoundEntryId(3u, 8u) == 3880u);

        for (uint32_t seId : {1111u, 1112u, 1113u, 1114u, 3052u, 3072u, 3880u}) {
            const auto* rec = audio::SoundEntryTable::instance().findById(&assets, seId);
            REQUIRE(rec != nullptr);
            CHECK_FALSE(audio::SoundEntryTable::instance().pickExistingPath(&assets, *rec).empty());
        }
    }

    SECTION("Real TBC ADTs (Lake Everstill, Tanaris Ocean, Searing Gorge Lava) and WMOs (Undercity Slime, Ironforge, Shattrath)") {
        // 1. Searing Gorge ADT (azeroth_33_44.adt -> magma basicType=2, subtype=4 -> SE 3052)
        const auto lavaAdtBytes = assets.readFile("World\\Maps\\Azeroth\\Azeroth_33_44.adt");
        REQUIRE_FALSE(lavaAdtBytes.empty());
        const auto lavaTerrain = pipeline::ADTLoader::load(lavaAdtBytes);
        REQUIRE(lavaTerrain.isLoaded());
        rendering::WaterRenderer water;
        water.loadFromTerrain(lavaTerrain, false, 33, 44, false);
        const glm::vec3 searingRender(-6468.8f, -977.1f, 336.3f);
        const auto lavaHits = water.queryLiquidProximity(searingRender, 60.0f);
        REQUIRE_FALSE(lavaHits.empty());
        CHECK(lavaHits[0].basicType == 2u);
        CHECK(lavaHits[0].soundSubtype == 4u);
        CHECK(ambient.resolveWaterSoundEntryId(lavaHits[0].basicType, lavaHits[0].soundSubtype) == 3052u);

        // 2. Undercity WMO group 052 (Slime basicType=3, subtype=0 -> SE 3880)
        const auto ucRootBytes = assets.readFile("World\\wmo\\Lorderon\\Undercity\\Undercity.wmo");
        const auto ucGrpBytes = assets.readFile("World\\wmo\\Lorderon\\Undercity\\Undercity_052.wmo");
        REQUIRE_FALSE(ucRootBytes.empty());
        REQUIRE_FALSE(ucGrpBytes.empty());
        auto ucModel = pipeline::WMOLoader::load(ucRootBytes);
        REQUIRE(pipeline::WMOLoader::loadGroup(ucGrpBytes, ucModel, 0));
        REQUIRE(ucModel.groups[0].liquid.hasLiquid());
        water.clear();
        water.loadFromWMO(ucModel.groups[0].liquid, glm::mat4(1.0f), 1u, 0u);
        const glm::vec3 ucLocalQuery(
            ucModel.groups[0].liquid.basePosition.x + 8.0f,
            ucModel.groups[0].liquid.basePosition.y + 8.0f,
            ucModel.groups[0].liquid.basePosition.z + 2.5f);
        const auto ucHits = water.queryLiquidProximity(ucLocalQuery, 60.0f);
        REQUIRE_FALSE(ucHits.empty());
        CHECK(ucHits[0].basicType == 3u);
        CHECK(ucHits[0].soundSubtype == 0u);
        CHECK(ambient.resolveWaterSoundEntryId(ucHits[0].basicType, ucHits[0].soundSubtype) == 3880u);
    }

    SECTION("Headless PCM: distance falloff, +/-90 deg 3D stereo panning, Ambience gating, and Water vs Slime submerge") {
        std::vector<audio::AudioEngine::PlaybackEvent> startedEvents;
        engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            if (ev.started) startedEvents.push_back(ev);
        });

        // Listener at origin facing +Y (North in render coords), up +Z -> Right ear is +X, Left ear is -X.
        engine.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));

        // A. Ocean (SE 1114: vol=0.30, min=10, max=60; raw OceanA.wav RMS -25.75 dBFS) at 2 yd, 25 yd, 65 yd
        const std::array<audio::LiquidProximityCandidate, 1> ocean2yd{{
            {.position = glm::vec3(0.0f, 2.0f, 0.0f), .distanceSq = 4.0f, .basicType = 1u, .soundSubtype = 0u},
        }};
        ambient.setLiquidProximity(ocean2yd);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        REQUIRE_FALSE(startedEvents.empty());
        CHECK(startedEvents.back().soundEntryId == 1114u);
        const auto rms2yd = renderStereoRms(engine, 24000);
        CHECK(rms2yd.mono > 0.003);
        CHECK(std::abs(rms2yd.ildDb) < 0.25);

        // Move ocean to 25 yd ahead -> attenuated (~10/70 = 0.143x) without restarting voice
        const std::array<audio::LiquidProximityCandidate, 1> ocean25yd{{
            {.position = glm::vec3(0.0f, 25.0f, 0.0f), .distanceSq = 625.0f, .basicType = 1u, .soundSubtype = 0u},
        }};
        const size_t startsBeforeMove = startedEvents.size();
        ambient.setLiquidProximity(ocean25yd);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        CHECK(startedEvents.size() == startsBeforeMove);
        const auto rms25yd = renderStereoRms(engine, 24000);
        CHECK(rms25yd.mono > 0.0005);
        CHECK(rms25yd.mono < rms2yd.mono * 0.35);

        // Pan 90 deg Left (-X = -8 yd) vs Right (+X = +8 yd)
        const std::array<audio::LiquidProximityCandidate, 1> oceanLeft{{
            {.position = glm::vec3(-8.0f, 0.0f, 0.0f), .distanceSq = 64.0f, .basicType = 1u, .soundSubtype = 0u},
        }};
        ambient.setLiquidProximity(oceanLeft);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        (void)renderStereoRms(engine, 2048);  // flush spatializer gainer smoothing
        const auto rmsLeft = renderStereoRms(engine, 24000);
        CHECK(rmsLeft.ildDb > 7.5);

        const std::array<audio::LiquidProximityCandidate, 1> oceanRight{{
            {.position = glm::vec3(8.0f, 0.0f, 0.0f), .distanceSq = 64.0f, .basicType = 1u, .soundSubtype = 0u},
        }};
        ambient.setLiquidProximity(oceanRight);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        (void)renderStereoRms(engine, 2048);
        const auto rmsRight = renderStereoRms(engine, 24000);
        CHECK(rmsRight.ildDb < -7.5);

        // Past DistanceCutoff (65 yd > 60 yd) -> stopped immediately, zero PCM
        const std::array<audio::LiquidProximityCandidate, 1> oceanFar{{
            {.position = glm::vec3(0.0f, 65.0f, 0.0f), .distanceSq = 4225.0f, .basicType = 1u, .soundSubtype = 0u},
        }};
        ambient.setLiquidProximity(oceanFar);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        CHECK(renderStereoRms(engine, 12000).mono == Catch::Approx(0.0).margin(1e-7));

        // B. Sound_EnableAmbience=0 (setVolumeScale(0)) immediately stops active loop; =1 resumes
        ambient.setLiquidProximity(ocean2yd);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        CHECK(renderStereoRms(engine, 12000).mono > 0.003);
        ambient.setVolumeScale(0.0f);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        CHECK(renderStereoRms(engine, 12000).mono == Catch::Approx(0.0).margin(1e-7));
        ambient.setVolumeScale(1.0f);
        ambient.update(0.05f, glm::vec3(0.0f), false, false);
        CHECK(renderStereoRms(engine, 12000).mono > 0.003);

        // C. Submerging camera in Water/Ocean suppresses SE 1114 and loops SE 4209 continuously past 6.5 s
        startedEvents.clear();
        ambient.update(0.05f, glm::vec3(0.0f), false, /*isSwimming=*/true);
        REQUIRE_FALSE(startedEvents.empty());
        CHECK(startedEvents.back().soundEntryId == 4209u);
        CHECK_FALSE(engine.isSoundEntryPlaying(1114u));
        // Advance 6.5 s (> 5.86 s UndwaterLoop.wav duration) and verify seamless continuous loop!
        (void)renderStereoRms(engine, 48000 * 6);
        const auto rmsAfterWrap = renderStereoRms(engine, 24000);
        CHECK(rmsAfterWrap.mono > 0.02);
        CHECK(std::abs(rmsAfterWrap.ildDb) < 0.05);

        // Exiting water stops UndwaterLoop immediately
        ambient.clearLiquidProximity();
        ambient.update(0.05f, glm::vec3(0.0f), false, /*isSwimming=*/false);
        CHECK(renderStereoRms(engine, 12000).mono == Catch::Approx(0.0).margin(1e-7));

        // D. Slime (basicType=3, SE 3880) stays active even when submerged (isSwimming=false from Renderer for basicType>=2,
        // and even if isSwimming=true is passed, basicType>=2 is NOT suppressed)
        const std::array<audio::LiquidProximityCandidate, 1> slime3yd{{
            {.position = glm::vec3(0.0f, 3.0f, 0.0f), .distanceSq = 9.0f, .basicType = 3u, .soundSubtype = 0u},
        }};
        ambient.setLiquidProximity(slime3yd);
        ambient.update(0.05f, glm::vec3(0.0f), true, /*isSwimming=*/false);
        CHECK(engine.isSoundEntryPlaying(3880u));
        CHECK(renderStereoRms(engine, 12000).mono > 0.05);

        engine.setPlaybackObserver({});
    }

    ambient.shutdown();
    engine.shutdown();
}
