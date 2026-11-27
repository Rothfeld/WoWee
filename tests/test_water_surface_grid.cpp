// Projecting a world position onto a water surface's own grid.
//
// Everything that asks what the water is doing under a point starts here: the
// height under the player, the nearest surface, the liquid type, and whether
// the water belongs to a building. water_renderer.cpp wrote the projection out
// four times, and none of the four was tested.
//
// The failures are all quiet. Project onto the wrong axis and the height comes
// from somewhere else in the lake, which shows up as the player swimming a
// little above or below the surface. Divide by a zero-length step and every
// point in the world is inside that surface.
#include <catch_amalgamated.hpp>

#include "rendering/terrain_wdl.hpp"
#include "rendering/water_surface_grid.hpp"
#include "test_support.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>

using wowee::rendering::emitLiquidCellTriangles;
using wowee::rendering::liquidGridUpwardCcw;
using wowee::rendering::liquidInteriorFogMode;
using wowee::rendering::sampleGridHeight;
using wowee::rendering::surfaceGridPosition;

namespace {

constexpr glm::vec3 kOrigin{100.0f, 200.0f, 30.0f};
/// An axis-aligned surface whose cells are 10 units across.
constexpr glm::vec3 kStepX{10.0f, 0.0f, 0.0f};
constexpr glm::vec3 kStepY{0.0f, 10.0f, 0.0f};

}  // namespace

TEST_CASE("the origin is grid zero", "[watergrid]") {
    const auto grid = surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                          kOrigin.x, kOrigin.y);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(0.0f));
    CHECK(grid->y == Catch::Approx(0.0f));
}

TEST_CASE("a position is measured in cells, not units", "[watergrid]") {
    // 25 units along a 10-unit step is 2.5 cells. Answering 25 would send the
    // caller off the end of a grid that only has nine corners.
    const auto grid = surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                          kOrigin.x + 25.0f, kOrigin.y + 5.0f);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(2.5f));
    CHECK(grid->y == Catch::Approx(0.5f));
}

TEST_CASE("the two axes do not swap", "[watergrid]") {
    // A surface twice as long in one direction as the other tells them apart;
    // a square one cannot. Swapping them reads the height from the far side of
    // the lake, which is a plausible height and therefore invisible.
    const glm::vec3 longX{20.0f, 0.0f, 0.0f};
    const auto grid = surfaceGridPosition(kOrigin, longX, kStepY, 8, 8,
                                          kOrigin.x + 20.0f, kOrigin.y + 10.0f);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(1.0f));
    CHECK(grid->y == Catch::Approx(1.0f));
}

TEST_CASE("a rotated surface projects onto its own axes", "[watergrid]") {
    // WMO water lies at whatever angle its building does. The projection is
    // onto the surface's axes, not the world's, so a point 10 units along a
    // diagonal step is one cell along, not 14.
    const glm::vec3 diagX{6.0f, 8.0f, 0.0f};    // length 10
    const glm::vec3 diagY{-8.0f, 6.0f, 0.0f};   // length 10, perpendicular
    const auto grid = surfaceGridPosition(kOrigin, diagX, diagY, 4, 4,
                                          kOrigin.x + 6.0f, kOrigin.y + 8.0f);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(1.0f));
    CHECK(grid->y == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("a point outside the surface is not on it", "[watergrid]") {
    CHECK_FALSE(surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                    kOrigin.x - 1.0f, kOrigin.y).has_value());
    CHECK_FALSE(surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                    kOrigin.x, kOrigin.y - 1.0f).has_value());
    // Eight cells of ten units is eighty; eighty-one is off the end.
    CHECK_FALSE(surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                    kOrigin.x + 81.0f, kOrigin.y).has_value());
}

TEST_CASE("the far edge belongs to the surface", "[watergrid]") {
    // A grid of eight cells has nine corners, and the last one is on it.
    // Excluding it leaves a one-unit seam of no water between two chunks,
    // which the player falls through.
    const auto grid = surfaceGridPosition(kOrigin, kStepX, kStepY, 8, 8,
                                          kOrigin.x + 80.0f, kOrigin.y + 80.0f);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(8.0f));
    CHECK(grid->y == Catch::Approx(8.0f));
}

