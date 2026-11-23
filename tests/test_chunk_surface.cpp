// Where a point inside a terrain chunk sits in the world.
//
// The ground-clutter scatterer and the doodad scatterer each had their own
// copy of this: interpolate the height between the four surrounding grid
// points, and cross the axes on the way out. Neither was tested.
//
// Nothing fails when it is wrong. Objects sit a little above the ground or a
// little inside it, or a whole chunk's worth of them lands mirrored about the
// diagonal, and it reads as the doodad placement data being wrong.
//
// The bilinear values below are worked out by hand: at (tx, ty) the height is
// h00(1-tx)(1-ty) + h10·tx(1-ty) + h01(1-tx)ty + h11·tx·ty.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "pipeline/terrain_mesh.hpp"

using wowee::pipeline::HeightMap;
using wowee::pipeline::TerrainMeshGenerator;

namespace {

/// A height map filled by a rule, inner vertices included.
///
/// The layout is WoW's interleaved 9x9 outer + 8x8 inner, and the inner grid is
/// not decoration: the mesh fans four triangles from each quad's centre vertex,
/// so that vertex is the surface over the middle of every cell. Leaving it at
/// zero here, as this once did, describes a chunk that is spiked downwards
/// between every corner - which is nothing like the terrain being sampled.
/// Inner vertex (x, y) sits at grid (x + 0.5, y + 0.5).
template <typename Rule>
HeightMap gridWhere(Rule rule) {
    HeightMap map{};
    map.heights.fill(0.0f);
    map.loaded = true;
    for (int y = 0; y <= 8; ++y) {
        for (int x = 0; x <= 8; ++x) {
            map.heights[static_cast<size_t>(y * 17 + x)] =
                rule(static_cast<float>(x), static_cast<float>(y));
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            map.heights[static_cast<size_t>(9 + y * 17 + x)] =
                rule(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        }
    }
    return map;
}

constexpr float kUnitSize = 4.1666665f;   // a chunk is 33.33 yards across 8 cells

}  // namespace

TEST_CASE("a flat chunk is flat everywhere", "[chunksurface]") {
    const HeightMap map = gridWhere([](float, float) { return 100.0f; });
    const float position[3] = {0.0f, 0.0f, 10.0f};

    for (float f = 0.0f; f <= 8.0f; f += 2.0f) {
        INFO("at " << f);
        const glm::vec3 point =
            TerrainMeshGenerator::chunkSurfacePoint(position, map, f, f, kUnitSize);
        CHECK(point.z == Catch::Approx(110.0f));
    }
}

TEST_CASE("the height is interpolated between grid points", "[chunksurface]") {
    // A ramp along the grid's x axis: height equals x.
    const HeightMap map = gridWhere([](float x, float) { return x; });
    const float position[3] = {0.0f, 0.0f, 0.0f};

    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.0f, 0.0f, kUnitSize).z ==
          Catch::Approx(0.0f));
    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.5f, 0.0f, kUnitSize).z ==
          Catch::Approx(0.5f));
    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 3.25f, 5.0f, kUnitSize).z ==
          Catch::Approx(3.25f));
}

TEST_CASE("both axes contribute to the interpolation", "[chunksurface]") {
    // height = x + 10y, so a point halfway along both is 0.5 + 5.
    const HeightMap map =
        gridWhere([](float x, float y) { return x + 10.0f * y; });
    const float position[3] = {0.0f, 0.0f, 0.0f};

    const glm::vec3 point =
        TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.5f, 0.5f, kUnitSize);
    CHECK(point.z == Catch::Approx(5.5f));
}

TEST_CASE("world X comes from the grid's Y and world Y from its X",
          "[chunksurface]") {
    // The pairing that must not be "fixed": swapping these lays every
    // scattered object out mirrored about the chunk diagonal. A point at
    // grid (2, 6) is 6 units of size along negative world X and 2 along
    // negative world Y.
    const HeightMap map = gridWhere([](float, float) { return 0.0f; });
    const float position[3] = {1000.0f, 2000.0f, 0.0f};

    const glm::vec3 point =
        TerrainMeshGenerator::chunkSurfacePoint(position, map, 2.0f, 6.0f, kUnitSize);
    CHECK(point.x == Catch::Approx(1000.0f - 6.0f * kUnitSize));
    CHECK(point.y == Catch::Approx(2000.0f - 2.0f * kUnitSize));
}

