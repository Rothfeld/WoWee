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
#include "rendering/placement_transform.hpp"

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

TEST_CASE("ground clutter density follows TBC 2.4.3 Binomial(density, N/256) per-slot survival across 256 chunks",
          "[terrain][clutter]") {
    const MapChunk chunk = makeUniformChunk(944u);
    const GroundClutterEffectSpec ge = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &ge : nullptr;
    };

    REQUIRE(effectiveGroundEffectDensity(0.0f) == Catch::Approx(0.0f));
    REQUIRE(effectiveGroundEffectDensity(0.375f) == Catch::Approx(16.0f));
    REQUIRE(effectiveGroundEffectDensity(0.5625f) == Catch::Approx(24.0f));
    REQUIRE(effectiveGroundEffectDensity(0.9375f) == Catch::Approx(40.0f));
    REQUIRE(effectiveGroundEffectDensity(1.5f) == Catch::Approx(64.0f));

    struct LevelCheck {
        float scale;
        double pSlot;
    };
    const std::array<LevelCheck, 4> levels = {{
        {0.375f,  16.0 / 256.0},
        {0.5625f, 24.0 / 256.0},
        {0.9375f, 40.0 / 256.0},
        {1.5f,    64.0 / 256.0},
    }};

    for (const auto& lvl : levels) {
        size_t total = 0;
        size_t activeCells = 0;
        size_t multiTuftCells = 0;
        for (int cy = 0; cy < 16; ++cy) {
            for (int cx = 0; cx < 16; ++cx) {
                std::array<int, 64> counts{};
                scatterChunkGroundClutter(chunk, 31, 42, cx, cy, lvl.scale, lookup,
                                          [&](const GroundClutterSample& s) {
                                              ++total;
                                              ++counts[s.qy * 8 + s.qx];
                                          });
                for (int c : counts) {
                    if (c > 0) ++activeCells;
                    if (c >= 2) ++multiTuftCells;
                }
            }
        }
        const double meanPerCell = static_cast<double>(total) / (256.0 * 64.0);
        const double activeFrac = static_cast<double>(activeCells) / (256.0 * 64.0);
        const double expectedMean = 8.0 * lvl.pSlot;
        const double expectedActive = 1.0 - std::pow(1.0 - lvl.pSlot, 8.0);
        REQUIRE(meanPerCell == Catch::Approx(expectedMean).margin(0.02));
        REQUIRE(activeFrac == Catch::Approx(expectedActive).margin(0.02));
        REQUIRE(multiTuftCells > 0);
    }

    // Low-density effects (density = 2 at N = 24 -> 2 * 24 / 256 = 0.1875/cell).
    const GroundClutterEffectSpec sparseGe = makeEffect(2u);
    auto sparseLookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &sparseGe : nullptr;
    };
    size_t sparseTotal = 0;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            scatterChunkGroundClutter(chunk, 31, 42, cx, cy, 0.5625f, sparseLookup,
                                      [&](const GroundClutterSample&) { ++sparseTotal; });
        }
    }
    REQUIRE(static_cast<double>(sparseTotal) / (256.0 * 64.0) == Catch::Approx(0.1875).margin(0.02));

    // Zero density emits nothing.
    size_t total0 = 0;
    scatterChunkGroundClutter(chunk, 31, 42, 0, 0, 0.0f, lookup,
                              [&](const GroundClutterSample&) { ++total0; });
    REQUIRE(total0 == 0);
}