TEST_CASE("a degenerate surface claims nothing", "[watergrid]") {
    // A zero-length step would divide by zero and put every point in the world
    // inside this surface, so a malformed chunk would roof the whole map.
    const glm::vec3 none{0.0f, 0.0f, 0.0f};
    CHECK_FALSE(surfaceGridPosition(kOrigin, none, kStepY, 8, 8,
                                    kOrigin.x, kOrigin.y).has_value());
    CHECK_FALSE(surfaceGridPosition(kOrigin, kStepX, none, 8, 8,
                                    kOrigin.x, kOrigin.y).has_value());
}

TEST_CASE("the z components of the steps do not enter into it", "[watergrid]") {
    // The grid is a flat projection: a sloped step vector still measures the
    // same distance across the map. Including z here would shrink the grid of
    // every surface that is not level.
    const glm::vec3 slopedX{10.0f, 0.0f, 5.0f};
    const auto grid = surfaceGridPosition(kOrigin, slopedX, kStepY, 8, 8,
                                          kOrigin.x + 10.0f, kOrigin.y);
    REQUIRE(grid.has_value());
    CHECK(grid->x == Catch::Approx(1.0f));
}

// ---- the height at a grid position ----

namespace {

/// A (width+1) x (height+1) corner grid filled by a rule.
std::vector<float> corners(int width, int height, float (*rule)(int, int)) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>((width + 1) * (height + 1)));
    for (int y = 0; y <= height; ++y) {
        for (int x = 0; x <= width; ++x) out.push_back(rule(x, y));
    }
    return out;
}

}  // namespace

TEST_CASE("a level surface is the same height everywhere", "[watergrid]") {
    const auto heights = corners(8, 8, [](int, int) { return 42.0f; });
    for (float g = 0.0f; g <= 8.0f; g += 2.0f) {
        INFO("at " << g);
        const auto h = sampleGridHeight(heights, 8, 8, g, g);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(42.0f));
    }
}

TEST_CASE("the height is interpolated across the cell", "[watergrid]") {
    // Height equals the x corner index, so halfway along a cell is halfway
    // between its corners. A sampler that snapped to a corner instead would
    // step the water surface in visible terraces.
    const auto heights = corners(8, 8, [](int x, int) { return static_cast<float>(x); });
    CHECK(*sampleGridHeight(heights, 8, 8, 0.5f, 0.0f) == Catch::Approx(0.5f));
    CHECK(*sampleGridHeight(heights, 8, 8, 3.25f, 4.0f) == Catch::Approx(3.25f));
}

TEST_CASE("both axes are interpolated", "[watergrid]") {
    const auto heights = corners(8, 8, [](int x, int y) {
        return static_cast<float>(x) + 10.0f * static_cast<float>(y);
    });
    CHECK(*sampleGridHeight(heights, 8, 8, 0.5f, 0.5f) == Catch::Approx(5.5f));
}

TEST_CASE("the far edge samples the last cell rather than falling off",
          "[watergrid]") {
    // A grid of eight cells has nine corners, so grid position 8 is the last
    // corner, not the start of a ninth cell. Reading it as a cell start walks
    // off the array and drops the answer, which leaves a seam between two
    // surfaces that the player falls through.
    const auto heights = corners(8, 8, [](int x, int) { return static_cast<float>(x); });
    const auto h = sampleGridHeight(heights, 8, 8, 8.0f, 8.0f);
    REQUIRE(h.has_value());
    CHECK(*h == Catch::Approx(8.0f));
}

TEST_CASE("a position behind the grid has no height", "[watergrid]") {
    const auto heights = corners(8, 8, [](int, int) { return 1.0f; });
    CHECK_FALSE(sampleGridHeight(heights, 8, 8, -0.5f, 0.0f).has_value());
    CHECK_FALSE(sampleGridHeight(heights, 8, 8, 0.0f, -0.5f).has_value());
}

TEST_CASE("a short height array answers nothing rather than reading past it",
          "[watergrid]") {
    // A surface whose heights failed to load. Answering a height read from
    // whatever follows the array is worse than answering none.
    const std::vector<float> tooShort(4, 5.0f);
    CHECK_FALSE(sampleGridHeight(tooShort, 8, 8, 4.0f, 4.0f).has_value());
    CHECK_FALSE(sampleGridHeight({}, 8, 8, 0.0f, 0.0f).has_value());
}