TEST_CASE("the chunk position is the origin the point is measured from",
          "[chunksurface]") {
    const HeightMap map = gridWhere([](float, float) { return 7.0f; });
    const float position[3] = {-500.0f, 300.0f, 42.0f};

    const glm::vec3 point =
        TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.0f, 0.0f, kUnitSize);
    CHECK(point.x == Catch::Approx(-500.0f));
    CHECK(point.y == Catch::Approx(300.0f));
    CHECK(point.z == Catch::Approx(49.0f));
}

TEST_CASE("a coordinate past the edge clamps to it", "[chunksurface]") {
    // A caller that rounds a fraction slightly past 8 gets the edge height
    // rather than a zero, which would drop the object through the world.
    const HeightMap map = gridWhere([](float, float) { return 55.0f; });
    const float position[3] = {0.0f, 0.0f, 0.0f};

    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 8.0f, 8.0f, kUnitSize).z ==
          Catch::Approx(55.0f));
    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 8.5f, 8.5f, kUnitSize).z ==
          Catch::Approx(55.0f));
    CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, -1.0f, -1.0f, kUnitSize).z ==
          Catch::Approx(55.0f));
}

TEST_CASE("a slope is sampled continuously across a cell boundary",
          "[chunksurface]") {
    // Just either side of a grid line the height must agree, or scattered
    // objects step up and down along every cell edge.
    const HeightMap map =
        gridWhere([](float x, float y) { return x * 3.0f + y * 2.0f; });
    const float position[3] = {0.0f, 0.0f, 0.0f};

    const float justBelow =
        TerrainMeshGenerator::chunkSurfacePoint(position, map, 3.999f, 2.0f, kUnitSize).z;
    const float justAbove =
        TerrainMeshGenerator::chunkSurfacePoint(position, map, 4.001f, 2.0f, kUnitSize).z;
    CHECK(justBelow == Catch::Approx(justAbove).margin(0.01f));
}

// The surface the player walks on is the surface that gets drawn, and the mesh
// fans four triangles from each quad's centre vertex. Interpolating the four
// outer corners instead ignores that vertex entirely, and MCVT puts it wherever
// the artist needed it - commonly a yard or two off the plane of its corners on
// a hillside. The floor query then answered below the visible ground and the
// player sank into a slope that looked gentle.
TEST_CASE("the quad's centre vertex is part of the surface", "[chunksurface]") {
    HeightMap map{};
    map.heights.fill(0.0f);
    map.loaded = true;
    // One quad, corners at zero, centre raised two yards.
    map.heights[9] = 2.0f;   // inner (0, 0) -> grid (0.5, 0.5)
    const float position[3] = {0.0f, 0.0f, 0.0f};

    SECTION("dead centre reads the centre vertex, not the corner average") {
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.5f, 0.5f, kUnitSize).z ==
              Catch::Approx(2.0f));
    }

    SECTION("the corners are still exactly their own height") {
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.0f, 0.0f, kUnitSize).z ==
              Catch::Approx(0.0f));
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 1.0f, 0.0f, kUnitSize).z ==
              Catch::Approx(0.0f));
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.0f, 1.0f, kUnitSize).z ==
              Catch::Approx(0.0f));
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 1.0f, 1.0f, kUnitSize).z ==
              Catch::Approx(0.0f));
    }

    SECTION("every wedge rises toward the middle") {
        // One sample inside each of the four triangles, all half way from a
        // corner to the centre, so all four must agree.
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.5f, 0.25f, kUnitSize).z ==
              Catch::Approx(1.0f));   // top
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.75f, 0.5f, kUnitSize).z ==
              Catch::Approx(1.0f));   // right
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.5f, 0.75f, kUnitSize).z ==
              Catch::Approx(1.0f));   // bottom
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, 0.25f, 0.5f, kUnitSize).z ==
              Catch::Approx(1.0f));   // left
    }
}