TEST_CASE("raising groundEffectDensity adds tufts while preserving every lower-density tuft identically",
          "[terrain][clutter]") {
    const MapChunk chunk = makeUniformChunk(944u);
    const GroundClutterEffectSpec ge = makeEffect(8u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        return (id == 944u) ? &ge : nullptr;
    };

    size_t countLow = 0;
    size_t countHigh = 0;
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            std::array<std::vector<GroundClutterSample>, 64> lowByCell;
            std::array<std::vector<GroundClutterSample>, 64> highByCell;
            scatterChunkGroundClutter(chunk, 16, 36, cx, cy, 0.5625f, lookup,
                                      [&](const GroundClutterSample& s) { lowByCell[s.qy * 8 + s.qx].push_back(s); });
            scatterChunkGroundClutter(chunk, 16, 36, cx, cy, 1.5f, lookup,
                                      [&](const GroundClutterSample& s) { highByCell[s.qy * 8 + s.qx].push_back(s); });

            for (size_t cell = 0; cell < 64; ++cell) {
                const auto& low = lowByCell[cell];
                const auto& high = highByCell[cell];
                countLow += low.size();
                countHigh += high.size();
                REQUIRE(high.size() >= low.size());
                // Every tuft present at N=24 appears with identical (fracX, fracY, doodadId, yaw, scale) at N=64.
                for (const auto& ls : low) {
                    bool found = false;
                    for (const auto& hs : high) {
                        if (hs.doodadId == ls.doodadId &&
                            std::abs(hs.fracX - ls.fracX) < 1e-6f &&
                            std::abs(hs.fracY - ls.fracY) < 1e-6f &&
                            std::abs(hs.yaw - ls.yaw) < 1e-6f &&
                            std::abs(hs.scale - ls.scale) < 1e-6f) {
                            found = true;
                            break;
                        }
                    }
                    REQUIRE(found);
                }
            }
        }
    }
    REQUIRE(countLow > 0);
    REQUIRE(countHigh > countLow * 2);
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
    for (int cx = 12; cx < 16; ++cx) {
        scatterChunkGroundClutter(ratchet.getChunk(cx, 12), 38, 33, cx, 12, 0.5625f, lookup,
                                  [&](const GroundClutterSample&) { ++shoreEmitted; });
    }
    REQUIRE(shoreEmitted > 0);

    size_t xroadsEmitted = 0;
    for (int cx = 12; cx < 16; ++cx) {
        scatterChunkGroundClutter(xroads.getChunk(cx, 15), 36, 32, cx, 15, 0.5625f, lookup,
                                  [&](const GroundClutterSample&) { ++xroadsEmitted; });
    }
    REQUIRE(xroadsEmitted > 0);
}

TEST_CASE("sub-quad MCAL alpha suppresses ground clutter on bare road halves of mixed quads",
          "[terrain][clutter]") {
    MapChunk chunk = makeUniformChunk(944u);
    chunk.layers.resize(2);
    chunk.layers[1].textureId = 1;
    chunk.layers[1].flags = 0x100; // has alpha map (4-bit packed 2048 bytes)
    chunk.layers[1].offsetMCAL = 0;
    chunk.layers[1].effectId = 417u; // bare road (no doodads)

    // Paint layer 1 (bare road) across the right half of the chunk (tx >= 32, i.e., fracX >= 4.0)
    // while leaving predTex mapped to layer 0 everywhere.
    chunk.alphaMap.assign(wowee::pipeline::ALPHA_MAP_PACKED, 0u);
    for (size_t ty = 0; ty < 64; ++ty) {
        for (size_t tx = 0; tx < 64; tx += 2) {
            const uint8_t lo = (tx >= 32) ? 0x0Fu : 0x00u;
            const uint8_t hi = (tx + 1 >= 32) ? 0xF0u : 0x00u;
            chunk.alphaMap[(ty * 64 + tx) / 2] = static_cast<uint8_t>(lo | hi);
        }
    }

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
                                          // No sample may land deep inside the painted bare-road half.
                                          REQUIRE(s.fracX < 4.15f);
                                      });
        }
    }
    REQUIRE(emitted > 0);
}

