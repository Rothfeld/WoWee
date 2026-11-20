// Projected-texture quads (M2 skin batch flag 0x4) draped over the ground.
#include <catch_amalgamated.hpp>

#include "rendering/projected_texture.hpp"

namespace pt = wowee::rendering::projected_texture;

TEST_CASE("quad corners come out in order around the quad", "[projected_texture]") {
    // Flamestrike_Impact_Base's LavaGround quad is two triangles over a
    // shared diagonal; any index order of that kind gives a cycle.
    const std::array<uint16_t, 6> tri{120, 121, 122, 122, 123, 120};
    const auto c = pt::orderQuadCorners(std::span<const uint16_t, 6>(tri));
    // Diagonal is 120-122, so they must sit opposite each other.
    REQUIRE(c[1] != c[3]);
    REQUIRE(((c[1] == 120 && c[3] == 122) || (c[1] == 122 && c[3] == 120)));
    REQUIRE(((c[0] == 121 && c[2] == 123) || (c[0] == 123 && c[2] == 121)));
}

TEST_CASE("orderQuadCorners and buildGrid preserve authored front-face winding", "[projected_texture]") {
    // ResistanceAura_Base.m2 (Shadow/Frost/Fire Resistance, Sanctity, Crusader
    // Auras) authors single-sided quads with CCW (+Z) index order [0,1,2, 2,3,0].
    // DevotionAura_Base.m2 authors [3,2,1, 0,1,2]. Both must keep +Z winding in
    // buildGrid so Vulkan backface culling does not discard the projected quad.
    const std::array<glm::vec3, 4> resPos{{
        {0.0f, -0.78f, 0.06f},
        {0.0f,  0.00f, 0.06f},
        {-0.78f, 0.00f, 0.06f},
        {-0.78f, -0.78f, 0.06f},
    }};
    const std::array<uint16_t, 6> resTri{0, 1, 2, 2, 3, 0};
    const auto resOrd = pt::orderQuadCorners(std::span<const uint16_t, 6>(resTri));
    REQUIRE(resOrd == std::array<uint16_t, 4>{1, 2, 3, 0});

    std::array<pt::Corner, 4> corners;
    for (int k = 0; k < 4; ++k) {
        corners[k] = {resPos[resOrd[k]], {0, 0, 1}, {0, 0}, {0, 0}};
    }
    std::vector<pt::Vertex> out;
    pt::buildGrid(corners, glm::vec3(0), [](float, float, float, float) { return std::optional<float>{}; }, out);
    REQUIRE(!out.empty());
    REQUIRE(out.size() % 3 == 0);
    for (size_t i = 0; i < out.size(); i += 3) {
        const glm::vec3 a(out[i].data[0], out[i].data[1], out[i].data[2]);
        const glm::vec3 b(out[i + 1].data[0], out[i + 1].data[1], out[i + 1].data[2]);
        const glm::vec3 c(out[i + 2].data[0], out[i + 2].data[1], out[i + 2].data[2]);
        REQUIRE(glm::cross(b - a, c - a).z > 0.0f);
    }

    const std::array<glm::vec3, 4> devPos{{
        {-0.34f, -1.48f, 0.23f},
        { 0.34f, -1.48f, 0.23f},
        {-0.34f, -0.33f, 0.23f},
        { 0.34f, -0.33f, 0.23f},
    }};
    const std::array<uint16_t, 6> devTri{3, 2, 1, 0, 1, 2};
    const auto devOrd = pt::orderQuadCorners(std::span<const uint16_t, 6>(devTri));
    REQUIRE(devOrd == std::array<uint16_t, 4>{3, 2, 0, 1});
    const glm::vec3 origN = glm::cross(devPos[devTri[1]] - devPos[devTri[0]],
                                       devPos[devTri[2]] - devPos[devTri[0]]);
    const glm::vec3 gridN = glm::cross(devPos[devOrd[1]] - devPos[devOrd[0]],
                                       devPos[devOrd[2]] - devPos[devOrd[0]]);
    REQUIRE(origN.z > 0.0f);
    REQUIRE(gridN.z > 0.0f);
}