TEST_CASE("a non-square surface indexes by its own width", "[watergrid]") {
    // Row stride is width + 1, not height + 1. Swapping them reads the height
    // of a different part of the lake, which is a plausible number.
    const auto heights = corners(4, 8, [](int, int y) { return static_cast<float>(y); });
    CHECK(*sampleGridHeight(heights, 4, 8, 0.0f, 3.0f) == Catch::Approx(3.0f));
    CHECK(*sampleGridHeight(heights, 4, 8, 4.0f, 6.0f) == Catch::Approx(6.0f));
}

TEST_CASE("liquid cell winding keeps upward/outward normals across ADT and WMO grids",
          "[watergrid][sidedisc5]") {
    constexpr float kTile = 4.1666665f;

    auto checkCellNormals = [](const glm::vec3& stepX, const glm::vec3& stepY,
                               float h00, float h10, float h01, float h11) {
        const glm::vec3 p[4] = {
            glm::vec3(0.0f, 0.0f, h00),
            glm::vec3(stepX.x, stepX.y, h10),
            glm::vec3(stepY.x, stepY.y, h01),
            glm::vec3(stepX.x + stepY.x, stepX.y + stepY.y, h11),
        };
        const bool upwardCcw = liquidGridUpwardCcw(stepX, stepY);
        const auto idx = emitLiquidCellTriangles(0u, 1u, 2u, 3u, upwardCcw);
        const glm::vec3 n0 = glm::cross(p[idx[1]] - p[idx[0]], p[idx[2]] - p[idx[0]]);
        const glm::vec3 n1 = glm::cross(p[idx[4]] - p[idx[3]], p[idx[5]] - p[idx[3]]);
        CHECK(n0.z > 0.0f);
        CHECK(n1.z > 0.0f);
        return std::pair{n0, n1};
    };

    // Unmerged ADT chunk: stepX = (0, -T, 0), stepY = (-T, 0, 0).
    CHECK(liquidGridUpwardCcw(glm::vec3(0.0f, -kTile, 0.0f), glm::vec3(-kTile, 0.0f, 0.0f)));
    checkCellNormals(glm::vec3(0.0f, -kTile, 0.0f), glm::vec3(-kTile, 0.0f, 0.0f),
                     0.0f, 0.0f, 0.0f, 0.0f);

    // Merged 128x128 ADT tile: stepX = (-T, 0, 0), stepY = (0, -T, 0).
    CHECK_FALSE(liquidGridUpwardCcw(glm::vec3(-kTile, 0.0f, 0.0f), glm::vec3(0.0f, -kTile, 0.0f)));
    checkCellNormals(glm::vec3(-kTile, 0.0f, 0.0f), glm::vec3(0.0f, -kTile, 0.0f),
                     0.0f, 0.0f, 0.0f, 0.0f);

    // Rotated WMO MLIQ lavafall: local (+T, 0, 0) / (0, +T, 0) rotated 97 deg around Z,
    // sloping steeply upward away from the viewer (FragPos.z = -170 > viewPos.z = -227.8).
    const float c = -0.12186934f;
    const float s = 0.99254615f;
    const glm::vec3 wmoStepX(c * kTile, s * kTile, 0.0f);
    const glm::vec3 wmoStepY(-s * kTile, c * kTile, 0.0f);
    CHECK_FALSE(liquidGridUpwardCcw(wmoStepX, wmoStepY));
    // Lavafall rises +6 yd along +wmoStepY away from a viewer standing 20 cells downhill
    // along -wmoStepY and 54.8 yd below FragPos.z in world Z.
    const auto [n0, n1] = checkCellNormals(wmoStepX, wmoStepY, -176.0f, -176.0f, -170.0f, -170.0f);
    const glm::vec3 fragPos(0.0f, 0.0f, -173.0f);
    const glm::vec3 cavernViewerBelow = fragPos - 20.0f * wmoStepY + glm::vec3(0.0f, 0.0f, -54.8f);
    // Viewer is 54.8 yd below FragPos.z in world Z, yet stands in front of the falling sheet.
    CHECK(cavernViewerBelow.z < fragPos.z);
    CHECK(glm::dot(n0, cavernViewerBelow - fragPos) > 0.0f);
    CHECK(glm::dot(n1, cavernViewerBelow - fragPos) > 0.0f);

    // Submerged viewer directly underneath the sheet sees the back face and culls it.
    const glm::vec3 submergedUnderSheet(0.0f, 0.0f, -190.0f);
    CHECK(glm::dot(n0, submergedUnderSheet - fragPos) < 0.0f);
    CHECK(glm::dot(n1, submergedUnderSheet - fragPos) < 0.0f);
}

