// The primitives every collision query is built from.
//
// The doodad and building renderers each carried their own copy. A difference
// between them does not crash or log: it moves where the world is solid, and
// the report reads as "I fell through the floor in one building".
//
// The expectations below are computed independently - a brute-force scan of
// the box surface for the distance test, and a plane-intersection solved by
// hand for the ray test - rather than by calling the functions a second way.
#include "rendering/body_collision.hpp"
#include <catch_amalgamated.hpp>

#include <cmath>
#include <limits>

#include <glm/glm.hpp>

#include "rendering/collision_geometry.hpp"

using wowee::rendering::CollisionFocus;
using wowee::rendering::pointAABBDistanceSq;
using wowee::rendering::rayTriangleIntersect;

namespace {

/// Nearest distance from a point to a box, found by sampling the box surface.
/// Slow and obviously correct, which is what an oracle has to be.
///
/// Each face is swept with two independent parameters. A first version of this
/// drove two of the three coordinates from the same one, so it only ever
/// sampled a diagonal of each face and reported a point 36.99 away from a box
/// whose nearest surface is exactly 36. The function under test was right and
/// the oracle was not, which is the failure an oracle is supposed to make
/// visible rather than the one it is supposed to cause.
float bruteForceDistanceSq(const glm::vec3& p, const glm::vec3& bmin,
                           const glm::vec3& bmax) {
    float best = std::numeric_limits<float>::max();
    const int steps = 60;
    const glm::vec3 span = bmax - bmin;
    for (int i = 0; i <= steps; ++i) {
        for (int j = 0; j <= steps; ++j) {
            const float u = static_cast<float>(i) / steps;
            const float v = static_cast<float>(j) / steps;
            const glm::vec3 faces[6] = {
                {bmin.x, bmin.y + span.y * u, bmin.z + span.z * v},
                {bmax.x, bmin.y + span.y * u, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmin.y, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmax.y, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmin.y + span.y * v, bmin.z},
                {bmin.x + span.x * u, bmin.y + span.y * v, bmax.z},
            };
            for (const glm::vec3& q : faces) {
                const glm::vec3 d = p - q;
                best = std::min(best, glm::dot(d, d));
            }
        }
    }
    return best;
}

}  // namespace

TEST_CASE("distance to a box is zero inside it", "[collision]") {
    // Every caller relies on this: an instance the query point is inside must
    // never sort further away than one it is outside.
    const glm::vec3 bmin(-5.0f, -5.0f, -5.0f), bmax(5.0f, 5.0f, 5.0f);
    CHECK(pointAABBDistanceSq({0, 0, 0}, bmin, bmax) == Catch::Approx(0.0f));
    CHECK(pointAABBDistanceSq({4.9f, -4.9f, 0}, bmin, bmax) == Catch::Approx(0.0f));
    CHECK(pointAABBDistanceSq(bmax, bmin, bmax) == Catch::Approx(0.0f));
}

TEST_CASE("distance to a box matches a brute-force search", "[collision]") {
    const glm::vec3 bmin(-3.0f, 1.0f, -2.0f), bmax(4.0f, 6.0f, 9.0f);
    const glm::vec3 outside[] = {
        {10.0f, 3.0f, 0.0f},     // past one face
        {-8.0f, -4.0f, 0.0f},    // past an edge
        {12.0f, 20.0f, 30.0f},   // past a corner
        {0.0f, 0.0f, -6.0f},
    };
    for (const glm::vec3& p : outside) {
        INFO("point " << p.x << "," << p.y << "," << p.z);
        // The sampled surface is a lower bound on accuracy, so allow the grid.
        CHECK(pointAABBDistanceSq(p, bmin, bmax) ==
              Catch::Approx(bruteForceDistanceSq(p, bmin, bmax)).epsilon(0.02));
    }
}

TEST_CASE("a ray hits a triangle at the distance the plane says", "[collision]") {
    // Triangle in the z = 4 plane; a ray straight down the +z axis from the
    // origin must therefore report exactly 4.
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);
    const float t = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v1, v2);
    CHECK(t == Catch::Approx(4.0f));
}

