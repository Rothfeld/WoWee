#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "pipeline/adt_alpha.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/ground_clutter_scatter.hpp"

using wowee::pipeline::ADTLoader;
using wowee::pipeline::ADTTerrain;
using wowee::pipeline::decodeLayerAlpha;
using wowee::pipeline::effectiveGroundEffectDensity;
using wowee::pipeline::GroundClutterEffectSpec;
using wowee::pipeline::GroundClutterSample;
using wowee::pipeline::MapChunk;
using wowee::pipeline::scatterChunkGroundClutter;

namespace {

MapChunk makeUniformChunk(uint32_t effectId, uint64_t noEffectDoodad = 0, uint16_t holes = 0) {
    MapChunk chunk;
    chunk.heightMap.loaded = true;
    chunk.heightMap.heights.fill(0.0f);
    chunk.noEffectDoodad = noEffectDoodad;
    chunk.holes = holes;
    chunk.layers.resize(1);
    chunk.layers[0].textureId = 0;
    chunk.layers[0].flags = 0;
    chunk.layers[0].offsetMCAL = 0;
    chunk.layers[0].effectId = effectId;
    chunk.doodadMapping.fill(0);
    return chunk;
}

void setEffectLayer(MapChunk& chunk, int qy, int qx, uint32_t layerIdx) {
    const int quad = qy * 8 + qx;
    const int byteIdx = quad / 4;
    const int shift = (quad % 4) * 2;
    chunk.doodadMapping[byteIdx] = static_cast<uint8_t>(
        (chunk.doodadMapping[byteIdx] & ~(3u << shift)) | ((layerIdx & 3u) << shift));
}

GroundClutterEffectSpec makeEffect(uint32_t density) {
    GroundClutterEffectSpec spec;
    spec.doodadIds = {10u, 11u, 12u, 13u};
    spec.weights = {1u, 1u, 1u, 1u};
    spec.density = density;
    return spec;
}

} // namespace

TEST_CASE("ground clutter density follows TBC 2.4.3 N/256 scaling across 256 chunks",
          "[terrain][clutter]") {
    const MapChunk chunk = makeUniformChunk(944u);
    const GroundClutterEffectSpec ge = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &ge : nullptr;
    };

    REQUIRE(effectiveGroundEffectDensity(0.0f) == Catch::Approx(0.0f));
    REQUIRE(effectiveGroundEffectDensity(0.5625f) == Catch::Approx(24.0f));
    REQUIRE(effectiveGroundEffectDensity(1.5f) == Catch::Approx(64.0f));

    // At groundEffectDensity = 24 (56.25% UI), density = 8 -> expected 8 * 24 / 256 = 0.75 / cell.
    size_t total24 = 0;
    size_t activeCells24 = 0;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            std::array<int, 64> counts{};
            scatterChunkGroundClutter(chunk, 31, 42, cx, cy, 0.5625f, lookup,
                                      [&](const GroundClutterSample& s) {
                                          ++total24;
                                          ++counts[s.qy * 8 + s.qx];
                                      });
            for (int c : counts) {
                if (c > 0) ++activeCells24;
            }
        }
    }
    const double meanPerCell24 = static_cast<double>(total24) / (256.0 * 64.0);
    const double activeFrac24 = static_cast<double>(activeCells24) / (256.0 * 64.0);
    REQUIRE(meanPerCell24 == Catch::Approx(0.75).margin(0.03));
    // Expected active cell fraction: 1 - (63/64)^24 = 0.3147 (patchy clumps, not 75% uniform).
    REQUIRE(activeFrac24 == Catch::Approx(0.3147).margin(0.02));

    // At groundEffectDensity = 64 (150% max UI slider), density = 8 -> expected 8 * 64 / 256 = 2.0 / cell.
    size_t total64 = 0;
    size_t activeCells64 = 0;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            std::array<int, 64> counts{};
            scatterChunkGroundClutter(chunk, 31, 42, cx, cy, 1.5f, lookup,
                                      [&](const GroundClutterSample& s) {
                                          ++total64;
                                          ++counts[s.qy * 8 + s.qx];
                                      });
            for (int c : counts) {
                if (c > 0) ++activeCells64;
            }
        }
    }
    const double meanPerCell64 = static_cast<double>(total64) / (256.0 * 64.0);
    const double activeFrac64 = static_cast<double>(activeCells64) / (256.0 * 64.0);
    REQUIRE(meanPerCell64 == Catch::Approx(2.0).margin(0.05));
    // Expected active cell fraction: 1 - (63/64)^64 = 0.6350.
    REQUIRE(activeFrac64 == Catch::Approx(0.6350).margin(0.02));

    // Zero density emits nothing.
    size_t total0 = 0;
    scatterChunkGroundClutter(chunk, 31, 42, 0, 0, 0.0f, lookup,
                              [&](const GroundClutterSample&) { ++total0; });
    REQUIRE(total0 == 0);
}

