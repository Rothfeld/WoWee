#pragma once

#include "core/coordinates.hpp"
#include "pipeline/terrain_mesh.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <span>
#include <vector>

namespace wowee::rendering {

/// Default TBC 2.4.3 CVar `horizonfarclip` distance in yards (`GetCVarDefault("horizonfarclip") == "2112"`).
inline constexpr float kDefaultHorizonFarClip = 2112.0f;

/// One 533.333-yard map tile from a `.wdl` low-resolution horizon file (`MARE` + optional `MAHO`).
struct WdlHorizonTile {
    int maofRow = 0;  ///< 0..63 north -> south (`idx / 64`, `tileX` in `worldToTile`)
    int maofCol = 0;  ///< 0..63 west  -> east  (`idx % 64`, `tileY` in `worldToTile`)
    std::array<int16_t, 17 * 17> outerHeights{};
    std::array<int16_t, 16 * 16> innerHeights{};
    std::array<uint16_t, 16> holeMask{};
};

/// Parsed `.wdl` horizon heightfields for an entire map (`World\Maps\<Map>\<Map>.wdl`).
struct WdlHorizonMap {
    std::vector<WdlHorizonTile> tiles;
};

/// Parse a `.wdl` binary buffer (`MVER`, optional `MWMO`/`MWID`/`MODF`, `MAOF`, `MARE`, `MAHO`).
inline bool parseWdlHorizon(std::span<const uint8_t> data, WdlHorizonMap& out) {
    out.tiles.clear();
    if (data.size() < 16) return false;

    auto readU32 = [&](size_t offset) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, data.data() + offset, sizeof(uint32_t));
        return v;
    };
    auto isChunk = [&](size_t offset, const char fourcc[4]) -> bool {
        // On-disk WoW IFF FourCCs are reversed little-endian ASCII (e.g. 'FOAM' for MAOF).
        return data[offset + 0] == static_cast<uint8_t>(fourcc[3]) &&
               data[offset + 1] == static_cast<uint8_t>(fourcc[2]) &&
               data[offset + 2] == static_cast<uint8_t>(fourcc[1]) &&
               data[offset + 3] == static_cast<uint8_t>(fourcc[0]);
    };

    size_t maofPayloadOffset = 0;
    bool foundMaof = false;
    size_t pos = 0;
    while (pos + 8 <= data.size()) {
        const uint32_t chunkSize = readU32(pos + 4);
        if (chunkSize > data.size() - (pos + 8)) break;
        if (isChunk(pos, "MAOF")) {
            if (chunkSize < 4096u * sizeof(uint32_t)) return false;
            maofPayloadOffset = pos + 8;
            foundMaof = true;
            break;
        }
        pos += 8 + static_cast<size_t>(chunkSize);
    }
    if (!foundMaof) return false;

    static constexpr uint32_t kMarePayloadBytes = (17u * 17u + 16u * 16u) * sizeof(int16_t); // 1090
    for (uint32_t idx = 0; idx < 4096u; ++idx) {
        const uint32_t mareOffset = readU32(maofPayloadOffset + idx * sizeof(uint32_t));
        if (mareOffset == 0) continue;
        if (static_cast<size_t>(mareOffset) + 8 + kMarePayloadBytes > data.size()) continue;
        if (!isChunk(mareOffset, "MARE")) continue;
        const uint32_t mareSize = readU32(static_cast<size_t>(mareOffset) + 4);
        if (mareSize < kMarePayloadBytes) continue;
        if (static_cast<size_t>(mareOffset) + 8 + static_cast<size_t>(mareSize) > data.size()) continue;

        WdlHorizonTile tile;
        tile.maofRow = static_cast<int>(idx / 64u);
        tile.maofCol = static_cast<int>(idx % 64u);
        std::memcpy(tile.outerHeights.data(),
                    data.data() + mareOffset + 8,
                    17u * 17u * sizeof(int16_t));
        std::memcpy(tile.innerHeights.data(),
                    data.data() + mareOffset + 8 + 17u * 17u * sizeof(int16_t),
                    16u * 16u * sizeof(int16_t));

        const size_t nextChunk = static_cast<size_t>(mareOffset) + 8 + static_cast<size_t>(mareSize);
        if (nextChunk + 8 + 16u * sizeof(uint16_t) <= data.size() && isChunk(nextChunk, "MAHO")) {
            const uint32_t mahoSize = readU32(nextChunk + 4);
            if (mahoSize >= 16u * sizeof(uint16_t) &&
                nextChunk + 8 + 16u * sizeof(uint16_t) <= data.size()) {
                std::memcpy(tile.holeMask.data(),
                            data.data() + nextChunk + 8,
                            16u * sizeof(uint16_t));
            }
        }
        out.tiles.push_back(tile);
    }
    return true;
}