TEST_CASE("a floor is solid from underneath", "[collision]") {
    // Two-sided on purpose. A version that culled backfaces would make every
    // floor one-way, and a character under a bridge or inside a building
    // would fall through the surface holding them up.
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);
    const float front = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v1, v2);
    // Same triangle, opposite winding: the same ray must still hit it.
    const float back = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v2, v1);
    CHECK(front == Catch::Approx(back));
    CHECK(back > 0.0f);
}

TEST_CASE("winding never changes the answer", "[collision]") {
    // Five call sites tested each triangle twice, the second time with the
    // vertices reversed, in case the first order missed. It cannot: the
    // barycentric bounds are symmetric in u and v, so reversing the winding
    // flips the sign of the determinant and swaps the two coordinates, and
    // both the hit test and the distance come out the same.
    //
    // Swept rather than argued: a deterministic spread of triangles and rays,
    // hits and misses alike.
    uint32_t seed = 12345;
    const auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1 << 24) * 20.0f - 10.0f;
    };
    int hits = 0;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 v0(next(), next(), next());
        const glm::vec3 v1(next(), next(), next());
        const glm::vec3 v2(next(), next(), next());
        const glm::vec3 origin(next(), next(), next());
        const glm::vec3 dir = glm::normalize(glm::vec3(next(), next(), next()) + glm::vec3(1e-3f));

        const float forward = rayTriangleIntersect(origin, dir, v0, v1, v2);
        const float reversed = rayTriangleIntersect(origin, dir, v0, v2, v1);
        if (forward > 0.0f) ++hits;
        // Both miss, or both hit at the same distance.
        REQUIRE((forward > 0.0f) == (reversed > 0.0f));
        if (forward > 0.0f) {
            REQUIRE(forward == Catch::Approx(reversed).epsilon(1e-4));
        }
    }
    // A sweep that never hits anything would prove nothing.
    CHECK(hits > 20);
}

TEST_CASE("a ray misses what it does not point at", "[collision]") {
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);

    SECTION("outside the triangle but on its plane") {
        CHECK(rayTriangleIntersect({10, 10, 0}, {0, 0, 1}, v0, v1, v2) < 0.0f);
    }
    SECTION("parallel to the plane") {
        CHECK(rayTriangleIntersect({0, 0, 0}, {1, 0, 0}, v0, v1, v2) < 0.0f);
    }
    SECTION("pointing away from it") {
        // Behind the origin is a miss, not a negative-distance hit: a floor
        // above your head is not one you are standing on.
        CHECK(rayTriangleIntersect({0, 0, 0}, {0, 0, -1}, v0, v1, v2) < 0.0f);
    }
    SECTION("just past the far edge") {
        // u + v > 1 - the diagonal, which is the edge a barycentric test gets
        // wrong first if its bound is written as u > 1 || v > 1.
        CHECK(rayTriangleIntersect({2.5f, 2.5f, 0}, {0, 0, 1}, v0, v1, v2) < 0.0f);
    }
}

TEST_CASE("an unset focus excludes nothing", "[collision]") {
    // Radius zero means no restriction. Reading it as an empty sphere would
    // make the whole world non-solid rather than merely slow, which is the
    // failure worth being explicit about.
    CollisionFocus focus;
    CHECK_FALSE(focus.excludes({-1, -1, -1}, {1, 1, 1}));
    CHECK_FALSE(focus.excludes({1000, 1000, 1000}, {1001, 1001, 1001}));

    SECTION("and setting it to zero radius clears it again") {
        focus.set({0, 0, 0}, 5.0f);
        CHECK(focus.enabled);
        focus.set({0, 0, 0}, 0.0f);
        CHECK_FALSE(focus.enabled);
        CHECK_FALSE(focus.excludes({1000, 1000, 1000}, {1001, 1001, 1001}));
    }
}

