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

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
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
/// Steepest surface slope (|dz| / horizontal distance) bridged between adjacent
/// grid samples (~56 degrees). Preserves walkable slopes (45 deg+) while
/// rejecting vertical walls, pillars, and upper ledges/ceilings.
inline constexpr float kMaxSlopeTan = 1.5f;
/// Minimum vertical step allowance between adjacent grid samples so low stair
/// treads and curbs stay continuous even when cells are closely spaced.
inline constexpr float kMaxStepHeight = 0.55f;

/// The quad's corners in the order (0,0) (1,0) (1,1) (0,1), preserving the
/// authored winding of the first triangle: the two vertices shared by both
/// triangles form the diagonal, and rotating the first triangle cyclically to
/// start at its outer corner gives (outer0, diagA, diagB) with the same winding
/// as (tri[0], tri[1], tri[2]). Placing outer1 opposite outer0 yields the
/// perimeter cycle (outer0, diagA, outer1, diagB), so buildGrid's triangles
/// keep the authored front-face orientation and single-sided projected quads
/// (such as ResistanceAura_Base.m2) are not backface-culled.
inline std::array<uint16_t, 4> orderQuadCorners(std::span<const uint16_t, 6> tri) {
    const auto inSecond = [&](uint16_t v) {
        return v == tri[3] || v == tri[4] || v == tri[5];
    };
    int outer0Pos = -1;
    int sharedCount = 0;
    for (int i = 0; i < 3; ++i) {
        if (inSecond(tri[i])) {
            ++sharedCount;
        } else {
            outer0Pos = i;
        }
    }
    assert(sharedCount == 2 && outer0Pos >= 0);
    const uint16_t outer0 = tri[outer0Pos];
    const uint16_t diagA = tri[(outer0Pos + 1) % 3];
    const uint16_t diagB = tri[(outer0Pos + 2) % 3];
    uint16_t outer1 = 0;
    int outer1Count = 0;
    for (int j = 3; j < 6; ++j) {
        if (tri[j] != diagA && tri[j] != diagB) {
            outer1 = tri[j];
            ++outer1Count;
        }
    }
    assert(outer1Count == 1);
    return {outer0, diagA, outer1, diagB};
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
    const int cols = cellsU + 1;
    const int rows = cellsV + 1;
    const size_t totalVerts = static_cast<size_t>(cols * rows);
    std::vector<Vertex> grid(totalVerts);
    std::vector<glm::vec3> worldPos(totalVerts);
    std::vector<float> quadPlaneZ(totalVerts);
    for (int j = 0; j <= cellsV; ++j) {
        for (int i = 0; i <= cellsU; ++i) {
            const float u = static_cast<float>(i) / cellsU;
            const float v = static_cast<float>(j) / cellsV;
            glm::vec3 p = bilinear(c[0].position, c[1].position, c[2].position, c[3].position, u, v);
            const size_t idx = static_cast<size_t>(j * cols + i);
            quadPlaneZ[idx] = p.z;
            if (auto h = surface(p.x, p.y, p.z, reach)) p.z = *h + kSurfaceLift;
            worldPos[idx] = p;
            const glm::vec3 n = bilinear(c[0].normal, c[1].normal, c[2].normal, c[3].normal, u, v);
            const glm::vec2 t0 = bilinear(c[0].uv0, c[1].uv0, c[2].uv0, c[3].uv0, u, v);
            const glm::vec2 t1 = bilinear(c[0].uv1, c[1].uv1, c[2].uv1, c[3].uv1, u, v);
            const glm::vec3 rel = p - origin;
            grid[idx] = Vertex{{
                rel.x, rel.y, rel.z, n.x, n.y, n.z, t0.x, t0.y, t1.x, t1.y,
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
        }
    }

    // Flood-fill surface continuity from the quad center so vertical cliffs
    // (pillars, walls, ceiling/capital hits) never stretch vertical curtains or
    // emit disconnected patches high above the floor.
    const auto edgeContinuous = [&](size_t u, size_t v) {
        const glm::vec3 d = worldPos[v] - worldPos[u];
        const float ds = std::hypot(d.x, d.y);
        const float maxDz = std::max(kMaxSlopeTan * ds, kMaxStepHeight);
        return std::abs(d.z) <= maxDz;
    };

    const int centerI = cellsU / 2;
    const int centerJ = cellsV / 2;
    size_t seed = static_cast<size_t>(centerJ * cols + centerI);
    const float seedTol = kMaxStepHeight + kSurfaceLift;
    if (std::abs(worldPos[seed].z - quadPlaneZ[seed]) > seedTol) {
        int bestDist2 = std::numeric_limits<int>::max();
        bool foundNearPlane = false;
        float bestDz = std::numeric_limits<float>::max();
        for (int j = 0; j < rows; ++j) {
            for (int i = 0; i < cols; ++i) {
                const size_t idx = static_cast<size_t>(j * cols + i);
                const float dz = std::abs(worldPos[idx].z - quadPlaneZ[idx]);
                const int di = i - centerI;
                const int dj = j - centerJ;
                const int dist2 = di * di + dj * dj;
                if (dz <= seedTol) {
                    if (!foundNearPlane || dist2 < bestDist2) {
                        foundNearPlane = true;
                        bestDist2 = dist2;
                        seed = idx;
                    }
                } else if (!foundNearPlane && dz < bestDz) {
                    bestDz = dz;
                    seed = idx;
                }
            }
        }
    }

    std::vector<uint8_t> reachable(totalVerts, 0);
    std::vector<size_t> queue;
    queue.reserve(totalVerts);
    reachable[seed] = 1;
    queue.push_back(seed);
    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t cur = queue[head];
        const int ci = static_cast<int>(cur % static_cast<size_t>(cols));
        const int cj = static_cast<int>(cur / static_cast<size_t>(cols));
        constexpr int kDx[4] = {-1, 1, 0, 0};
        constexpr int kDy[4] = {0, 0, -1, 1};
        for (int dir = 0; dir < 4; ++dir) {
            const int ni = ci + kDx[dir];
            const int nj = cj + kDy[dir];
            if (ni < 0 || ni >= cols || nj < 0 || nj >= rows) continue;
            const size_t next = static_cast<size_t>(nj * cols + ni);
            if (reachable[next] || !edgeContinuous(cur, next)) continue;
            reachable[next] = 1;
            queue.push_back(next);
        }
    }

    const auto emitTri = [&](size_t i0, size_t i1, size_t i2) {
        if (!reachable[i0] || !reachable[i1] || !reachable[i2]) return;
        if (!edgeContinuous(i0, i1) || !edgeContinuous(i1, i2) || !edgeContinuous(i2, i0)) return;
        out.push_back(grid[i0]);
        out.push_back(grid[i1]);
        out.push_back(grid[i2]);
    };

    out.reserve(static_cast<size_t>(cellsU * cellsV * 6));
    for (int j = 0; j < cellsV; ++j) {
        for (int i = 0; i < cellsU; ++i) {
            const size_t a = static_cast<size_t>(j * cols + i);
            const size_t b = a + 1;
            const size_t d = a + static_cast<size_t>(cols);
            const size_t e = d + 1;
            const int count = reachable[a] + reachable[b] + reachable[d] + reachable[e];
            if (count == 4) {
                emitTri(a, b, e);
                emitTri(a, e, d);
            } else if (count == 3) {
                if (!reachable[e]) emitTri(a, b, d);
                else if (!reachable[d]) emitTri(a, b, e);
                else if (!reachable[b]) emitTri(a, e, d);
                else emitTri(b, e, d);
            }
        }
    }
}

}  // namespace wowee::rendering::projected_texture
