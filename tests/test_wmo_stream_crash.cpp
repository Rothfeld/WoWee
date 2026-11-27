// Regression tests for WMORenderer incremental streaming lifecycle across
// clearAll(), unloadModel(), shutdown(), and source-model mismatch recovery.
// Covers Wave-4 Side-Discovery Shard 1: SIGSEGV in WMORenderer::loadModelIncremental
// when streaming Stormwind/Elwynn WMO 2814845793 (human_barracks.wmo) on Map 0 tile [30, 49].

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/wmo_renderer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

wowee::pipeline::WMOModel makeMultiGroupSyntheticWmo(uint32_t groupCount,
                                                     uint32_t quadsPerGroup,
                                                     const std::string& sourcePath) {
    wowee::pipeline::WMOModel model{};
    model.version = 17;
    model.nGroups = groupCount;
    model.nTextures = 2;
    model.sourcePath = sourcePath;
    model.boundingBoxMin = glm::vec3(-20.0f, -20.0f, 0.0f);
    model.boundingBoxMax = glm::vec3(20.0f * static_cast<float>(groupCount), 20.0f, 10.0f);

    model.textures = {"DUNGEONS\\TEXTURES\\WALLS\\MM_BARRACKS_WALL_01.BLP",
                      "DUNGEONS\\TEXTURES\\WALLS\\MM_BARRACKS_WALL_02.BLP"};
    model.textureOffsetToIndex[0] = 0;
    model.textureOffsetToIndex[48] = 1;

    wowee::pipeline::WMOMaterial m0{};
    m0.texture1 = 0;
    wowee::pipeline::WMOMaterial m1{};
    m1.texture1 = 48;
    model.materials = {m0, m1};

    model.groups.resize(groupCount);
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        auto& g = model.groups[gi];
        g.groupId = 1000u + gi;
        g.flags = 0x8u;  // outdoor
        const float baseX = static_cast<float>(gi) * 16.0f;
        g.boundingBoxMin = glm::vec3(baseX - 8.0f, -8.0f, 0.0f);
        g.boundingBoxMax = glm::vec3(baseX + 8.0f, 8.0f, 4.0f);

        g.vertices.reserve(quadsPerGroup * 4u);
        g.indices.reserve(quadsPerGroup * 6u);
        for (uint32_t q = 0; q < quadsPerGroup; ++q) {
            const uint16_t vBase = static_cast<uint16_t>(g.vertices.size());
            const float z = 1.0f + 0.001f * static_cast<float>(q % 16u);
            wowee::pipeline::WMOVertex v0{}, v1{}, v2{}, v3{};
            v0.position = glm::vec3(baseX - 6.0f, -6.0f, z);
            v1.position = glm::vec3(baseX + 6.0f, -6.0f, z);
            v2.position = glm::vec3(baseX + 6.0f, 6.0f, z);
            v3.position = glm::vec3(baseX - 6.0f, 6.0f, z);
            v0.normal = v1.normal = v2.normal = v3.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v0.texCoord = glm::vec2(0.0f, 0.0f);
            v1.texCoord = glm::vec2(1.0f, 0.0f);
            v2.texCoord = glm::vec2(1.0f, 1.0f);
            v3.texCoord = glm::vec2(0.0f, 1.0f);
            g.vertices.push_back(v0);
            g.vertices.push_back(v1);
            g.vertices.push_back(v2);
            g.vertices.push_back(v3);

            g.indices.push_back(vBase + 0u);
            g.indices.push_back(vBase + 1u);
            g.indices.push_back(vBase + 2u);
            g.indices.push_back(vBase + 0u);
            g.indices.push_back(vBase + 2u);
            g.indices.push_back(vBase + 3u);
            g.triFlags.push_back(0);
            g.triFlags.push_back(0);
            g.triMaterials.push_back(static_cast<uint8_t>(gi & 1u));
            g.triMaterials.push_back(static_cast<uint8_t>(gi & 1u));
        }

        wowee::pipeline::WMOBatch batch{};
        batch.startIndex = 0;
        batch.indexCount = static_cast<uint16_t>(g.indices.size());
        batch.startVertex = 0;
        batch.lastVertex = static_cast<uint16_t>(g.vertices.size() - 1u);
        batch.materialId = static_cast<uint8_t>(gi & 1u);
        g.batches.push_back(batch);
    }
    return model;
}