TEST_CASE("grid follows the surface and keeps the quad's texture mapping", "[projected_texture]") {
    std::array<pt::Corner, 4> c{{
        {{0, 0, 10}, {0, 0, 1}, {0, 0}, {0, 0}},
        {{8, 0, 10}, {0, 0, 1}, {1, 0}, {1, 0}},
        {{8, 8, 10}, {0, 0, 1}, {1, 1}, {1, 1}},
        {{0, 8, 10}, {0, 0, 1}, {0, 1}, {0, 1}},
    }};
    // A slope rising one yard per yard along x, through the quad's centre.
    const auto slope = [](float x, float, float planeZ, float reach) -> std::optional<float> {
        const float h = 10.0f + (x - 4.0f);
        if (std::abs(h - planeZ) > reach) return std::nullopt;
        return h;
    };
    std::vector<pt::Vertex> out;
    const glm::vec3 origin(4, 4, 10);
    pt::buildGrid(c, origin, slope, out);
    REQUIRE(!out.empty());
    REQUIRE(out.size() % 6 == 0);
    for (const auto& v : out) {
        const float x = v.data[0] + origin.x;
        const float y = v.data[1] + origin.y;
        const float z = v.data[2] + origin.z;
        REQUIRE(z == Catch::Approx(10.0f + (x - 4.0f) + pt::kSurfaceLift).margin(1e-4));
        // Vertical projection: the texture coordinate is the quad's at that x, y.
        REQUIRE(v.data[6] == Catch::Approx(x / 8.0f).margin(1e-5));
        REQUIRE(v.data[7] == Catch::Approx(y / 8.0f).margin(1e-5));
    }
}

TEST_CASE("points with no surface in reach stay on the quad", "[projected_texture]") {
    std::array<pt::Corner, 4> c{{
        {{0, 0, 5}, {0, 0, 1}, {0, 0}, {0, 0}},
        {{2, 0, 5}, {0, 0, 1}, {1, 0}, {1, 0}},
        {{2, 2, 5}, {0, 0, 1}, {1, 1}, {1, 1}},
        {{0, 2, 5}, {0, 0, 1}, {0, 1}, {0, 1}},
    }};
    std::vector<pt::Vertex> out;
    pt::buildGrid(c, glm::vec3(0), [](float, float, float, float) { return std::optional<float>{}; }, out);
    for (const auto& v : out) REQUIRE(v.data[2] == Catch::Approx(5.0f));
}

TEST_CASE("nearestSurfaceZ follows uphill and downhill slopes and keeps caves under hills", "[projected_texture]") {
    // Shimmering Flats 26-deg slope: DynamicObject at z = -34.94, radius 10
    // (reach 20). Uphill impact at z = -30.5, downhill impact at z = -39.2.
    CHECK(pt::nearestSurfaceZ(-34.94f, 20.0f, -30.5f, std::nullopt) == Catch::Approx(-30.5f));
    CHECK(pt::nearestSurfaceZ(-34.94f, 20.0f, -39.2f, std::nullopt) == Catch::Approx(-39.2f));
    // Indoor cave floor at z = 10.5 under a terrain hill at z = 48: hill is
    // out of reach, and even a shallow hill at z = 22 loses to the cave floor.
    CHECK(pt::nearestSurfaceZ(10.0f, 16.0f, 48.0f, 10.5f) == Catch::Approx(10.5f));
    CHECK(pt::nearestSurfaceZ(10.0f, 16.0f, 22.0f, 10.5f) == Catch::Approx(10.5f));
    // Out-of-reach surfaces yield nullopt so callers fall back cleanly.
    CHECK_FALSE(pt::nearestSurfaceZ(0.0f, 16.0f, 40.0f, std::nullopt).has_value());
}
