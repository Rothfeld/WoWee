#pragma once

// M2 skin batches whose flags carry 0x4 are projected textures: the batch is a
// single quad (all 533 flagged batches in the TBC data are one four-vertex
// submesh with one bone set), and its texture is laid onto the ground under it
// rather than drawn as a flat plane. Flamestrike_Impact_Base's LavaGround,
// ThunderClap_Cast_Base's CrackedGround and Shockwave10 (War Stomp) and
// Consecration_Impact_Base are examples. The census and the evidence are in
// _experiments/20261117T2235Z.ground_effects.
//
// The texture is projected straight down: a ground point takes the texture
// coordinates of the quad point above or below it. Draping a grid over the
// ground under the quad, with the grid's texture coordinates interpolated
// across the quad, is that projection sampled at the grid's spacing.

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::projected_texture {

inline constexpr uint8_t kBatchFlagProjected = 0x4;

/// Grid spacing: a quarter of an ADT chunk's vertex spacing (533.333 / 16 / 8
/// yards), half the distance from an outer terrain vertex to the inner vertex
/// between them, so every terrain triangle edge is crossed by grid lines.
inline constexpr float kCellSize = 533.33333f / 16.0f / 8.0f / 4.0f;
inline constexpr int kMaxCellsPerSide = 48;
/// Raised off the surface it lies on so the depth test keeps it in front.
inline constexpr float kSurfaceLift = 0.04f;

/// The quad's corners in the order (0,0) (1,0) (1,1) (0,1), from its two
/// triangles: the two vertices both triangles share are the diagonal.
inline std::array<uint16_t, 4> orderQuadCorners(std::span<const uint16_t, 6> tri) {
    std::array<uint16_t, 2> diagonal{};
    int found = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 3; j < 6; ++j) {
            if (tri[i] == tri[j]) {
                assert(found < 2);
                diagonal[found++] = tri[i];
            }
        }
    }
    assert(found == 2);
    std::array<uint16_t, 2> outer{};
    int outerCount = 0;
    for (uint16_t v : tri) {
        if (v == diagonal[0] || v == diagonal[1]) continue;
        if (outerCount == 1 && outer[0] == v) continue;
        assert(outerCount < 2);
        outer[outerCount++] = v;
    }
    assert(outerCount == 2);
    return {outer[0], diagonal[0], outer[1], diagonal[1]};
}

struct Corner {
    glm::vec3 position;  // world
    glm::vec3 normal;    // world
    glm::vec2 uv0;
    glm::vec2 uv1;
};

/// Vertex in the M2 pipeline's layout (18 floats): position relative to
/// `origin`, normal, uv0, uv1, bone weights, bone indices. Bones are unused.
struct Vertex {
    float data[18];
};

/// Height of the surface the texture lands on at (x, y), searching the band
/// [planeZ - reach, planeZ + reach] around the quad's own height there.
using SurfaceQuery = std::function<std::optional<float>(float x, float y, float planeZ, float reach)>;

/// Nearest terrain or WMO floor height within [planeZ - reach, planeZ + reach].
/// Keeps indoor cave floors below hills and lower floors under overhangs.
inline std::optional<float> nearestSurfaceZ(float planeZ, float reach,
                                            std::optional<float> terrainZ,
                                            std::optional<float> wmoZ) {
    std::optional<float> best;
    auto consider = [&](std::optional<float> h) {
        if (!h || std::abs(*h - planeZ) > reach) return;
        if (!best || std::abs(*h - planeZ) < std::abs(*best - planeZ)) best = h;
    };
    consider(terrainZ);
    consider(wmoZ);
    return best;
}

/// Triangle-list vertices of the quad draped over the surface. Points where
/// the query finds no surface stay on the quad's plane.
inline void buildGrid(const std::array<Corner, 4>& c, const glm::vec3& origin,
                      const SurfaceQuery& surface, std::vector<Vertex>& out) {
    out.clear();
    const float sideU = std::max(glm::length(glm::vec2(c[1].position - c[0].position)),
                                 glm::length(glm::vec2(c[2].position - c[3].position)));
    const float sideV = std::max(glm::length(glm::vec2(c[3].position - c[0].position)),
                                 glm::length(glm::vec2(c[2].position - c[1].position)));
    const int cellsU = std::clamp(static_cast<int>(std::ceil(sideU / kCellSize)), 1, kMaxCellsPerSide);
    const int cellsV = std::clamp(static_cast<int>(std::ceil(sideV / kCellSize)), 1, kMaxCellsPerSide);
    // A surface rising at 45 degrees from the centre spans half the diagonal.
    const float reach = 0.5f * std::sqrt(sideU * sideU + sideV * sideV);

    auto bilinear = [](auto a, auto b, auto cc, auto d, float u, float v) {
        return a * ((1.0f - u) * (1.0f - v)) + b * (u * (1.0f - v)) + cc * (u * v) + d * ((1.0f - u) * v);
    };
    std::vector<Vertex> grid(static_cast<size_t>((cellsU + 1) * (cellsV + 1)));
    for (int j = 0; j <= cellsV; ++j) {
        for (int i = 0; i <= cellsU; ++i) {
            const float u = static_cast<float>(i) / cellsU;
            const float v = static_cast<float>(j) / cellsV;
            glm::vec3 p = bilinear(c[0].position, c[1].position, c[2].position, c[3].position, u, v);
            if (auto h = surface(p.x, p.y, p.z, reach)) p.z = *h + kSurfaceLift;
            const glm::vec3 n = bilinear(c[0].normal, c[1].normal, c[2].normal, c[3].normal, u, v);
            const glm::vec2 t0 = bilinear(c[0].uv0, c[1].uv0, c[2].uv0, c[3].uv0, u, v);
            const glm::vec2 t1 = bilinear(c[0].uv1, c[1].uv1, c[2].uv1, c[3].uv1, u, v);
            const glm::vec3 rel = p - origin;
            grid[static_cast<size_t>(j * (cellsU + 1) + i)] = Vertex{{
                rel.x, rel.y, rel.z, n.x, n.y, n.z, t0.x, t0.y, t1.x, t1.y,
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
        }
    }
    out.reserve(static_cast<size_t>(cellsU * cellsV * 6));
    for (int j = 0; j < cellsV; ++j) {
        for (int i = 0; i < cellsU; ++i) {
            const size_t a = static_cast<size_t>(j * (cellsU + 1) + i);
            const size_t b = a + 1;
            const size_t d = a + static_cast<size_t>(cellsU + 1);
            const size_t e = d + 1;
            for (size_t k : {a, b, e, a, e, d}) out.push_back(grid[k]);
        }
    }
}

}  // namespace wowee::rendering::projected_texture