// A plane is reproduced exactly by any correct barycentric split, so this
// catches a wedge whose weights do not sum to one or name the wrong corner.
TEST_CASE("a tilted plane is exact in all four wedges", "[chunksurface]") {
    const HeightMap map = gridWhere([](float x, float y) { return 3.0f * x + 7.0f * y + 5.0f; });
    const float position[3] = {0.0f, 0.0f, 0.0f};
    const float samples[][2] = {
        {2.5f, 2.25f}, {2.75f, 2.5f}, {2.5f, 2.75f}, {2.25f, 2.5f},
        {0.1f, 0.2f},  {7.9f, 7.8f},  {4.5f, 4.5f},  {3.3f, 6.7f},
    };
    for (const auto& s : samples) {
        INFO("at " << s[0] << ", " << s[1]);
        CHECK(TerrainMeshGenerator::chunkSurfacePoint(position, map, s[0], s[1], kUnitSize).z ==
              Catch::Approx(3.0f * s[0] + 7.0f * s[1] + 5.0f));
    }
}

TEST_CASE("surface normals follow individual fan triangles across a crease", "[chunksurface]") {
    HeightMap map = gridWhere([](float, float) { return 0.0f; });
    map.heights[9] = 3.0f;
    const float position[3] = {0.0f, 0.0f, 0.0f};
    const glm::vec2 samples[] = {{0.50f, 0.49f}, {0.51f, 0.50f},
                                  {0.49f, 0.50f}, {0.50f, 0.51f}};
    const glm::vec3 expected[] = {{6.0f, 0.0f, kUnitSize},
                                  {0.0f, -6.0f, kUnitSize},
                                  {0.0f, 6.0f, kUnitSize},
                                  {-6.0f, 0.0f, kUnitSize}};
    for (size_t i = 0; i < 4; ++i) {
        glm::vec3 normal;
        TerrainMeshGenerator::chunkSurfacePoint(position, map, samples[i].x,
            samples[i].y, kUnitSize, &normal);
        const glm::vec3 direction = glm::normalize(expected[i]);
        CHECK(normal.x == Catch::Approx(direction.x));
        CHECK(normal.y == Catch::Approx(direction.y));
        CHECK(normal.z == Catch::Approx(direction.z));
    }
}

TEST_CASE("quad triangles are the surface chunkSurfacePoint samples", "[chunksurface]") {
    // Uneven everywhere, the centre vertices off their corners' plane, so a
    // wrong fan or a crossed axis puts a sample off the triangles.
    const HeightMap map = gridWhere([](float x, float y) {
        return 3.0f * std::sin(1.3f * x) + 2.0f * std::cos(0.7f * y) + 0.4f * x * y;
    });
    const float position[3] = {100.0f, -40.0f, 25.0f};
    for (int qy = 0; qy < 8; qy += 3) {
        for (int qx = 0; qx < 8; qx += 2) {
            const auto tris = TerrainMeshGenerator::chunkQuadTriangles(position, map, qx, qy, kUnitSize);
            for (float u = 0.05f; u < 1.0f; u += 0.15f) {
                for (float v = 0.05f; v < 1.0f; v += 0.15f) {
                    const glm::vec3 point = TerrainMeshGenerator::chunkSurfacePoint(
                        position, map, qx + u, qy + v, kUnitSize);
                    // The one triangle whose XY contains the point.
                    int found = 0;
                    for (size_t t = 0; t < tris.size(); t += 3) {
                        const glm::vec3 &a = tris[t], &b = tris[t + 1], &c = tris[t + 2];
                        const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                        const float l1 = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / d;
                        const float l2 = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / d;
                        const float l3 = 1.0f - l1 - l2;
                        if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                        ++found;
                        INFO("quad " << qx << "," << qy << " at " << u << "," << v);
                        CHECK(l1 * a.z + l2 * b.z + l3 * c.z == Catch::Approx(point.z).margin(1e-3));
                    }
                    CHECK(found >= 1);
                }
            }
        }
    }
}

