// TBC ocean MCLQ tile coverage and depth-ramp invariants.
//
// In Vanilla/TBC MCLQ tiles[8][8], only the low-nibble sentinel 0x0F marks a
// dry/hidden cell. Upper bits (0x40 = non-fishable, 0x80 = fatigue/deep ocean,
// e.g. 0xC1) mark valid rendered ocean cells. Treating 0x80 as hidden dropped
// every fatigue ocean tile across Azeroth, Kalimdor, and Expansion01.

#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "rendering/water_mask.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using wowee::pipeline::ADTLoader;
using wowee::pipeline::ADTTerrain;
using wowee::rendering::chunkWaterMask;
using wowee::rendering::waterVertexUsed;

namespace {

constexpr uint32_t kMCNK = 0x4D434E4B;
constexpr uint32_t kMCLQ = 0x4D434C51;

void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, sizeof(v));
    out.insert(out.end(), b, b + 4);
}

void appendFloat(std::vector<uint8_t>& out, float v) {
    uint8_t b[4];
    std::memcpy(b, &v, sizeof(v));
    out.insert(out.end(), b, b + 4);
}

void writeU32At(std::vector<uint8_t>& buf, size_t offset, uint32_t v) {
    REQUIRE(offset + sizeof(v) <= buf.size());
    std::memcpy(buf.data() + offset, &v, sizeof(v));
}

std::vector<uint8_t> buildSingleOceanChunkAdt(const std::array<uint8_t, 64>& tileFlags,
                                              const std::array<uint8_t, 81>& depths,
                                              float waterHeight = 0.0f) {
    std::vector<uint8_t> mcnk(128, 0);
    // MCNK flags: 0x08 = ocean liquid
    writeU32At(mcnk, 0, 0x08u);
    const uint32_t ofsLiquid = 128;

    std::vector<uint8_t> mclqPayload;
    appendFloat(mclqPayload, waterHeight);  // minHeight
    appendFloat(mclqPayload, waterHeight);  // maxHeight
    for (int i = 0; i < 81; ++i) {
        mclqPayload.push_back(depths[static_cast<size_t>(i)]);
        mclqPayload.push_back(0);
        mclqPayload.push_back(0);
        mclqPayload.push_back(0);
        appendFloat(mclqPayload, waterHeight);
    }
    mclqPayload.insert(mclqPayload.end(), tileFlags.begin(), tileFlags.end());
    // Trailing 84 bytes of flow data (standard MCLQ size 804 bytes)
    mclqPayload.insert(mclqPayload.end(), 84, 0u);

    const uint32_t sizeLiquid = 8u + static_cast<uint32_t>(mclqPayload.size());
    writeU32At(mcnk, 0x60, ofsLiquid);
    writeU32At(mcnk, 0x64, sizeLiquid);

    appendU32(mcnk, kMCLQ);
    appendU32(mcnk, static_cast<uint32_t>(mclqPayload.size()));
    mcnk.insert(mcnk.end(), mclqPayload.begin(), mclqPayload.end());

    std::vector<uint8_t> adt;
    appendU32(adt, kMCNK);
    appendU32(adt, static_cast<uint32_t>(mcnk.size()));
    adt.insert(adt.end(), mcnk.begin(), mcnk.end());
    return adt;
}

int popcount64(uint64_t v) {
    int n = 0;
    while (v != 0) {
        n += static_cast<int>(v & 1u);
        v >>= 1;
    }
    return n;
}

