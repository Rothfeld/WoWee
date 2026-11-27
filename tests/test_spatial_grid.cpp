// The uniform grid that narrows a collision query.
//
// This was two copies, one in the M2 renderer and one in the WMO renderer,
// both private and both reachable only through a Vulkan context, so neither
// had ever been tested. Its failure mode is not a crash: hand back too few
// candidates and the caller finds no floor and no wall, which reads as falling
// through the world rather than as a bug in a hash table.
//
// The values below are worked out by hand from the cell size, not recorded
// from the implementation.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "rendering/spatial_grid.hpp"

using namespace wowee::rendering;

namespace {

/// Every id the grid offers for a query box.
std::vector<uint32_t> query(const SpatialGrid& grid, const glm::vec3& lo,
                            const glm::vec3& hi) {
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> out;
    gatherIds(grid, lo, hi, seen, [&](uint32_t id) { out.push_back(id); });
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("a position maps to the cell that contains it", "[spatialgrid]") {
    // Cells are 64 wide, so 0..63.99 is cell 0 and 64 begins cell 1.
    CHECK(toSpatialCell({0.0f, 0.0f, 0.0f}) == GridCell{0, 0, 0});
    CHECK(toSpatialCell({63.9f, 0.0f, 0.0f}) == GridCell{0, 0, 0});
    CHECK(toSpatialCell({64.0f, 0.0f, 0.0f}) == GridCell{1, 0, 0});
    CHECK(toSpatialCell({130.0f, 200.0f, 5.0f}) == GridCell{2, 3, 0});
}

TEST_CASE("negative positions floor rather than truncate", "[spatialgrid]") {
    // The trap. A cast to int truncates towards zero, which would put -0.5 and
    // +0.5 in the same cell and make cell 0 twice as wide as every other. Half
    // of every map is at negative coordinates, so this is not an edge case.
    CHECK(toSpatialCell({-0.5f, 0.0f, 0.0f}) == GridCell{-1, 0, 0});
    CHECK(toSpatialCell({-63.9f, 0.0f, 0.0f}) == GridCell{-1, 0, 0});
    CHECK(toSpatialCell({-64.0f, 0.0f, 0.0f}) == GridCell{-1, 0, 0});
    CHECK(toSpatialCell({-64.1f, 0.0f, 0.0f}) == GridCell{-2, 0, 0});
    CHECK(toSpatialCell({-1.0f, -1.0f, -1.0f}) == GridCell{-1, -1, -1});
}

TEST_CASE("an instance is filed under every cell its bounds reach", "[spatialgrid]") {
    SpatialGrid grid;
    // Spans two cells in x and two in y, one in z: four cells.
    insertBounds(grid, {10.0f, 10.0f, 10.0f}, {70.0f, 70.0f, 20.0f}, 7);

    CHECK(grid.size() == 4);
    for (int y = 0; y <= 1; ++y) {
        for (int x = 0; x <= 1; ++x) {
            INFO("cell " << x << "," << y);
            auto it = grid.find(GridCell{x, y, 0});
            REQUIRE(it != grid.end());
            CHECK(it->second == std::vector<uint32_t>{7});
        }
    }
}

TEST_CASE("a query finds what overlaps it and nothing else", "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}, 1);       // cell 0,0,0
    insertBounds(grid, {200.0f, 0.0f, 0.0f}, {210.0f, 10.0f, 10.0f}, 2);    // cell 3,0,0
    insertBounds(grid, {-10.0f, 0.0f, 0.0f}, {-5.0f, 10.0f, 10.0f}, 3);     // cell -1,0,0

    CHECK(query(grid, {1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}) == std::vector<uint32_t>{1});
    CHECK(query(grid, {-8.0f, 1.0f, 1.0f}, {-7.0f, 2.0f, 2.0f}) == std::vector<uint32_t>{3});
    // A box wide enough to touch the first and last cells picks up all three,
    // including the one in the empty cell between them, which is skipped.
    CHECK(query(grid, {-10.0f, 1.0f, 1.0f}, {205.0f, 2.0f, 2.0f}) ==
          std::vector<uint32_t>{1, 2, 3});
    CHECK(query(grid, {1000.0f, 1000.0f, 1000.0f}, {1001.0f, 1001.0f, 1001.0f}).empty());
}

TEST_CASE("an instance spanning several cells is offered once", "[spatialgrid]") {
    // Without the seen set the caller would test the same wall four times, and
    // a raycast that counts hits would count each of them.
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 10.0f, 0.0f}, {70.0f, 70.0f, 0.0f}, 9);
    CHECK(query(grid, {0.0f, 0.0f, 0.0f}, {128.0f, 128.0f, 0.0f}) ==
          std::vector<uint32_t>{9});
}