TEST_CASE("real TBC Deadwind Pass Karazhan meadow populates grass at default density while respecting noEffectDoodad",
          "[terrain][clutter]") {
    const std::filesystem::path dwpAdt =
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/maps/azeroth/azeroth_35_52.adt";
    if (!std::filesystem::exists(dwpAdt)) {
        SUCCEED("TBC Deadwind Pass ADT data not present in this environment");
        return;
    }

    std::ifstream in(dwpAdt, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    const ADTTerrain terrain = ADTLoader::load(bytes);
    REQUIRE(terrain.isLoaded());

    const GroundClutterEffectSpec weeds = makeEffect(8u);
    const GroundClutterEffectSpec dirt = makeEffect(4u);
    const GroundClutterEffectSpec rock = makeEffect(2u);
    auto lookup = [&](uint32_t id) -> const GroundClutterEffectSpec* {
        if (id == 599u) return &weeds;
        if (id == 605u) return &dirt;
        if (id == 541u || id == 598u) return &rock;
        return nullptr;
    };

    // Karazhan meadow patch (12..15, 12..15) has unmasked DeadwindPassWeeds (density=8) quads;
    // under N/256 clump activation at d=24, active weeds quads carry full 8-tuft clumps while
    // masked road/courtyard quads stay bare.
    size_t emitted = 0;
    size_t activeQuads = 0;
    for (int cy = 12; cy < 16; ++cy) {
        for (int cx = 12; cx < 16; ++cx) {
            const MapChunk& chunk = terrain.getChunk(cx, cy);
            std::array<int, 64> perQuad{};
            scatterChunkGroundClutter(chunk, 35, 52, cx, cy, 0.5625f, lookup,
                                      [&](const GroundClutterSample& s) {
                                          ++emitted;
                                          ++perQuad[s.qy * 8 + s.qx];
                                          REQUIRE_FALSE(chunk.isEffectDisabled(s.qy, s.qx));
                                      });
            for (int c : perQuad) {
                if (c > 0) ++activeQuads;
            }
        }
    }
    REQUIRE(activeQuads >= 20);
    REQUIRE(emitted >= 120);
}

TEST_CASE("groundClutterSlopeEuler tilts GroundEffectDoodad +Z onto terrain surfaceNormal",
          "[terrain][clutter][slope]") {
    using wowee::pipeline::groundClutterSlopeEuler;
    using wowee::rendering::placementModelMatrix;

    // Flat terrain keeps horizontal yaw-only rotation.
    for (float yaw : {0.0f, 0.85f, 3.14159f, 5.42f}) {
        const glm::vec3 e = groundClutterSlopeEuler(glm::vec3(0.0f, 0.0f, 1.0f), yaw);
        const glm::mat4 m = placementModelMatrix(glm::vec3(0.0f), e, 1.0f);
        const glm::vec3 zOut = glm::vec3(m * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        const glm::vec3 xOut = glm::vec3(m * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        CHECK(zOut.x == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(zOut.y == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(zOut.z == Catch::Approx(1.0f).margin(1e-5f));
        CHECK(xOut.x == Catch::Approx(std::cos(yaw)).margin(1e-5f));
        CHECK(xOut.y == Catch::Approx(std::sin(yaw)).margin(1e-5f));
    }

    // Sloped hillsides (including Dustfire Valley MCNK (10..11, 15) ash slopes)
    // map local +Z onto normalize(surfaceNormal) with orthonormal basis vectors.
    const std::array<glm::vec3, 4> normals = {{
        {-0.02f, -0.37f, 0.92f},
        {-0.17f, -0.27f, 0.94f},
        { 0.35f, -0.28f, 0.89f},
        {-0.42f,  0.31f, 0.85f},
    }};
    for (const glm::vec3& rawN : normals) {
        const glm::vec3 n = glm::normalize(rawN);
        for (float yaw : {0.0f, 1.23f, 2.75f, 4.90f}) {
            const glm::vec3 e = groundClutterSlopeEuler(rawN, yaw);
            const glm::mat4 m = placementModelMatrix(glm::vec3(-6391.0f, -1973.0f, 250.5f), e, 0.95f);
            const glm::vec3 xAxis = glm::normalize(glm::vec3(m[0]));
            const glm::vec3 yAxis = glm::normalize(glm::vec3(m[1]));
            const glm::vec3 zAxis = glm::normalize(glm::vec3(m[2]));
            CHECK(zAxis.x == Catch::Approx(n.x).margin(1e-5f));
            CHECK(zAxis.y == Catch::Approx(n.y).margin(1e-5f));
            CHECK(zAxis.z == Catch::Approx(n.z).margin(1e-5f));
            CHECK(glm::dot(xAxis, zAxis) == Catch::Approx(0.0f).margin(1e-5f));
            CHECK(glm::dot(yAxis, zAxis) == Catch::Approx(0.0f).margin(1e-5f));
            CHECK(glm::dot(xAxis, yAxis) == Catch::Approx(0.0f).margin(1e-5f));
        }
    }
}