TEST_CASE("tile-indexed chunkQuadTriangles match TerrainMeshGenerator vertices bit-for-bit at large coords",
          "[chunksurface]") {
    // Sunstrider Isle tile (tileX=12, tileY=43 -> X~-6361, Y~10348): projected
    // ground decals (unit shadows, selection circle) must share exact vertex
    // coordinates with the rendered terrain mesh to avoid depth z-fighting.
    wowee::pipeline::ADTTerrain terrain;
    terrain.loaded = true;
    terrain.coord = {12, 43};
    const int cx = 7, cy = 11;
    auto& chunk = terrain.getChunk(cx, cy);
    chunk.flags = 0;
    chunk.position[2] = 34.08f;
    chunk.heightMap = gridWhere([](float x, float y) {
        return 1.5f * std::sin(0.8f * x) + 0.9f * std::cos(0.5f * y);
    });
    const auto mesh = TerrainMeshGenerator::generate(terrain);
    const auto& cm = mesh.getChunk(cx, cy);
    REQUIRE(cm.isValid());

    for (int qy = 0; qy < 8; ++qy) {
        for (int qx = 0; qx < 8; ++qx) {
            const auto tris = TerrainMeshGenerator::chunkQuadTriangles(
                terrain.coord.x, terrain.coord.y, cx, cy, chunk.heightMap, chunk.position[2], qx, qy);
            const int centreIdx = 9 + qy * 17 + qx;
            const int expectedIdx[12] = {
                centreIdx, centreIdx - 9, centreIdx - 8,
                centreIdx, centreIdx - 8, centreIdx + 9,
                centreIdx, centreIdx + 9, centreIdx + 8,
                centreIdx, centreIdx + 8, centreIdx - 9,
            };
            for (int k = 0; k < 12; ++k) {
                const auto& v = cm.vertices[expectedIdx[k]];
                CHECK(tris[k].x == v.position[0]);
                CHECK(tris[k].y == v.position[1]);
                CHECK(tris[k].z == v.position[2]);
            }
        }
    }
}

TEST_CASE("projection * (view * worldPos) preserves painting-in-front-of-wall depth at Karazhan coords",
          "[chunksurface][depth]") {
    // Karazhan Banquet Hall south wall (~x=-11015, y=-1900, z=86):
    // painting canvas sits 0.1468 yd in front of the stone wall.
    // Left-to-right (projection * view) * worldPos suffers float32 cancellation
    // at |worldPos| ~ 11000, inverting NDC z against projection * (view * worldPos).
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.22f, 30000.0f);
    proj[1][1] *= -1.0f;

    const glm::vec4 wall(-11015.0f, -1900.0f, 86.0f, 1.0f);
    const glm::vec4 paint(-11014.853f, -1900.0f, 86.0f, 1.0f);
    const glm::vec3 fwd = glm::normalize(glm::vec3(-0.97f, 0.0f, -0.24f));
    const glm::vec3 up(0.0f, 0.0f, 1.0f);

    for (float camX : {-10975.0f, -10970.0f, -10960.0f, -10955.0f}) {
        const glm::vec3 eye(camX, -1900.0f, 91.0f);
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, up);

        const glm::vec4 clipWall = proj * (view * wall);
        const glm::vec4 clipPaint = proj * (view * paint);
        const float ndcWall = clipWall.z / clipWall.w;
        const float ndcPaint = clipPaint.z / clipPaint.w;

        INFO("camX=" << camX << " ndcPaint=" << ndcPaint << " ndcWall=" << ndcWall);
        CHECK(ndcPaint < ndcWall);
    }

    // Guard against reintroducing unparenthesized "projection * view *" in any GLSL shader.
    namespace fs = std::filesystem;
    fs::path shaderDir = fs::path("assets") / "shaders";
    if (!fs::exists(shaderDir)) {
        shaderDir = fs::path(__FILE__).parent_path().parent_path() / "assets" / "shaders";
    }
    REQUIRE(fs::exists(shaderDir));
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(shaderDir)) {
        if (entry.path().extension() != ".glsl") continue;
        ++checked;
        std::ifstream in(entry.path());
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            INFO(entry.path().filename().string() << ":" << lineNo << ": " << line);
            CHECK(line.find("projection * view *") == std::string::npos);
        }
    }
    CHECK(checked > 20);
}