TEST_CASE("a query box crossing the origin covers both sides", "[spatialgrid]") {
    // Cell -1 and cell 0 are different cells, so a box straddling zero has to
    // walk both. If toSpatialCell truncated, minCell and maxCell would both be
    // 0 and everything to the west would be invisible.
    SpatialGrid grid;
    insertBounds(grid, {-20.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, 1);
    insertBounds(grid, {20.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, 2);
    CHECK(query(grid, {-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}) ==
          std::vector<uint32_t>{1, 2});
}

TEST_CASE("the scratch set is cleared by the query, not by the caller",
          "[spatialgrid]") {
    // Both callers hold one thread_local set and reuse it for every query. If
    // it kept its contents, the second query on a thread would drop every id
    // the first one saw.
    SpatialGrid grid;
    insertBounds(grid, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 5);

    std::unordered_set<uint32_t> seen{5, 6, 7};
    std::vector<uint32_t> out;
    gatherIds(grid, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, seen,
              [&](uint32_t id) { out.push_back(id); });
    CHECK(out == std::vector<uint32_t>{5});
}

TEST_CASE("inverted bounds file nothing rather than looping backwards",
          "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {100.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1);
    CHECK(grid.empty());
    CHECK(query(grid, {100.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}).empty());
}

TEST_CASE("erasing takes an instance out of every cell it was in", "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 10.0f, 0.0f}, {70.0f, 70.0f, 0.0f}, 1);
    insertBounds(grid, {10.0f, 10.0f, 0.0f}, {70.0f, 70.0f, 0.0f}, 2);
    eraseBounds(grid, {10.0f, 10.0f, 0.0f}, {70.0f, 70.0f, 0.0f}, 1);

    CHECK(query(grid, {0.0f, 0.0f, 0.0f}, {128.0f, 128.0f, 0.0f}) ==
          std::vector<uint32_t>{2});
    // The cells stay, because the zone will fill them again.
    CHECK(grid.size() == 4);
}

TEST_CASE("erasing with the bounds an instance has moved to strands it",
          "[spatialgrid]") {
    // The reason erase takes bounds rather than reading the instance: file at
    // A, move to B, then erase with B, and the entry at A is still there. Both
    // callers erase with the old box for exactly this reason, and this pins
    // what goes wrong if one stops.
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 1);   // cell 0
    eraseBounds(grid, {200.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 0.0f}, 1);  // cell 3
    CHECK(query(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}) ==
          std::vector<uint32_t>{1});

    eraseBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 1);
    CHECK(query(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}).empty());
}

TEST_CASE("erasing an id that was never filed changes nothing", "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 1);
    eraseBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 99);
    eraseBounds(grid, {5000.0f, 0.0f, 0.0f}, {5000.0f, 0.0f, 0.0f}, 1);
    CHECK(query(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}) ==
          std::vector<uint32_t>{1});
}

TEST_CASE("a move inside one cell refiles nothing", "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, 1);
    refileBounds(grid, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f},
                 {11.0f, 0.0f, 0.0f}, {21.0f, 0.0f, 0.0f}, 1);
    // Listed once, not twice: refiling without the check would insert a second
    // entry in the cell it never left.
    auto it = grid.find(GridCell{0, 0, 0});
    REQUIRE(it != grid.end());
    CHECK(it->second == std::vector<uint32_t>{1});
}

TEST_CASE("a move across a cell boundary refiles", "[spatialgrid]") {
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, 1);
    refileBounds(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f},
                 {100.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, 1);

    CHECK(query(grid, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}).empty());
    CHECK(query(grid, {100.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}) ==
          std::vector<uint32_t>{1});
}

TEST_CASE("growing within the same min cell still refiles", "[spatialgrid]") {
    // The max corner crossing a boundary is enough on its own: the instance
    // now reaches a cell it is not filed under.
    SpatialGrid grid;
    insertBounds(grid, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, 1);
    refileBounds(grid, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f},
                 {10.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, 1);
    CHECK(query(grid, {70.0f, 0.0f, 0.0f}, {70.0f, 0.0f, 0.0f}) ==
          std::vector<uint32_t>{1});
}

// ---- the dense per-group grid a WMO carries for its own triangles ----

TEST_CASE("a box covering the whole group covers every cell", "[spatialgrid]") {
    // Ten cells across an extent of 100, so each cell is 10 wide.
    const auto range = cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                         0.0f, 0.0f, 100.0f, 100.0f);
    REQUIRE(range.has_value());
    CHECK(range->minX == 0);
    CHECK(range->minY == 0);
    CHECK(range->maxX == 9);
    CHECK(range->maxY == 9);
    CHECK(range->count() == 100u);
}