TEST_CASE("raising groundEffectDensity preserves lower-density clumps monotonically",
          "[terrain][clutter]") {
    const MapChunk chunk = makeUniformChunk(944u);
    const GroundClutterEffectSpec ge = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &ge : nullptr;
    };

    std::vector<GroundClutterSample> low;
    std::vector<GroundClutterSample> high;
    scatterChunkGroundClutter(chunk, 31, 42, 7, 9, 0.5625f, lookup,
                              [&](const GroundClutterSample& s) { low.push_back(s); });
    scatterChunkGroundClutter(chunk, 31, 42, 7, 9, 1.5f, lookup,
                              [&](const GroundClutterSample& s) { high.push_back(s); });

    REQUIRE(!low.empty());
    REQUIRE(high.size() > low.size());
    for (size_t i = 0; i < low.size(); ++i) {
        REQUIRE(high[i].qx == low[i].qx);
        REQUIRE(high[i].qy == low[i].qy);
        REQUIRE(high[i].fracX == Catch::Approx(low[i].fracX));
        REQUIRE(high[i].fracY == Catch::Approx(low[i].fracY));
        REQUIRE(high[i].doodadId == low[i].doodadId);
        REQUIRE(high[i].yaw == Catch::Approx(low[i].yaw));
        REQUIRE(high[i].scale == Catch::Approx(low[i].scale));
    }
}

TEST_CASE("diagonal road layer map, noEffectDoodad, and holes suppress road clutter",
          "[terrain][clutter]") {
    MapChunk chunk = makeUniformChunk(944u);
    chunk.layers.resize(2);
    chunk.layers[1].textureId = 1;
    chunk.layers[1].effectId = 417u; // bare road (no doodads)

    // Diagonal road: qx == qy or qx == qy + 1 mapped to bare road layer 1.
    for (int qy = 0; qy < 8; ++qy) {
        for (int qx = 0; qx < 8; ++qx) {
            if (qx == qy || qx == qy + 1) {
                setEffectLayer(chunk, qy, qx, 1u);
            }
        }
    }
    // Suppress an additional off-road strip via noEffectDoodad (row 0, cols 5..7).
    chunk.noEffectDoodad = (1ull << 5) | (1ull << 6) | (1ull << 7);
    // Cut a 2x2 terrain hole at quads (qy=6..7, qx=0..1) -> column=3, row=0 -> bit 12.
    chunk.holes = static_cast<uint16_t>(1u << 12);

    const GroundClutterEffectSpec grass = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &grass : nullptr;
    };

    size_t emitted = 0;
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            scatterChunkGroundClutter(chunk, 31, 42, cx, cy, 1.5f, lookup,
                                      [&](const GroundClutterSample& s) {
                                          ++emitted;
                                          REQUIRE_FALSE(chunk.isHole(s.qy, s.qx));
                                          REQUIRE_FALSE(chunk.isEffectDisabled(s.qy, s.qx));
                                          REQUIRE(chunk.effectLayerFor(s.qy, s.qx) == 0u);
                                          REQUIRE_FALSE(s.qx == s.qy);
                                          REQUIRE_FALSE(s.qx == s.qy + 1);
                                      });
        }
    }
    REQUIRE(emitted > 0);
}

TEST_CASE("real Dun Morogh road ADT doodadMapping aligns with dominant MCAL layer",
          "[terrain][clutter]") {
    const std::filesystem::path adtPath =
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/maps/azeroth/azeroth_31_43.adt";
    if (!std::filesystem::exists(adtPath)) {
        SUCCEED("TBC ADT data not present in this environment");
        return;
    }

    std::ifstream in(adtPath, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    const ADTTerrain terrain = ADTLoader::load(bytes);
    REQUIRE(terrain.isLoaded());

    size_t matched = 0;
    size_t total = 0;
    for (const auto& chunk : terrain.chunks) {
        if (chunk.layers.size() < 2) continue;
        std::array<std::vector<uint8_t>, 4> alphas;
        const size_t nLayers = std::min<size_t>(chunk.layers.size(), 4);
        for (size_t i = 1; i < nLayers; ++i) {
            decodeLayerAlpha(chunk, i, alphas[i], 0);
        }
        for (int qy = 0; qy < 8; ++qy) {
            for (int qx = 0; qx < 8; ++qx) {
                std::array<float, 4> wsum{};
                for (int ty = qy * 8; ty < qy * 8 + 8; ++ty) {
                    for (int tx = qx * 8; tx < qx * 8 + 8; ++tx) {
                        float rem = 1.0f;
                        for (size_t i = nLayers; i-- > 1;) {
                            const float a = alphas[i].empty()
                                ? 0.0f
                                : static_cast<float>(alphas[i][static_cast<size_t>(ty * 64 + tx)]) / 255.0f;
                            const float w = rem * a;
                            wsum[i] += w;
                            rem -= w;
                        }
                        wsum[0] += std::max(rem, 0.0f);
                    }
                }
                uint32_t dom = 0;
                for (uint32_t i = 1; i < nLayers; ++i) {
                    if (wsum[i] > wsum[dom]) dom = i;
                }
                if (chunk.effectLayerFor(qy, qx) == dom) ++matched;
                ++total;
            }
        }
    }
    REQUIRE(total > 0);
    REQUIRE(static_cast<double>(matched) / static_cast<double>(total) > 0.95);
}

TEST_CASE("steep terrain slopes suppress ground clutter while flat quads still grow",
          "[terrain][clutter]") {
    MapChunk chunk = makeUniformChunk(2811u);
    // Populate MCNR normals: top half (qy=0..3) steep cliff (nz ~ 0.55),
    // bottom half (qy=4..7) gentle ground (nz ~ 0.96).
    for (int qy = 0; qy < 8; ++qy) {
        for (int qx = 0; qx < 8; ++qx) {
            const size_t idx = static_cast<size_t>(9 + qy * 17 + qx);
            chunk.normals[idx * 3 + 0] = (qy < 4) ? 105 : 25;
            chunk.normals[idx * 3 + 1] = 0;
            chunk.normals[idx * 3 + 2] = (qy < 4) ? 70 : 122;
        }
    }

    const GroundClutterEffectSpec grass = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 2811u) ? &grass : nullptr;
    };

    size_t emitted = 0;
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            scatterChunkGroundClutter(chunk, 38, 33, cx, cy, 1.5f, lookup,
                                      [&](const GroundClutterSample& s) {
                                          ++emitted;
                                          REQUIRE(s.qy >= 4);
                                          REQUIRE(wowee::pipeline::chunkQuadNormalZ(chunk, s.qy, s.qx) >=
                                                  wowee::pipeline::kGroundClutterMinNormalZ);
                                      });
        }
    }
    REQUIRE(emitted > 0);
}