TEST_CASE("WMO interior magma uses global Light.dbc fog mode while indoor water uses MFOG",
          "[watergrid][sidedisc5]") {
    CHECK(liquidInteriorFogMode(false, false) == Catch::Approx(0.0f));
    CHECK(liquidInteriorFogMode(false, true) == Catch::Approx(0.0f));
    CHECK(liquidInteriorFogMode(true, false) == Catch::Approx(1.0f));
    CHECK(liquidInteriorFogMode(true, true) == Catch::Approx(2.0f));
}

TEST_CASE("WDL horizon parser decodes MAOF/MARE/MAHO with upward CCW triangles",
          "[watergrid][terrain][sidedisc6]") {
    using wowee::rendering::WdlHorizonMap;
    using wowee::rendering::buildWdlTileMesh;
    using wowee::rendering::kDefaultHorizonFarClip;
    using wowee::rendering::parseWdlHorizon;
    using wowee::rendering::wdlTileWithinHorizonRange;

    // Construct a minimal valid WDL file with MVER + MAOF + 1 MARE + 1 MAHO.
    std::vector<uint8_t> buf;
    auto pushFourCC = [&](const char cc[4], uint32_t sz) {
        buf.push_back(static_cast<uint8_t>(cc[3]));
        buf.push_back(static_cast<uint8_t>(cc[2]));
        buf.push_back(static_cast<uint8_t>(cc[1]));
        buf.push_back(static_cast<uint8_t>(cc[0]));
        const size_t at = buf.size();
        buf.resize(at + 4);
        std::memcpy(buf.data() + at, &sz, 4);
    };
    pushFourCC("MVER", 4);
    const uint32_t ver = 18;
    buf.resize(buf.size() + 4);
    std::memcpy(buf.data() + buf.size() - 4, &ver, 4);

    pushFourCC("MAOF", 4096u * 4u);
    const size_t maofStart = buf.size();
    buf.resize(maofStart + 4096u * 4u, 0);

    const uint32_t mareOffset = static_cast<uint32_t>(buf.size());
    const uint32_t targetIdx = 48u * 64u + 32u; // Northshire tile (row=48, col=32)
    std::memcpy(buf.data() + maofStart + targetIdx * 4u, &mareOffset, 4);

    pushFourCC("MARE", (17u * 17u + 16u * 16u) * 2u);
    for (int i = 0; i < 17 * 17; ++i) {
        const int16_t h = static_cast<int16_t>(80 + (i % 17));
        const size_t at = buf.size();
        buf.resize(at + 2);
        std::memcpy(buf.data() + at, &h, 2);
    }
    for (int i = 0; i < 16 * 16; ++i) {
        const int16_t h = 95;
        const size_t at = buf.size();
        buf.resize(at + 2);
        std::memcpy(buf.data() + at, &h, 2);
    }

    pushFourCC("MAHO", 16u * 2u);
    for (int r = 0; r < 16; ++r) {
        // Punch 2 holes in row 0 (cols 0 and 3)
        const uint16_t mask = (r == 0) ? static_cast<uint16_t>((1u << 0) | (1u << 3)) : 0u;
        const size_t at = buf.size();
        buf.resize(at + 2);
        std::memcpy(buf.data() + at, &mask, 2);
    }

    WdlHorizonMap parsed;
    REQUIRE(parseWdlHorizon(buf, parsed));
    REQUIRE(parsed.tiles.size() == 1);
    CHECK(parsed.tiles[0].maofRow == 48);
    CHECK(parsed.tiles[0].maofCol == 32);

    std::vector<wowee::pipeline::TerrainVertex> verts;
    std::vector<uint32_t> indices;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    buildWdlTileMesh(parsed.tiles[0], verts, indices, center, radius);

    CHECK(verts.size() == 545u);
    // 256 cells minus 2 MAHO holes = 254 cells * 4 tris * 3 indices = 3048 indices
    CHECK(indices.size() == 254u * 12u);
    CHECK(radius > 350.0f);

    // Northshire tile (maofRow=48, maofCol=32 -> ADT tileX=32, tileY=48) NW corner
    // must land in the Northshire quadrant: position[0] = -8533.33, position[1] = 0.0,
    // matching MCNK render coordinates (cam ~ (-9008, -130, 87)).
    CHECK(verts[0].position[0] == Catch::Approx(-16.0f * wowee::core::coords::TILE_SIZE).margin(0.05f));
    CHECK(verts[0].position[1] == Catch::Approx(0.0f).margin(0.05f));
    CHECK(center.x < -8500.0f);
    CHECK(center.x > -9100.0f);
    CHECK(center.y < 0.0f);
    CHECK(center.y > -600.0f);

    // Every emitted triangle must match TerrainMeshGenerator::generateIndices winding
    // (-Z cross product in TerrainRenderer vertex space) so VK_CULL_MODE_BACK_BIT keeps top faces.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& va = verts[indices[i + 0]];
        const auto& vb = verts[indices[i + 1]];
        const auto& vc = verts[indices[i + 2]];
        const glm::vec3 aRender(va.position[0], va.position[1], va.position[2]);
        const glm::vec3 bRender(vb.position[0], vb.position[1], vb.position[2]);
        const glm::vec3 cRender(vc.position[0], vc.position[1], vc.position[2]);
        const glm::vec3 nRender = glm::cross(bRender - aRender, cRender - aRender);
        CHECK(nRender.z < 0.0f);
    }

    // Horizon range annulus check across farclip = 177 and farclip = 2400
    const glm::vec3 cam(0.0f, 0.0f, 85.0f);
    CHECK(wdlTileWithinHorizonRange(glm::vec3(550.0f, 0.0f, 200.0f), 380.0f, cam, 177.0f, kDefaultHorizonFarClip));
    CHECK(wdlTileWithinHorizonRange(glm::vec3(2000.0f, 0.0f, 200.0f), 380.0f, cam, 177.0f, kDefaultHorizonFarClip));
    CHECK_FALSE(wdlTileWithinHorizonRange(glm::vec3(2600.0f, 0.0f, 200.0f), 380.0f, cam, 177.0f, kDefaultHorizonFarClip));
    // A tile completely inside close MCNK range (e.g. farclip = 1900, tile at d = 300, r = 380 -> 300+380=680 < 1852) is skipped
    CHECK_FALSE(wdlTileWithinHorizonRange(glm::vec3(300.0f, 0.0f, 80.0f), 380.0f, cam, 1900.0f, kDefaultHorizonFarClip));
}