TEST_CASE("a focus measures to the box, not to its centre", "[collision]") {
    // A long building whose origin is far away is still under the player's
    // feet at one end. Measuring to the centre would drop it out of the query.
    CollisionFocus focus;
    focus.set({0, 0, 0}, 10.0f);

    const glm::vec3 longMin(-5.0f, -1.0f, -1.0f), longMax(500.0f, 1.0f, 1.0f);
    CHECK_FALSE(focus.excludes(longMin, longMax));

    SECTION("and something wholly beyond the radius is excluded") {
        CHECK(focus.excludes({100, 100, 100}, {101, 101, 101}));
    }
    SECTION("the boundary is the radius itself, inclusive") {
        // Exactly at the radius is kept: the test is a strict greater-than,
        // and an instance exactly at the edge is one the player can touch.
        CHECK_FALSE(focus.excludes({10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}));
        CHECK(focus.excludes({10.001f, 0.0f, 0.0f}, {10.001f, 0.0f, 0.0f}));
    }
}

TEST_CASE("horizontal wall collision blocks sustained movement and preserves sliding", "[collision]") {
    for (float scale : {0.25f, 1.0f, 4.0f}) {
        for (float side : {-1.0f, 1.0f}) {
            glm::vec2 position(side * 0.5f / scale, 0.0f);
            for (int step = 0; step < 120; ++step) {
                glm::vec2 proposed = position + glm::vec2(-side * 0.12f, 0.04f) / scale;
                proposed += wowee::rendering::horizontalWallSeparation(
                    {position.x, 0.0f}, {proposed.x, 0.0f}, {1.0f, 0.0f}, 0.5f / scale);
                REQUIRE(proposed.x * side * scale >= 0.4999f);
                REQUIRE(std::abs(proposed.y * scale - (step + 1) * 0.04f) < 0.0001f);
                position = proposed;
            }
        }
    }
}

TEST_CASE("horizontal wall collision retains entry side across thin surfaces", "[collision]") {
    for (float normal : {-1.0f, 1.0f}) {
        for (float destination : {-0.1f, 0.0f, 0.1f}) {
            const auto push = wowee::rendering::horizontalWallSeparation(
                {-0.2f, 0.0f}, {destination, 0.0f}, {normal, 0.0f}, 0.5f);
            REQUIRE(std::abs(destination + push.x + 0.5f) < 0.0001f);
            REQUIRE(push.y == 0.0f);
        }
    }
    const auto clear = wowee::rendering::horizontalWallSeparation(
        {-0.7f, 0.0f}, {-0.6f, 0.0f}, {1.0f, 0.0f}, 0.5f);
    REQUIRE(clear == glm::vec2(0.0f));
}

TEST_CASE("cylinder contact clips ramp edges below the step height", "[collision]") {
    using wowee::rendering::closestHorizontalPointInSlab;
    const glm::vec3 a(0,0,0), b(0,4,0), c(0,4,2);
    const auto low = closestHorizontalPointInSlab({0.1f,0.2f}, a,b,c,0.6f,2.0f);
    REQUIRE(low);
    CHECK(glm::distance(*low,glm::vec2(0.1f,0.2f)) > 0.5f);
    const auto tall = closestHorizontalPointInSlab({0.1f,3.0f}, a,b,c,0.6f,2.0f);
    REQUIRE(tall);
    CHECK(glm::distance(*tall,glm::vec2(0.1f,3.0f)) < 0.11f);
    CHECK_FALSE(closestHorizontalPointInSlab({0,0},a,b,c,2.1f,4.0f));
}


TEST_CASE("beveled body cannot tunnel through a thin wall") {
    using namespace wowee::rendering::movement;
    const std::array wall = {
        BodyTriangle{glm::vec3(0,-10,-5),glm::vec3(0,10,-5),glm::vec3(0,10,10)},
        BodyTriangle{glm::vec3(0,-10,-5),glm::vec3(0,10,10),glm::vec3(0,-10,10)}};
    for (float sign : {-1.0f, 1.0f}) {
        for (float travel : {3.0f, 10.0f, 100.0f}) {
            glm::vec3 result;
            REQUIRE(moveBody({sign*2,0,0},{sign*(2-travel),0,0},.5f,2.0f,wall,result));
            CHECK(std::abs(result.x-sign*.5014f)<.001f);
            CHECK(std::abs(result.z)<.001f);
        }
    }
}