TEST_CASE("real TBC Kalimdor Ratchet hill suppresses path and steep-slope clutter while keeping gentle Barrens grass",
          "[terrain][clutter]") {
    const std::filesystem::path ratchetAdt =
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/maps/kalimdor/kalimdor_38_33.adt";
    const std::filesystem::path xroadsAdt =
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/maps/kalimdor/kalimdor_36_32.adt";
    if (!std::filesystem::exists(ratchetAdt) || !std::filesystem::exists(xroadsAdt)) {
        SUCCEED("TBC Kalimdor ADT data not present in this environment");
        return;
    }

    auto loadAdt = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        ADTTerrain terrain = ADTLoader::load(bytes);
        REQUIRE(terrain.isLoaded());
        return terrain;
    };

    const ADTTerrain ratchet = loadAdt(ratchetAdt);
    const ADTTerrain xroads = loadAdt(xroadsAdt);

    const GroundClutterEffectSpec barrensDirt = makeEffect(8u);
    const GroundClutterEffectSpec barrensGrass = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        if (id == 2808u || id == 2873u) return &barrensDirt;
        if (id == 2811u || id == 2813u || id == 2872u || id == 2877u) return &barrensGrass;
        return nullptr;
    };

    // Ratchet hill chunk (15,15) overlooking Ratchet Bruiser at (-1054.79, -3702.88):
    // Bruiser's sandy path quads (qy=5,qx=0..2; qy=6,qx=2..5; qy=7,qx=5..6) are masked by
    // noEffectDoodad, and steep hillside quads (e.g. qy=4,qx=4; qy=7,qx=3) must stay bare.
    const MapChunk& hillChunk = ratchet.getChunk(15, 15);
    REQUIRE(hillChunk.isEffectDisabled(5, 0));
    REQUIRE(hillChunk.isEffectDisabled(5, 1));
    REQUIRE(hillChunk.isEffectDisabled(5, 2));
    REQUIRE(wowee::pipeline::chunkQuadNormalZ(hillChunk, 4, 4) < wowee::pipeline::kGroundClutterMinNormalZ);
    REQUIRE(wowee::pipeline::chunkQuadNormalZ(hillChunk, 7, 3) < wowee::pipeline::kGroundClutterMinNormalZ);

    size_t hillEmitted = 0;
    scatterChunkGroundClutter(hillChunk, 38, 33, 15, 15, 1.5f, lookup,
                              [&](const GroundClutterSample& s) {
                                  ++hillEmitted;
                                  REQUIRE_FALSE(hillChunk.isEffectDisabled(s.qy, s.qx));
                                  REQUIRE(wowee::pipeline::chunkQuadNormalZ(hillChunk, s.qy, s.qx) >=
                                          wowee::pipeline::kGroundClutterMinNormalZ);
                              });
    REQUIRE(hillEmitted > 0);

    // Ratchet shoreline chunk (15,12) and Crossroads open grass chunk (14,15) must still grow grass.
    size_t shoreEmitted = 0;
    scatterChunkGroundClutter(ratchet.getChunk(15, 12), 38, 33, 15, 12, 0.5625f, lookup,
                              [&](const GroundClutterSample&) { ++shoreEmitted; });
    REQUIRE(shoreEmitted > 0);

    size_t xroadsEmitted = 0;
    scatterChunkGroundClutter(xroads.getChunk(14, 15), 36, 32, 14, 15, 0.5625f, lookup,
                              [&](const GroundClutterSample&) { ++xroadsEmitted; });
    REQUIRE(xroadsEmitted > 0);
}
