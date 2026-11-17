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