std::filesystem::path findTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p / "world/maps/expansion01/expansion01_46_13.adt")) return p;
    }
    const std::filesystem::path fallback("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    if (std::filesystem::exists(fallback / "world/maps/expansion01/expansion01_46_13.adt")) return fallback;
    return {};
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("MCLQ fatigue bit 0x80 keeps deep ocean cells visible while 0x0F hides dry cells",
          "[water][ocean][adt]") {
    std::array<uint8_t, 64> tileFlags{};
    // Rows 0-1: dry beach (0x0F and 0x8F)
    // Rows 2-4: coastal ocean (0x41)
    // Rows 5-7: fatigue deep ocean (0xC1 = 0x80 | 0x40 | 0x01)
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            uint8_t flag = 0xC1;
            if (row == 0) flag = 0x0F;
            else if (row == 1) flag = 0x8F;
            else if (row < 5) flag = 0x41;
            tileFlags[static_cast<size_t>(row * 8 + col)] = flag;
        }
    }
    std::array<uint8_t, 81> depths{};
    depths.fill(54);

    const ADTTerrain terrain = ADTLoader::load(buildSingleOceanChunkAdt(tileFlags, depths, 0.0f));
    REQUIRE(terrain.isLoaded());
    REQUIRE(terrain.waterData[0].layers.size() == 1);

    const auto& layer = terrain.waterData[0].layers[0];
    CHECK(layer.liquidType == 2);  // ocean (basicType = (2 - 1) % 4 = 1)
    const uint64_t mask = chunkWaterMask(layer.mask, layer.x, layer.y, layer.width, layer.height);
    CHECK(popcount64(mask) == 48);  // 6 wet rows * 8 cols

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            const bool wet = (mask & (uint64_t{1} << (row * 8 + col))) != 0;
            CHECK(wet == (row >= 2));
        }
    }
}

TEST_CASE("MCLQ all-fatigue 0xC1 ocean chunk loads all 64 cells", "[water][ocean][adt]") {
    std::array<uint8_t, 64> tileFlags{};
    tileFlags.fill(0xC1);
    std::array<uint8_t, 81> depths{};
    depths.fill(255);

    const ADTTerrain terrain = ADTLoader::load(buildSingleOceanChunkAdt(tileFlags, depths, 0.0f));
    REQUIRE(terrain.isLoaded());
    REQUIRE(terrain.waterData[0].layers.size() == 1);

    const auto& layer = terrain.waterData[0].layers[0];
    const uint64_t mask = chunkWaterMask(layer.mask, layer.x, layer.y, layer.width, layer.height);
    CHECK(mask == ~uint64_t{0});
}

TEST_CASE("Expansion01 North Sea ADTs around (10030, -7862) preserve all fatigue ocean tiles",
          "[water][ocean][adt]") {
    const auto tbcDir = findTbcDataDir();
    if (tbcDir.empty()) {
        SKIP("TBC extracted data directory not available");
    }

    struct TileExpectation {
        const char* relPath;
        int expectedChunks;
        int expectedWetCells;
    };
    const std::array<TileExpectation, 4> cases{{
        {"world/maps/expansion01/expansion01_46_13.adt", 58, 2791},
        {"world/maps/expansion01/expansion01_46_12.adt", 213, 13028},
        {"world/maps/expansion01/expansion01_47_12.adt", 256, 16384},
        {"world/maps/expansion01/expansion01_47_13.adt", 253, 16061},
    }};

    for (const auto& tc : cases) {
        const auto bytes = readFileBytes(tbcDir / tc.relPath);
        const ADTTerrain terrain = ADTLoader::load(bytes);
        REQUIRE(terrain.isLoaded());

        int oceanChunks = 0;
        int wetCells = 0;
        for (int c = 0; c < 256; ++c) {
            for (const auto& layer : terrain.waterData[c].layers) {
                if (layer.liquidType != 2) continue;
                ++oceanChunks;
                const uint64_t mask = chunkWaterMask(layer.mask, layer.x, layer.y,
                                                     layer.width, layer.height);
                wetCells += popcount64(mask);
                for (int y = 0; y <= layer.height; ++y) {
                    for (int x = 0; x <= layer.width; ++x) {
                        if (!waterVertexUsed(mask, layer.x + x, layer.y + y)) continue;
                        const float h = layer.heights[static_cast<size_t>(y * (layer.width + 1) + x)];
                        REQUIRE(std::isfinite(h));
                        REQUIRE(std::abs(h) < 50000.0f);
                    }
                }
            }
        }
        CHECK(oceanChunks == tc.expectedChunks);
        CHECK(wetCells == tc.expectedWetCells);
    }
}