TEST_CASE("flat character head stops on either winding of a ceiling") {
    using namespace wowee::rendering::movement;
    std::array ceiling = {
        BodyTriangle{glm::vec3(-5,-5,1.8f),glm::vec3(5,-5,1.8f),glm::vec3(5,5,1.8f)},
        BodyTriangle{glm::vec3(-5,-5,1.8f),glm::vec3(5,5,1.8f),glm::vec3(-5,5,1.8f)}};
    for (int winding=0;winding<2;++winding) {
        glm::vec3 result;
        REQUIRE(moveBody({0,0,0},{0,0,1},.30555f,1.607f,ceiling,result));
        CHECK(std::abs(result.z-(1.8f-1.607f-.0014f))<.001f);
        CHECK(std::abs(result.x)<.001f);
        CHECK(std::abs(result.y)<.001f);
        for (auto& triangle:ceiling) std::swap(triangle[0],triangle[1]);
    }
}

TEST_CASE("overhang contact cannot push walking feet downward") {
    using namespace wowee::rendering::movement;
    const std::array overhang = {
        BodyTriangle{glm::vec3(1,-3,0),glm::vec3(-1,-3,3),glm::vec3(-1,3,3)},
        BodyTriangle{glm::vec3(1,-3,0),glm::vec3(-1,3,3),glm::vec3(1,3,0)}};
    glm::vec3 position(-2,0,0);
    for (int frame=0;frame<100;++frame) {
        glm::vec3 result;
        moveBody(position,position+glm::vec3(.1f,0,0),.5f,2.0f,overhang,result);
        position=result;
        CHECK(std::abs(position.z)<.001f);
    }
    // Plane x+2z/3=1 intersects the body's leading top corner.
    CHECK(std::abs(position.x+5.0f/6.0f)<.003f);
}

TEST_CASE("airborne lateral contact cannot gain grounded step height") {
    using namespace wowee::rendering::movement;
    const std::array slope = {
        BodyTriangle{glm::vec3(0,-4,0),glm::vec3(2,-4,3),glm::vec3(2,4,3)},
        BodyTriangle{glm::vec3(0,-4,0),glm::vec3(2,4,3),glm::vec3(0,4,0)}};
    glm::vec3 walking(-1,0,.2f), airborne = walking;
    bool hit = false;
    for (int frame = 0; frame < 30; ++frame) {
        glm::vec3 result;
        moveBody(walking,walking+glm::vec3(.1f,0,0),.3f,2.0f,slope,result,true);
        walking = result;
        hit |= moveBody(airborne,airborne+glm::vec3(.1f,0,0),.3f,2.0f,slope,result,false);
        airborne = result;
        CHECK(airborne.z == Catch::Approx(.2f));
    }
    REQUIRE(hit);
    CHECK(walking.z > airborne.z + .5f);
    CHECK(airborne.x < walking.x);
}

TEST_CASE("offset floor support agrees with the swept lower body") {
    using namespace wowee::rendering::movement;
    for (float radius : {0.15f, 0.305556f, 0.5f, 1.0f}) {
        for (const auto& offset : bodySupportPoints(radius)) {
            CHECK(std::abs(offset.x) <= radius);
            CHECK(std::abs(offset.y) <= radius);
            constexpr float floorZ = 0.7f;
            // Keep triangle area above the collision solver's degeneracy gate.
            constexpr float halfSize = 0.002f;
            const glm::vec3 a(offset.x-halfSize, offset.y-halfSize, floorZ);
            const glm::vec3 b(offset.x+halfSize, offset.y-halfSize, floorZ);
            const glm::vec3 c(offset.x+halfSize, offset.y+halfSize, floorZ);
            const glm::vec3 d(offset.x-halfSize, offset.y+halfSize, floorZ);
            const std::array surface = {BodyTriangle{a,b,c}, BodyTriangle{a,c,d}};
            std::optional<float> first;
            for (const auto& triangle : surface) {
                const auto contact = sweepBodyTriangle({0,0,3}, {0,0,-6}, radius,
                    radius*kBodyBevel + 1.0f, triangle);
                if (contact && (!first || contact->time < *first)) first = contact->time;
            }
            REQUIRE(first);
            // Independent continuous collision against a tiny horizontal prop
            // must place the sole at the sampled lower-envelope height.
            CHECK(3.0f - 6.0f * *first ==
                Catch::Approx(floorZ - offset.z).margin(halfSize * kBodyBevel + 0.0001f));
        }
    }
}