TEST_CASE("a small box covers the cells it actually touches", "[spatialgrid]") {
    const auto range = cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                         25.0f, 35.0f, 26.0f, 36.0f);
    REQUIRE(range.has_value());
    CHECK(range->minX == 2);
    CHECK(range->maxX == 2);
    CHECK(range->minY == 3);
    CHECK(range->maxY == 3);
    CHECK(range->count() == 1u);
}

TEST_CASE("a box straddling a cell edge covers both", "[spatialgrid]") {
    // The case a collision query depends on: a capsule sitting on the line
    // between two cells has to test the triangles of both, or it falls
    // through wherever the floor triangle happens to be filed.
    const auto range = cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                         19.0f, 5.0f, 21.0f, 5.0f);
    REQUIRE(range.has_value());
    CHECK(range->minX == 1);
    CHECK(range->maxX == 2);
}

TEST_CASE("the grid origin shifts the query", "[spatialgrid]") {
    // A group's grid starts at its own bounding box, not at the world origin.
    const auto range = cellRangeCovering(10, 10, 100.0f, 100.0f, {1000.0f, -500.0f},
                                         1025.0f, -465.0f, 1026.0f, -464.0f);
    REQUIRE(range.has_value());
    CHECK(range->minX == 2);
    CHECK(range->minY == 3);
}

TEST_CASE("a box past the far edge is clamped, not extended", "[spatialgrid]") {
    const auto range = cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                         50.0f, 50.0f, 5000.0f, 5000.0f);
    REQUIRE(range.has_value());
    CHECK(range->maxX == 9);
    CHECK(range->maxY == 9);
}

TEST_CASE("a box on the far edge covers the last cell", "[spatialgrid]") {
    // A wall lying on the group's bounding-box face: its whole XY box sits on
    // the maximum, which divides out to index `cells`.
    const auto range = cellRangeCovering(6, 6, 23.17f, 23.17f, {0.0f, 0.0f},
                                         15.3f, 23.17f, 19.2f, 23.17f);
    REQUIRE(range.has_value());
    CHECK(range->minY == 5);
    CHECK(range->maxY == 5);
}

TEST_CASE("a box entirely past the grid covers nothing", "[spatialgrid]") {
    // Clamping min to 0 and max to cells-1 can leave min above max, which is
    // the only signal that the box missed: iterating anyway walks backwards
    // over the whole grid.
    CHECK_FALSE(cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                  -50.0f, 5.0f, -10.0f, 6.0f).has_value());
    CHECK_FALSE(cellRangeCovering(10, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                  5.0f, 500.0f, 6.0f, 600.0f).has_value());
}

TEST_CASE("a group with no grid answers nothing", "[spatialgrid]") {
    CHECK_FALSE(cellRangeCovering(0, 10, 100.0f, 100.0f, {0.0f, 0.0f},
                                  0.0f, 0.0f, 1.0f, 1.0f).has_value());
    CHECK_FALSE(cellRangeCovering(10, 0, 100.0f, 100.0f, {0.0f, 0.0f},
                                  0.0f, 0.0f, 1.0f, 1.0f).has_value());
}

TEST_CASE("a flat group does not divide by zero", "[spatialgrid]") {
    // A doorway or a railing can be modelled as a single plane, so one extent
    // is genuinely zero. The guard keeps the cell size finite; what matters is
    // that an answer comes back at all.
    const auto range = cellRangeCovering(4, 4, 100.0f, 0.0f, {0.0f, 0.0f},
                                         0.0f, 0.0f, 100.0f, 0.0f);
    REQUIRE(range.has_value());
    CHECK(range->minY == 0);
}

#include "rendering/wmo_renderer.hpp"
#include <random>

namespace {

bool rayHitsAabbRangeTest(const glm::vec3& origin, const glm::vec3& dir,
                          const glm::vec3& bmin, const glm::vec3& bmax,
                          float& outMin, float& outMax) {
    float tmin = -1e30f;
    float tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            float t0 = (bmin[i] - origin[i]) / dir[i];
            float t1 = (bmax[i] - origin[i]) / dir[i];
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    outMin = std::max(tmin, 0.0f);
    outMax = tmax;
    return true;
}

float rayTriMollerTrumbore(const glm::vec3& origin, const glm::vec3& dir,
                           const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) {
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 h = glm::cross(dir, e2);
    const float a = glm::dot(e1, h);
    if (std::abs(a) < 1e-6f) return -1.0f;
    const float f = 1.0f / a;
    const glm::vec3 s = origin - v0;
    const float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = f * glm::dot(e2, q);
    return t > 1e-5f ? t : -1.0f;
}

} // namespace