TEST_CASE("Real TBC Azeroth.wdl and Expansion01.wdl parse Northshire canyon ridge and shaders clip outdoor water",
          "[watergrid][terrain][sidedisc6]") {
    const std::string terrainVert = wowee::test::slurp("assets/shaders/terrain.vert.glsl");
    const std::string terrainFrag = wowee::test::slurp("assets/shaders/terrain.frag.glsl");
    const std::string waterFrag = wowee::test::slurp("assets/shaders/water.frag.glsl");

    CHECK(terrainVert.find("horizonParams") != std::string::npos);
    CHECK(terrainFrag.find("isHorizonMesh") != std::string::npos);
    CHECK(terrainFrag.find("outdoorFogColor.rgb") != std::string::npos);
    // Outdoor water (interiorFog < 0.5) must clip at outdoorFogParams.z alongside indoor magma (> 1.5).
    CHECK(waterFrag.find("push.interiorFog < 0.5 || push.interiorFog > 1.5") != std::string::npos);

    namespace fs = std::filesystem;
    const fs::path azerothWdl = "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/maps/azeroth/azeroth.wdl";
    if (!fs::is_regular_file(azerothWdl)) {
        WARN("TBC Azeroth.wdl not present on this runner; skipping live archive check");
        return;
    }

    std::ifstream in(azerothWdl, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    wowee::rendering::WdlHorizonMap wdl;
    REQUIRE(wowee::rendering::parseWdlHorizon(raw, wdl));
    CHECK(wdl.tiles.size() == 687u);

    // Northshire box-canyon mountain ridge behind Northshire Abbey sits in MAOF (row=47, col=32)
    // with peaks exceeding z = 320 yd (~530..570 yd north of (-9026.64, -130.0, 85.2)).
    const wowee::rendering::WdlHorizonTile* northRidge = nullptr;
    for (const auto& t : wdl.tiles) {
        if (t.maofRow == 47 && t.maofCol == 32) {
            northRidge = &t;
            break;
        }
    }
    REQUIRE(northRidge != nullptr);
    int16_t maxPeak = -32768;
    for (int16_t h : northRidge->outerHeights) maxPeak = std::max(maxPeak, h);
    CHECK(maxPeak >= 320);
}