std::vector<uint8_t> readDiskBinary(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> out(sz);
    if (sz > 0 && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz))) {
        return {};
    }
    return out;
}

}  // namespace

TEST_CASE("WMORenderer clears in-progress loadingModels_ on clearAll, unloadModel, and shutdown",
          "[wmo-stream][sidedisc4]") {
    using wowee::rendering::WMORenderer;
    constexpr uint32_t kModelId = 2814845793u;
    const auto model = makeMultiGroupSyntheticWmo(
        16u, 512u, "world\\wmo\\azeroth\\buildings\\human_barracks\\human_barracks.wmo");

    WMORenderer wmo;

    SECTION("clearAll discards mid-stream model so subsequent map load starts fresh with all groups") {
        // Tiny budget forces an incremental yield after the first group.
        const auto firstStep = wmo.loadModelIncremental(model, kModelId, 0.0001f);
        REQUIRE(firstStep == WMORenderer::ModelLoadResult::InProgress);
        CHECK(wmo.isModelLoading(kModelId));
        CHECK(wmo.getLoadingModelCount() == 1u);
        CHECK_FALSE(wmo.isModelLoaded(kModelId));

        // Cross-map teleport runs clearAll(): must wipe loadingModels_ so no stale
        // textures or half-uploaded groups survive into the next map load.
        wmo.clearAll();
        CHECK_FALSE(wmo.isModelLoading(kModelId));
        CHECK(wmo.getLoadingModelCount() == 0u);
        CHECK_FALSE(wmo.isModelLoaded(kModelId));

        // Re-stream incrementally on return to Map 0 and verify every group's floor exists.
        int slices = 0;
        while (wmo.loadModelIncremental(model, kModelId, 0.0001f) ==
               WMORenderer::ModelLoadResult::InProgress) {
            ++slices;
            REQUIRE(slices < 64);
        }
        CHECK(slices >= 1);
        CHECK(wmo.isModelLoaded(kModelId));
        CHECK_FALSE(wmo.isModelLoading(kModelId));

        const uint32_t instId = wmo.createInstance(kModelId, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
        REQUIRE(instId != 0u);
        // Group 0 (x = 0) and Group 15 (x = 240) must both have walkable collision geometry.
        const auto floorFirst = wmo.getFloorHeight(0.0f, 0.0f, 5.0f);
        const auto floorLast = wmo.getFloorHeight(15.0f * 16.0f, 0.0f, 5.0f);
        REQUIRE(floorFirst.has_value());
        REQUIRE(floorLast.has_value());
        CHECK(*floorFirst == Catch::Approx(1.0f).margin(0.05f));
        CHECK(*floorLast == Catch::Approx(1.0f).margin(0.05f));
    }

    SECTION("unloadModel and shutdown clean up mid-stream loadingModels_ entries") {
        REQUIRE(wmo.loadModelIncremental(model, kModelId, 0.0001f) ==
                WMORenderer::ModelLoadResult::InProgress);
        REQUIRE(wmo.isModelLoading(kModelId));

        wmo.unloadModel(kModelId);
        CHECK_FALSE(wmo.isModelLoading(kModelId));
        CHECK(wmo.getLoadingModelCount() == 0u);

        REQUIRE(wmo.loadModelIncremental(model, kModelId, 0.0001f) ==
                WMORenderer::ModelLoadResult::InProgress);
        REQUIRE(wmo.isModelLoading(kModelId));

        wmo.shutdown();
        CHECK_FALSE(wmo.isModelLoading(kModelId));
        CHECK(wmo.getLoadingModelCount() == 0u);
    }

    SECTION("Mismatched incoming WMO on same modelId restarts incremental state cleanly") {
        REQUIRE(wmo.loadModelIncremental(model, kModelId, 0.0001f) ==
                WMORenderer::ModelLoadResult::InProgress);
        REQUIRE(wmo.isModelLoading(kModelId));

        const auto differentModel = makeMultiGroupSyntheticWmo(
            6u, 256u, "world\\wmo\\azeroth\\buildings\\human_farm\\farm.wmo");
        REQUIRE(wmo.loadModel(differentModel, kModelId));
        CHECK(wmo.isModelLoaded(kModelId));
        CHECK_FALSE(wmo.isModelLoading(kModelId));

        const uint32_t instId = wmo.createInstance(kModelId, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
        REQUIRE(instId != 0u);
        CHECK(wmo.getFloorHeight(0.0f, 0.0f, 5.0f).has_value());
        CHECK(wmo.getFloorHeight(5.0f * 16.0f, 0.0f, 5.0f).has_value());
        CHECK_FALSE(wmo.getFloorHeight(12.0f * 16.0f, 0.0f, 5.0f).has_value());
    }
}

TEST_CASE("Shipped TBC human_barracks.wmo (2814845793, 28 groups, 27 textures) survives mid-stream clearAll",
          "[wmo-stream][sidedisc4]") {
    const char* envTbc = std::getenv("WOWEE_TBC_DATA_DIR");
    const std::filesystem::path tbcRoot = envTbc
        ? std::filesystem::path(envTbc)
        : std::filesystem::path("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    const auto rootDiskPath = tbcRoot / "world/wmo/azeroth/buildings/human_barracks/human_barracks.wmo";
    if (!std::filesystem::exists(rootDiskPath)) {
        SKIP("Shipped TBC human_barracks.wmo not present at " + rootDiskPath.string());
    }

    const std::string canonicalRelPath = "world\\wmo\\azeroth\\buildings\\human_barracks\\human_barracks.wmo";
    const uint32_t hashedId = static_cast<uint32_t>(std::hash<std::string>{}(canonicalRelPath));
    CHECK(hashedId == 2814845793u);

    const auto rootBytes = readDiskBinary(rootDiskPath);
    REQUIRE_FALSE(rootBytes.empty());
    auto barracks = wowee::pipeline::WMOLoader::load(rootBytes);
    barracks.sourcePath = canonicalRelPath;
    REQUIRE(barracks.nGroups == 28u);
    CHECK(barracks.textures.size() == 27u);

    uint32_t loadedDiskGroups = 0;
    for (uint32_t gi = 0; gi < barracks.nGroups; ++gi) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
        const auto gp = tbcRoot / ("world/wmo/azeroth/buildings/human_barracks/human_barracks" + std::string(suffix));
        const auto gBytes = readDiskBinary(gp);
        REQUIRE_FALSE(gBytes.empty());
        REQUIRE(wowee::pipeline::WMOLoader::loadGroup(gBytes, barracks, gi));
        ++loadedDiskGroups;
    }
    REQUIRE(loadedDiskGroups == 28u);

    wowee::rendering::WMORenderer wmo;
    // Pause mid-stream on 2814845793, simulate cross-map clearAll(), then complete streaming.
    REQUIRE(wmo.loadModelIncremental(barracks, hashedId, 0.0001f) ==
            wowee::rendering::WMORenderer::ModelLoadResult::InProgress);
    CHECK(wmo.isModelLoading(hashedId));

    wmo.clearAll();
    CHECK_FALSE(wmo.isModelLoading(hashedId));
    CHECK(wmo.getLoadingModelCount() == 0u);

    int steps = 0;
    while (wmo.loadModelIncremental(barracks, hashedId, 0.0001f) ==
           wowee::rendering::WMORenderer::ModelLoadResult::InProgress) {
        ++steps;
        REQUIRE(steps < 64);
    }
    CHECK(steps >= 1);
    CHECK(steps <= 27);
    CHECK(wmo.isModelLoaded(hashedId));
    CHECK_FALSE(wmo.isModelLoading(hashedId));

    const uint32_t instId = wmo.createInstance(
        hashedId, glm::vec3(-9051.0f, 154.0f, 115.2f), glm::vec3(0.0f), 1.0f);
    REQUIRE(instId != 0u);
    CHECK(wmo.instanceHasCollisionGeometry(instId));
}