TEST_CASE("WMORenderer::walkGridRayCells matches brute-force triangle hits while visiting O(N) cells",
          "[spatialgrid][sunray][wmo]") {
    using wowee::rendering::WMORenderer;

    // 64x64 grid over a 256x256x140 Shattrath-scale WMO group containing floors,
    // walls, arches, and a multi-cell elevated roof slab spanning several cells.
    constexpr int kCellsX = 64;
    constexpr int kCellsY = 64;
    const glm::vec3 boxMin(-128.0f, -128.0f, -20.0f);
    const glm::vec3 boxMax(128.0f, 128.0f, 120.0f);
    const glm::vec2 gridOrigin(boxMin.x, boxMin.y);
    const float extX = boxMax.x - boxMin.x;
    const float extY = boxMax.y - boxMin.y;

    struct Tri {
        glm::vec3 v0, v1, v2;
        float minZ, maxZ;
    };
    std::vector<Tri> tris;
    std::mt19937 rng(0x5A771234u);
    std::uniform_real_distribution<float> xyDist(-120.0f, 120.0f);
    std::uniform_real_distribution<float> zDist(-18.0f, 115.0f);
    std::uniform_real_distribution<float> spanDist(-14.0f, 14.0f);

    // Explicit multi-cell horizontal roof spanning 6x6 cells at z = 65: a sloped
    // ray entering earlier cells at z = 10 must NOT drop this triangle via early
    // dedup before climbing to z = 65 in a later cell.
    tris.push_back({{-20.0f, -20.0f, 65.0f}, {12.0f, -20.0f, 65.0f}, {12.0f, 12.0f, 65.0f}, 65.0f, 65.0f});
    tris.push_back({{-20.0f, -20.0f, 65.0f}, {12.0f, 12.0f, 65.0f}, {-20.0f, 12.0f, 65.0f}, 65.0f, 65.0f});
    // Cell-boundary vertical wall exactly on x = 0.0f (cell 31/32 boundary).
    tris.push_back({{0.0f, -16.0f, 0.0f}, {0.0f, 16.0f, 0.0f}, {0.0f, 16.0f, 40.0f}, 0.0f, 40.0f});
    tris.push_back({{0.0f, -16.0f, 0.0f}, {0.0f, 16.0f, 40.0f}, {0.0f, -16.0f, 40.0f}, 0.0f, 40.0f});

    for (int i = 0; i < 600; ++i) {
        const glm::vec3 base(xyDist(rng), xyDist(rng), zDist(rng));
        const glm::vec3 v0 = base;
        const glm::vec3 v1 = glm::clamp(base + glm::vec3(spanDist(rng), spanDist(rng), spanDist(rng)), boxMin, boxMax);
        const glm::vec3 v2 = glm::clamp(base + glm::vec3(spanDist(rng), spanDist(rng), spanDist(rng)), boxMin, boxMax);
        if (glm::length(glm::cross(v1 - v0, v2 - v0)) < 0.05f) continue;
        tris.push_back({v0, v1, v2, std::min({v0.z, v1.z, v2.z}), std::max({v0.z, v1.z, v2.z})});
    }

    std::vector<std::vector<uint32_t>> cellTris(kCellsX * kCellsY);
    for (uint32_t ti = 0; ti < tris.size(); ++ti) {
        const auto& t = tris[ti];
        const auto cr = cellRangeCovering(
            kCellsX, kCellsY, extX, extY, gridOrigin,
            std::min({t.v0.x, t.v1.x, t.v2.x}),
            std::min({t.v0.y, t.v1.y, t.v2.y}),
            std::max({t.v0.x, t.v1.x, t.v2.x}),
            std::max({t.v0.y, t.v1.y, t.v2.y}));
        REQUIRE(cr.has_value());
        for (int cy = cr->minY; cy <= cr->maxY; ++cy) {
            for (int cx = cr->minX; cx <= cr->maxX; ++cx) {
                cellTris[cy * kCellsX + cx].push_back(ti);
            }
        }
    }

    // 1. Shattrath diagonal afternoon sun ray cell-count check (< 5% of 4,096 cells).
    {
        const glm::vec3 origin(-110.0f, -105.0f, -12.0f);
        const glm::vec3 sunDir = glm::normalize(glm::vec3(0.45f, 0.68f, 0.58f));
        float tEnter = 0.0f, tExit = 0.0f;
        REQUIRE(rayHitsAabbRangeTest(origin, sunDir, boxMin, boxMax, tEnter, tExit));

        size_t walkedCells = 0;
        WMORenderer::walkGridRayCells(
            kCellsX, kCellsY, boxMin, boxMax, gridOrigin, origin, sunDir, tEnter, tExit,
            [&](int, int, float, float, float) {
                ++walkedCells;
                return true;
            });
        // Full 2D bounding box covers > 2,000 cells; 1D ray walk visits < 160 (< 4% of 4,096).
        CHECK(walkedCells > 0);
        CHECK(walkedCells < 160);

        // Early-exit any-hit stops immediately on the first cell returning false.
        size_t earlyStopCells = 0;
        const bool completed = WMORenderer::walkGridRayCells(
            kCellsX, kCellsY, boxMin, boxMax, gridOrigin, origin, sunDir, tEnter, tExit,
            [&](int, int, float, float, float) {
                ++earlyStopCells;
                return false;
            });
        CHECK_FALSE(completed);
        CHECK(earlyStopCells == 1);
    }

    // 2. Exact closest-hit & any-hit equivalence across 1,500 random + targeted rays.
    std::vector<uint8_t> visited(tris.size(), 0);
    std::vector<uint32_t> touched;
    auto queryFastClosest = [&](const glm::vec3& orig, const glm::vec3& dir, float maxDist) {
        float tEnter = 0.0f, tExit = 0.0f;
        if (!rayHitsAabbRangeTest(orig, dir, boxMin, boxMax, tEnter, tExit) || tEnter > maxDist) {
            return maxDist;
        }
        tExit = std::min(tExit, maxDist);
        float closest = maxDist;
        touched.clear();
        WMORenderer::walkGridRayCells(
            kCellsX, kCellsY, boxMin, boxMax, gridOrigin, orig, dir, tEnter, tExit,
            [&](int cx, int cy, float tc0, float cellZMin, float cellZMax) {
                if (tc0 > closest + 0.05f) return true;
                for (uint32_t ti : cellTris[cy * kCellsX + cx]) {
                    if (visited[ti]) continue;
                    if (tris[ti].maxZ < cellZMin || tris[ti].minZ > cellZMax) continue;
                    visited[ti] = 1;
                    touched.push_back(ti);
                    const float t = rayTriMollerTrumbore(orig, dir, tris[ti].v0, tris[ti].v1, tris[ti].v2);
                    if (t > 0.0f && t < closest) closest = t;
                }
                return true;
            });
        for (uint32_t ti : touched) visited[ti] = 0;
        return closest;
    };

    auto queryBruteClosest = [&](const glm::vec3& orig, const glm::vec3& dir, float maxDist) {
        float closest = maxDist;
        for (const auto& t : tris) {
            const float hit = rayTriMollerTrumbore(orig, dir, t.v0, t.v1, t.v2);
            if (hit > 0.0f && hit < closest) closest = hit;
        }
        return closest;
    };

    // Targeted sloped ray climbing from z = 5 under (-20, -20) up through the multi-cell roof at z = 65:
    {
        const glm::vec3 orig(-18.0f, -18.0f, 5.0f);
        const glm::vec3 dir = glm::normalize(glm::vec3(24.0f, 22.0f, 60.0f));
        CHECK(queryFastClosest(orig, dir, 300.0f) == Catch::Approx(queryBruteClosest(orig, dir, 300.0f)).margin(1e-4f));
    }
    // Targeted ray piercing the cell-boundary wall at x = 0.0f:
    {
        const glm::vec3 orig(-10.0f, 0.0f, 15.0f);
        const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.0f, 0.1f));
        CHECK(queryFastClosest(orig, dir, 300.0f) == Catch::Approx(queryBruteClosest(orig, dir, 300.0f)).margin(1e-4f));
    }

    std::uniform_real_distribution<float> dirDist(-1.0f, 1.0f);
    for (int r = 0; r < 1500; ++r) {
        const glm::vec3 orig(xyDist(rng), xyDist(rng), zDist(rng));
        glm::vec3 dir(dirDist(rng), dirDist(rng), dirDist(rng));
        if (glm::length(dir) < 0.05f) dir = glm::vec3(0.5f, 0.5f, 0.7071f);
        dir = glm::normalize(dir);
        const float fast = queryFastClosest(orig, dir, 400.0f);
        const float brute = queryBruteClosest(orig, dir, 400.0f);
        REQUIRE(fast == Catch::Approx(brute).margin(1e-4f));
    }
}