/// Build engine render-space vertices, CCW (+Z upward) triangle indices, and 3D
/// bounding sphere for one `.wdl` horizon tile, skipping `MAHO` hole cells.
inline void buildWdlTileMesh(const WdlHorizonTile& tile,
                             std::vector<pipeline::TerrainVertex>& outVertices,
                             std::vector<uint32_t>& outIndices,
                             glm::vec3& outCenter,
                             float& outRadius) {
    outVertices.clear();
    outIndices.clear();
    outVertices.reserve(17u * 17u + 16u * 16u);
    outIndices.reserve(16u * 16u * 12u);

    constexpr float kTileSize = core::coords::TILE_SIZE;
    glm::vec3 bbMin(std::numeric_limits<float>::max());
    glm::vec3 bbMax(std::numeric_limits<float>::lowest());

    auto pushVertex = [&](float gridCol, float gridRow, int16_t height) {
        pipeline::TerrainVertex v{};
        // Match TerrainMeshGenerator::generateVertices exactly:
        // ADT tileY == maofRow (0..63 north->south, position[0] decreasing)
        // ADT tileX == maofCol (0..63 west->east,  position[1] decreasing)
        v.position[0] = (32.0f - static_cast<float>(tile.maofRow) - gridRow / 16.0f) * kTileSize;
        v.position[1] = (32.0f - static_cast<float>(tile.maofCol) - gridCol / 16.0f) * kTileSize;
        v.position[2] = static_cast<float>(height);
        v.normal[0] = 0.0f;
        v.normal[1] = 0.0f;
        v.normal[2] = 1.0f;
        const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
        bbMin = glm::min(bbMin, p);
        bbMax = glm::max(bbMax, p);
        outVertices.push_back(v);
    };

    // 0..288: 17x17 outer grid (r = north->south, c = west->east)
    for (int r = 0; r < 17; ++r) {
        for (int c = 0; c < 17; ++c) {
            pushVertex(static_cast<float>(c), static_cast<float>(r),
                       tile.outerHeights[static_cast<size_t>(r * 17 + c)]);
        }
    }
    // 289..544: 16x16 inner cell centers
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            pushVertex(static_cast<float>(c) + 0.5f, static_cast<float>(r) + 0.5f,
                       tile.innerHeights[static_cast<size_t>(r * 16 + c)]);
        }
    }

    for (int r = 0; r < 16; ++r) {
        const uint16_t rowHoles = tile.holeMask[static_cast<size_t>(r)];
        for (int c = 0; c < 16; ++c) {
            if ((rowHoles & static_cast<uint16_t>(1u << c)) != 0) continue;
            const uint32_t nw = static_cast<uint32_t>(r * 17 + c);
            const uint32_t ne = static_cast<uint32_t>(r * 17 + c + 1);
            const uint32_t sw = static_cast<uint32_t>((r + 1) * 17 + c);
            const uint32_t se = static_cast<uint32_t>((r + 1) * 17 + c + 1);
            const uint32_t center = static_cast<uint32_t>(289 + r * 16 + c);

            // Match TerrainMeshGenerator::generateIndices (tl=nw, tr=ne, br=se, bl=sw)
            // so VK_CULL_MODE_BACK_BIT keeps top faces and culls undersides.
            outIndices.push_back(center); outIndices.push_back(nw); outIndices.push_back(ne);
            outIndices.push_back(center); outIndices.push_back(ne); outIndices.push_back(se);
            outIndices.push_back(center); outIndices.push_back(se); outIndices.push_back(sw);
            outIndices.push_back(center); outIndices.push_back(sw); outIndices.push_back(nw);
        }
    }

    outCenter = (bbMin + bbMax) * 0.5f;
    float maxDistSq = 0.0f;
    for (const auto& v : outVertices) {
        const glm::vec3 d = glm::vec3(v.position[0], v.position[1], v.position[2]) - outCenter;
        maxDistSq = std::max(maxDistSq, glm::dot(d, d));
    }
    outRadius = std::sqrt(maxDistSq);
}

/// Whether a `.wdl` horizon tile's horizontal footprint overlaps the `[viewDistance - seam, horizonFarClip]` annulus.
inline bool wdlTileWithinHorizonRange(const glm::vec3& center,
                                      float radius,
                                      const glm::vec3& cameraPos,
                                      float viewDistance,
                                      float horizonFarClip) {
    if (radius < 0.0f || horizonFarClip <= 0.0f) return false;
    const float effectiveFar = std::max(horizonFarClip, viewDistance);
    const float innerStart = std::max(0.0f, viewDistance - 48.0f);
    const float dist2D = glm::length(glm::vec2(center - cameraPos));
    return (dist2D <= effectiveFar + radius) && (dist2D + radius >= innerStart);
}

} // namespace wowee::rendering
