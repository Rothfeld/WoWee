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
#include "rendering/movement_limits.hpp"
#include "rendering/placement_transform.hpp"
#include "core/coordinates.hpp"
#include "pipeline/wmo_loader.hpp"
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

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

TEST_CASE("the camera passes only WMO triangles flagged 0x02", "[collision][camera]") {
    // Flag values the build 8606 camera was measured against in Black
    // Temple (see wmoTriangleStopsCamera). Movement blocks 0x26 (rendered,
    // not detail) and passes 0x64 (detail); the camera does the opposite.
    SECTION("passed through") {
        for (uint8_t mopy : {0x26, 0x66, 0x4a}) {
            INFO("mopy " << int(mopy));
            CHECK_FALSE(wowee::rendering::wmoTriangleStopsCamera(mopy));
        }
    }
    SECTION("stopped by") {
        for (uint8_t mopy : {0x60, 0x64, 0x48, 0x08, 0x20, 0x24}) {
            INFO("mopy " << int(mopy));
            CHECK(wowee::rendering::wmoTriangleStopsCamera(mopy));
        }
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

TEST_CASE("a body falling into a crease rests on both faces") {
    using namespace wowee::rendering::movement;
    // Placed collision of redridgedocks03 at Alcaz Island (uid 381622): the
    // fold between the deck's steep riser and the face in front of the beam.
    // Native 2.4.3 stood in this fold at (-2697.70, -4694.83, 9.43). The skin
    // push off one face used to carry the body into the other, which was then
    // ignored as an existing overlap: the body fell through into the deck.
    const std::array crease = {
        BodyTriangle{glm::vec3(-2712.85693f,-4694.49658f,9.70793915f),
                     glm::vec3(-2693.99902f,-4694.67871f,9.72304153f),
                     glm::vec3(-2712.64478f,-4691.78418f,10.4850664f)},
        BodyTriangle{glm::vec3(-2691.69751f,-4694.95459f,9.32808304f),
                     glm::vec3(-2712.85693f,-4694.49658f,9.70793915f),
                     glm::vec3(-2715.01074f,-4694.64746f,9.30869675f)}};
    glm::vec3 body(-2697.726074f,-4694.811035f,9.414773f);
    for (int frame = 0; frame < 20; ++frame) {
        glm::vec3 result;
        moveBody(body, body + glm::vec3(0,0,-0.2f), .306f, 2.031f, crease, result);
        body = result;
        CHECK(body.z > 9.41f);
        CHECK(body.y < -4694.80f);
    }
}

TEST_CASE("a body overlapping a face deeper than the skin still leaves it") {
    using namespace wowee::rendering::movement;
    const std::array floor = {
        BodyTriangle{glm::vec3(-5,-5,0),glm::vec3(5,-5,0),glm::vec3(5,5,0)},
        BodyTriangle{glm::vec3(-5,-5,0),glm::vec3(5,5,0),glm::vec3(-5,5,0)}};
    glm::vec3 result;
    // Sunk a hand's width into a floor: free to rise out and to walk.
    CHECK_FALSE(moveBody({0,0,-.1f},{0,0,.5f},.3f,2.0f,floor,result));
    CHECK(result.z == Catch::Approx(.5f));
    CHECK_FALSE(moveBody({0,0,-.1f},{1,0,-.1f},.3f,2.0f,floor,result));
    // Within the skin, moving deeper is contact; moving out is not.
    CHECK(moveBody({0,0,-.001f},{0,0,-.5f},.3f,2.0f,floor,result));
    CHECK(result.z > -.002f);
    CHECK_FALSE(moveBody({0,0,-.001f},{0,0,.5f},.3f,2.0f,floor,result));
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

TEST_CASE("ground snap cannot erase beveled side contact") {
    using namespace wowee::rendering::movement;
    const std::array side = {
        BodyTriangle{glm::vec3(0,-1,0),glm::vec3(0,1,0),glm::vec3(0,1,.25f)},
        BodyTriangle{glm::vec3(0,-1,0),glm::vec3(0,1,.25f),glm::vec3(0,-1,.25f)}};
    std::optional<BodyContact> first;
    for (const auto& triangle : side) {
        const auto contact = sweepBodyTriangle({-.1f,0,.1f}, {0,0,-.1f},
                                               .305556f, 1.607f, triangle);
        if (contact && (!first || contact->time < first->time)) first = contact;
    }
    REQUIRE(first);
    CHECK(.1f - first->time * .1f == Catch::Approx(.25f - .1f * kBodyBevel));
    CHECK(first->normal.z > 0.0f);
    // The side catches the lower bevel, but does not provide a walkable floor.
    CHECK(first->normal.z < .642787635f);
}

TEST_CASE("grounded step rests on the steep authored face it walks into") {
    using namespace wowee::rendering::movement;
    // One side of DR_TABLE_MEDIUM_01's collision pedestal: a face rising
    // 1.45 yards while leaning 0.416 yards inward, normal (-0.961, 0, 0.276).
    const float lean = 1.45f * 0.276f / 0.961f;
    const std::vector<BodyTriangle> side = {
        BodyTriangle{glm::vec3(1,-2,0),glm::vec3(1+lean,2,1.45f),glm::vec3(1,2,0)},
        BodyTriangle{glm::vec3(1,-2,0),glm::vec3(1+lean,-2,1.45f),glm::vec3(1+lean,2,1.45f)}};
    const float radius = .305556f, height = 2.03127f;
    // The body's full-width bevel corners meet the face first.
    const float faceAtBevel = 1 + lean * radius * kBodyBevel / 1.45f;
    const glm::vec3 from(faceAtBevel - radius - .01f, 0, 0), to(from.x + .12f, 0, 0);

    glm::vec3 ordinary;
    REQUIRE(moveBody(from, to, radius, height, side, ordinary, false));
    const glm::vec3 stepped = stepBodyAcross(from, to, radius, height, side, kMaxStepUp);
    CHECK(stepped.x == Catch::Approx(to.x).margin(.0001f));
    CHECK(stepped.x > ordinary.x);
    // The body ends where it rests on the face: lowering it any further
    // must meet the face immediately, with an unwalkable contact.
    REQUIRE(stepped.z > .2f);
    std::optional<BodyContact> below;
    for (const auto& triangle : side) {
        const auto contact = sweepBodyTriangle(stepped + glm::vec3(0,0,.01f), {0,0,-.1f},
                                               radius, height, triangle);
        if (contact && (!below || contact->time < below->time)) below = contact;
    }
    REQUIRE(below);
    CHECK(below->time * .1f == Catch::Approx(.01f).margin(.0002f));
    CHECK(!isWalkableNormal(below->normal.z));
}

TEST_CASE("grounded step over a low authored edge returns to the sole height") {
    using namespace wowee::rendering::movement;
    // A 0.2-yard block one yard deep: the step crosses its top and, with no
    // face beneath the far side, ends back at the original sole height.
    std::vector<BodyTriangle> block;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        block.push_back({a,b,c});
        block.push_back({a,c,d});
    };
    quad({0,-2,0},{0,2,0},{0,2,.2f},{0,-2,.2f});
    quad({0,-2,.2f},{0,2,.2f},{.1f,2,.2f},{.1f,-2,.2f});
    quad({.1f,-2,.2f},{.1f,2,.2f},{.1f,2,0},{.1f,-2,0});
    const float radius = .305556f, height = 2.03127f;
    const glm::vec3 from(-radius - .01f, 0, 0), onTop(.05f, 0, 0), beyond(.5f, 0, 0);
    CHECK(stepBodyAcross(from, onTop, radius, height, block, kMaxStepUp).z ==
          Catch::Approx(.2f).margin(.002f));
    CHECK(stepBodyAcross(from, beyond, radius, height, block, kMaxStepUp).z ==
          Catch::Approx(0.f).margin(.0001f));
}

namespace {

std::filesystem::path findTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p / "world/wmo/dungeon/qt_sunwell/sunwell.wmo")) return p;
    }
    const std::filesystem::path fallback("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    if (std::filesystem::exists(fallback / "world/wmo/dungeon/qt_sunwell/sunwell.wmo")) return fallback;
    return {};
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::optional<float> raycastFloorZ(std::span<const wowee::rendering::movement::BodyTriangle> tris,
                                   float x, float y, float maxZ, float* outNormalZ = nullptr) {
    std::optional<float> bestZ;
    float bestNz = 1.0f;
    const glm::vec3 origin(x, y, maxZ + 200.0f);
    const glm::vec3 dir(0.0f, 0.0f, -1.0f);
    for (const auto& tri : tris) {
        const float t = rayTriangleIntersect(origin, dir, tri[0], tri[1], tri[2]);
        if (t <= 0.0f) continue;
        const float hitZ = origin.z - t;
        if (hitZ > maxZ) continue;
        glm::vec3 n = glm::cross(tri[1] - tri[0], tri[2] - tri[0]);
        const float nLen = glm::length(n);
        if (nLen <= 1e-6f) continue;
        n /= nLen;
        if (n.z < 0.0f) n = -n;
        if (!bestZ || hitZ > *bestZ) {
            bestZ = hitZ;
            bestNz = n.z;
        }
    }
    if (bestZ && outNormalZ) *outNormalZ = bestNz;
    return bestZ;
}

struct SimulatedWalker {
    glm::vec3 pos;
    float radius;
    float height;
    bool grounded = true;
    bool stepFromRest = false;
    float verticalVelocity = 0.0f;
    float walkableBaseZ = 0.0f;

    void advanceFrame(const glm::vec2& horizDir, float speed, float dt,
                      std::span<const wowee::rendering::movement::BodyTriangle> allTris) {
        using namespace wowee::rendering::movement;
        constexpr float kGravity = -19.29f;
        const int physSteps = std::max(1, static_cast<int>(std::ceil(dt / (1.0f / 30.0f))));
        const float pdt = dt / static_cast<float>(physSteps);
        const glm::vec2 unitDir = glm::length(horizDir) > 1e-6f ? glm::normalize(horizDir) : glm::vec2(0.0f);

        std::vector<BodyTriangle> localTris;
        for (int ps = 0; ps < physSteps; ++ps) {
            bool stepFinishedThisFrame = false;
            const bool canRetryRest = stepFromRest && !grounded && verticalVelocity <= 0.0f;
            stepFromRest = false;

            const glm::vec3 prevPos = pos;
            glm::vec3 target = pos;
            target.x += unitDir.x * speed * pdt;
            target.y += unitDir.y * speed * pdt;
            target.z -= 0.5f * kGravity * pdt * pdt;
            verticalVelocity += kGravity * pdt;
            target.z += verticalVelocity * pdt;

            const glm::vec3 fullDelta = target - pos;
            const float dist = glm::length(fullDelta);
            const int substeps = std::clamp(static_cast<int>(std::ceil(dist / 0.20f)), 1, 32);
            const glm::vec3 subDelta = fullDelta / static_cast<float>(substeps);

            if (grounded && pos.z < walkableBaseZ) walkableBaseZ = pos.z;
            bool subCanStepFromRest = canRetryRest;
            glm::vec3 stepPos = pos;

            // Gather triangles near this physics step once for fast testing.
            const glm::vec3 qLow = glm::min(pos, target) - glm::vec3(radius + 1.5f, radius + 1.5f, 2.5f);
            const glm::vec3 qHigh = glm::max(pos, target) + glm::vec3(radius + 1.5f, radius + 1.5f, height + kMaxStepUp + 1.5f);
            localTris.clear();
            for (const auto& tri : allTris) {
                const glm::vec3 tMin = glm::min(tri[0], glm::min(tri[1], tri[2]));
                const glm::vec3 tMax = glm::max(tri[0], glm::max(tri[1], tri[2]));
                if (glm::any(glm::lessThan(tMax, qLow)) || glm::any(glm::greaterThan(tMin, qHigh))) continue;
                localTris.push_back(tri);
            }

            for (int i = 0; i < substeps; ++i) {
                glm::vec3 candidate = stepPos + subDelta;
                const glm::vec3 horizontal(candidate.x, candidate.y, stepPos.z);
                glm::vec3 horizAdj = horizontal;
                const bool horizContact = moveBody(stepPos, horizontal, radius, height,
                                                   localTris, horizAdj, false);
                bool didStepUp = false;
                if (horizContact && (grounded || subCanStepFromRest)) {
                    const bool fromRest = !grounded;
                    const float budget = std::max(0.0f, (walkableBaseZ + kMaxStepUp) - stepPos.z);
                    if (budget > 1e-4f) {
                        glm::vec3 landingFace(0.0f, 0.0f, 1.0f);
                        glm::vec3 landingNormal(0.0f, 0.0f, 1.0f);
                        const glm::vec3 stepped = stepBodyAcross(
                            stepPos, horizontal, radius, height, localTris, budget,
                            &landingFace, &landingNormal);
                        const glm::vec2 intent(horizontal - stepPos);
                        bool farther = glm::dot(glm::vec2(stepped - stepPos), intent) >
                            glm::dot(glm::vec2(horizAdj - stepPos), intent) + 1e-6f;
                        if (fromRest && !isWalkableNormal(landingFace.z)) farther = false;
                        if (farther) {
                            horizAdj = stepped;
                            didStepUp = stepped.z > stepPos.z + 1e-5f;
                            if (didStepUp) {
                                stepFinishedThisFrame = true;
                                if (isWalkableNormal(landingFace.z)) {
                                    grounded = true;
                                    verticalVelocity = 0.0f;
                                    subCanStepFromRest = false;
                                    if (isWalkableNormal(landingNormal.z)) {
                                        walkableBaseZ = stepped.z;
                                    }
                                } else {
                                    grounded = false;
                                    subCanStepFromRest = true;
                                }
                            }
                        }
                    }
                }
                if (didStepUp && subDelta.z <= 0.0f) {
                    candidate = horizAdj;
                } else {
                    candidate = horizAdj + glm::vec3(0.0f, 0.0f, subDelta.z);
                    glm::vec3 vertAdj = candidate;
                    if (moveBody(horizAdj, candidate, radius, height, localTris, vertAdj, true)) {
                        candidate = vertAdj;
                    }
                }
                stepPos = candidate;
            }

            // Grounding pass matching CameraController::groundFollowedCharacter
            const float stepBudget = grounded
                ? (stepPos.z > walkableBaseZ
                       ? std::max(0.05f, (walkableBaseZ + kMaxStepUp) - stepPos.z)
                       : kMaxStepUp)
                : 1.2f;
            float floorNz = 1.0f;
            std::optional<float> groundH = raycastFloorZ(localTris, stepPos.x, stepPos.y,
                                                         stepPos.z + stepBudget, &floorNz);
            if (groundH && !isWalkableNormal(floorNz)) groundH = std::nullopt;

            std::optional<float> steepSupportZ;
            bool bevelOnWalkable = false;
            if ((grounded || stepFinishedThisFrame) && verticalVelocity <= 0.0f &&
                (!groundH || *groundH < stepPos.z - 1e-4f)) {
                const float minSweepZ = stepPos.z - std::max(1.5f, radius * kBodyBevel + 0.5f);
                const float reqZ = groundH ? std::max(*groundH, minSweepZ) : minSweepZ;
                if (reqZ < stepPos.z - 1e-4f) {
                    float supZ = reqZ, supNormZ = 1.0f, supFaceZ = 1.0f;
                    if (constrainBodyDescent(stepPos, reqZ, radius, height, localTris, supZ, supNormZ, &supFaceZ)) {
                        if (isWalkableNormal(supFaceZ) && supZ <= walkableBaseZ + kMaxStepUp + 1e-3f) {
                            groundH = supZ;
                            bevelOnWalkable = !isWalkableNormal(supNormZ);
                        } else {
                            steepSupportZ = supZ;
                        }
                    }
                }
            }

            if (groundH) {
                const float dz = *groundH - stepPos.z;
                const bool nearGround = grounded && dz >= 0.0f && dz <= stepBudget + 0.05f;
                const bool slopeGrace = grounded && dz >= -(stepBudget + 0.05f) && dz <= (stepBudget + 0.05f) * 1.5f;
                const bool airLanding = !grounded && verticalVelocity <= 0.0f && dz >= 0.0f && prevPos.z >= *groundH;
                if (nearGround || slopeGrace || airLanding) {
                    float snapH = *groundH;
                    bool steep = false;
                    bool soleOnWalkable = !bevelOnWalkable;
                    if (steepSupportZ && *steepSupportZ > snapH) {
                        snapH = *steepSupportZ;
                        steep = true;
                        soleOnWalkable = false;
                    } else if (!steepSupportZ && !bevelOnWalkable && snapH < stepPos.z) {
                        float supZ = snapH, supNormZ = 1.0f, supFaceZ = 1.0f;
                        if (constrainBodyDescent(stepPos, snapH, radius, height, localTris, supZ, supNormZ, &supFaceZ)) {
                            snapH = supZ;
                            if (isWalkableNormal(supFaceZ) && supZ <= walkableBaseZ + kMaxStepUp + 1e-3f) {
                                steep = false;
                                soleOnWalkable = isWalkableNormal(supNormZ);
                            } else {
                                steep = true;
                                soleOnWalkable = false;
                            }
                        }
                    }
                    stepPos.z = snapH;
                    grounded = !steep;
                    if (grounded) {
                        verticalVelocity = 0.0f;
                        if (soleOnWalkable || snapH < walkableBaseZ) walkableBaseZ = snapH;
                    } else if (stepFinishedThisFrame) {
                        stepFromRest = true;
                    }
                } else {
                    if (steepSupportZ) stepPos.z = std::max(stepPos.z, *steepSupportZ);
                    grounded = false;
                    if (stepFinishedThisFrame) stepFromRest = true;
                }
            } else {
                if (steepSupportZ) stepPos.z = std::max(stepPos.z, *steepSupportZ);
                grounded = false;
                if (stepFinishedThisFrame) stepFromRest = true;
            }
            pos = stepPos;
        }
    }
};

} // namespace

TEST_CASE("WMO movement triangle and antiportal filters match TBC 2.4.3 rules", "[collision][wmo]") {
    using wowee::rendering::wmoGroupIsAntiportal;
    using wowee::rendering::wmoTriangleBlocksMovement;

    // Explicit collision hulls (0x08), rendered non-detail (0x20 without 0x04), and unflagged (0x00) block.
    for (uint8_t mopy : {0x00, 0x08, 0x48, 0x4a, 0x20, 0x60, 0x22, 0x62, 0x28, 0x6c}) {
        INFO("mopy 0x" << std::hex << int(mopy));
        CHECK(wmoTriangleBlocksMovement(mopy));
    }
    // Detail-only / camera-passthrough faces without 0x08 never block player movement.
    for (uint8_t mopy : {0x04, 0x24, 0x64, 0x26, 0x66, 0x02}) {
        INFO("mopy 0x" << std::hex << int(mopy));
        CHECK_FALSE(wmoTriangleBlocksMovement(mopy));
    }

    // MOGP 0x4000000 and Sunwell's sunwell_051 ("antiportal" with flags 0x9) are both antiportals.
    CHECK(wmoGroupIsAntiportal(0x4000000u, ""));
    CHECK(wmoGroupIsAntiportal(0x4000009u, "plaza"));
    CHECK(wmoGroupIsAntiportal(0x9u, "antiportal"));
    CHECK(wmoGroupIsAntiportal(0x9u, "AntiPortal"));
    CHECK_FALSE(wmoGroupIsAntiportal(0x9u, "plaza"));
    CHECK_FALSE(wmoGroupIsAntiportal(0x2001u, "wallsplit"));
    CHECK_FALSE(wmoGroupIsAntiportal(0x9u, "StairsDetail"));
}

TEST_CASE("stair edge tie-breaking and constrainBodyDescent preserve walkable tread normals", "[collision][stairs]") {
    using namespace wowee::rendering::movement;
    const float r = 0.305556f, h = 2.03127f;

    // Vertical riser (x = 0, z in [0, 0.85]) sharing its top edge with horizontal tread (x in [0, 2], z = 0.85).
    const BodyTriangle riserA{glm::vec3(0, -2, 0), glm::vec3(0, 2, 0), glm::vec3(0, 2, 0.85f)};
    const BodyTriangle riserB{glm::vec3(0, -2, 0), glm::vec3(0, 2, 0.85f), glm::vec3(0, -2, 0.85f)};
    const BodyTriangle treadA{glm::vec3(0, -2, 0.85f), glm::vec3(2, -2, 0.85f), glm::vec3(2, 2, 0.85f)};
    const BodyTriangle treadB{glm::vec3(0, -2, 0.85f), glm::vec3(2, 2, 0.85f), glm::vec3(0, 2, 0.85f)};

    const std::vector<BodyTriangle> riserFirst{riserA, riserB, treadA, treadB};
    const std::vector<BodyTriangle> treadFirst{treadA, treadB, riserA, riserB};

    for (const auto& order : {riserFirst, treadFirst}) {
        glm::vec3 landingFace(0.0f);
        const glm::vec3 stepped = stepBodyAcross({-0.31f, 0.0f, 0.0f}, {-0.15f, 0.0f, 0.0f},
                                                 r, h, order, kMaxStepUp, &landingFace);
        const float expectedZ = 0.85f - 0.15f * kBodyBevel;
        CHECK(stepped.z == Catch::Approx(expectedZ).margin(1e-3f));
        CHECK(landingFace.z == Catch::Approx(1.0f).margin(1e-4f));

        // Even when slightly sunk by crease skin contact, constrainBodyDescent holds the bevel height.
        float supZ = 0.0f, supNormZ = 0.0f, supFaceZ = 0.0f;
        REQUIRE(constrainBodyDescent({-0.15f, 0.0f, expectedZ - 0.002f}, 0.0f,
                                     r, h, order, supZ, supNormZ, &supFaceZ));
        CHECK(supZ == Catch::Approx(expectedZ - 0.002f).margin(1e-3f));
        CHECK(supNormZ < kMinWalkableNormalZ);
        CHECK(supFaceZ == Catch::Approx(1.0f).margin(1e-4f));
    }

    // Retaining wall ending at requestedZ (z = 0) does not falsely block a snap to z = 0.
    const std::vector<BodyTriangle> cliffWall{
        BodyTriangle{glm::vec3(0, -2, -5.0f), glm::vec3(0, 2, -5.0f), glm::vec3(0, 2, 0.0f)},
        BodyTriangle{glm::vec3(0, -2, -5.0f), glm::vec3(0, 2, 0.0f), glm::vec3(0, -2, 0.0f)}};
    float dummyZ = 0.0f, dummyFaceZ = 0.0f;
    CHECK_FALSE(constrainBodyDescent({-0.05f, 0.0f, 0.02f}, 0.0f, r, h, cliffWall, dummyZ, dummyFaceZ));
}

TEST_CASE("synthetic stairs climb smoothly while >1.0 yd walls and steep pedestals block", "[collision][stairs]") {
    using namespace wowee::rendering::movement;
    REQUIRE(kMaxStepUp == Catch::Approx(1.0f));

    // Build a 4-step staircase (0.85 yd risers, 0.65 yd shallow treads) over an overlapping lower floor z=0,
    // followed by a 1.15 yd blocking wall at x = 6.0.
    std::vector<BodyTriangle> mesh;
    auto addQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        mesh.push_back({a, b, c});
        mesh.push_back({a, c, d});
    };
    // Overlapping lower slab under the entire staircase (reproducing Sunwell's lower slab geometry)
    addQuad({-4, -14, 0}, {8, -14, 0}, {8, 14, 0}, {-4, 14, 0});
    const float risers[4] = {0.85f, 1.70f, 2.55f, 3.40f};
    const float xs[5] = {0.0f, 0.65f, 1.30f, 1.95f, 5.50f};
    float prevZ = 0.0f;
    for (int s = 0; s < 4; ++s) {
        addQuad({xs[s], -13, prevZ}, {xs[s], 13, prevZ}, {xs[s], 13, risers[s]}, {xs[s], -13, risers[s]});
        addQuad({xs[s], -13, risers[s]}, {xs[s + 1], -13, risers[s]}, {xs[s + 1], 13, risers[s]}, {xs[s], 13, risers[s]});
        prevZ = risers[s];
    }
    // Full-height room wall on the top landing at x = 5.50 (3.40 -> 8.00)
    addQuad({5.50f, -13, 3.40f}, {5.50f, 13, 3.40f}, {5.50f, 13, 8.00f}, {5.50f, -13, 8.00f});

    struct BodySpec { const char* name; float r; float h; };
    const BodySpec bodies[] = {
        {"Human", 0.305556f, 2.03127f},
        {"Tauren", 0.972222f, 2.23244f},
        {"CatForm", 0.611111f, 1.22222f},
    };
    for (const auto& body : bodies) {
        for (float speed : {7.0f, 35.0f}) {
            for (float fps : {20.0f, 60.0f, 144.0f}) {
                for (glm::vec2 dir : {glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 0.65f)}) {
                    INFO(body.name << " speed=" << speed << " fps=" << fps << " dirY=" << dir.y);
                    SimulatedWalker w{{-2.0f, -1.5f, 0.0f}, body.r, body.h, true, false, 0.0f, 0.0f};
                    const float totalSec = (speed > 20.0f) ? 0.45f : 1.6f;
                    const int frames = static_cast<int>(std::ceil(totalSec * fps));
                    for (int f = 0; f < frames; ++f) {
                        w.advanceFrame(dir, speed, 1.0f / fps, mesh);
                    }
                    // Must climb all four 0.85 yd steps onto z = 3.40, and stop cleanly in front of the wall at x = 5.50!
                    CHECK(w.pos.z == Catch::Approx(3.40f).margin(0.02f));
                    CHECK(w.pos.x > 2.5f);
                    CHECK(w.pos.x <= 5.50f - body.r + 0.02f);
                    CHECK(w.grounded);
                }
            }
        }
    }

    // A 1.15 yd waist-high ledge (just 0.15 yd over kMaxStepUp = 1.0f) must NEVER be climbed onto z = 1.15,
    // even by Tauren (bevel = 1.80 yd) across 60 frames at 1x and 5x!
    std::vector<BodyTriangle> ledgeMesh;
    auto addLedgeQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        ledgeMesh.push_back({a, b, c});
        ledgeMesh.push_back({a, c, d});
    };
    addLedgeQuad({-4, -4, 0}, {6, -4, 0}, {6, 4, 0}, {-4, 4, 0});
    addLedgeQuad({1.0f, -4, 0.0f}, {1.0f, 4, 0.0f}, {1.0f, 4, 1.15f}, {1.0f, -4, 1.15f});
    addLedgeQuad({1.0f, -4, 1.15f}, {4.0f, -4, 1.15f}, {4.0f, 4, 1.15f}, {1.0f, 4, 1.15f});
    for (const auto& body : bodies) {
        for (float speed : {7.0f, 35.0f}) {
            SimulatedWalker w{{-1.5f, 0.0f, 0.0f}, body.r, body.h, true, false, 0.0f, 0.0f};
            for (int f = 0; f < 60; ++f) {
                const glm::vec3 prev = w.pos;
                const float prevBase = w.walkableBaseZ;
                const bool prevGrd = w.grounded;
                w.advanceFrame({1.0f, 0.0f}, speed, 1.0f / 60.0f, ledgeMesh);
                INFO(body.name << " speed=" << speed << " f=" << f
                     << " prev=(" << prev.x << "," << prev.z << ") base=" << prevBase << " grd=" << prevGrd
                     << " -> pos=(" << w.pos.x << "," << w.pos.z << ") newBase=" << w.walkableBaseZ << " newGrd=" << w.grounded);
                REQUIRE(w.pos.z <= 1.005f);
            }
            CHECK(w.pos.x < 1.0f);
        }
    }

    // Exodar DR_TABLE_MEDIUM_01 (1.45 yd tall, 74-deg sloped pedestal + flat z=1.45 top) must NOT be climbable!
    std::vector<BodyTriangle> tableMesh;
    auto addTableQuad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        tableMesh.push_back({a, b, c});
        tableMesh.push_back({a, c, d});
    };
    addTableQuad({-3, -3, 0}, {4, -3, 0}, {4, 3, 0}, {-3, 3, 0});
    const float lean = 1.45f * 0.276f / 0.961f;
    addTableQuad({1.0f, -2.0f, 0.0f}, {1.0f + lean, -2.0f, 1.45f}, {1.0f + lean, 2.0f, 1.45f}, {1.0f, 2.0f, 0.0f});
    addTableQuad({1.0f + lean, -2.0f, 1.45f}, {3.0f, -2.0f, 1.45f}, {3.0f, 2.0f, 1.45f}, {1.0f + lean, 2.0f, 1.45f});

    for (float speed : {7.0f, 35.0f}) {
        for (float fps : {20.0f, 60.0f}) {
            SimulatedWalker w{{0.0f, 0.0f, 0.0f}, 0.305556f, 2.03127f, true, false, 0.0f, 0.0f};
            for (int f = 0; f < 40; ++f) {
                const glm::vec3 prev = w.pos;
                const float prevBase = w.walkableBaseZ;
                const bool prevGrd = w.grounded;
                w.advanceFrame({1.0f, 0.0f}, speed, 1.0f / fps, tableMesh);
                INFO("table speed=" << speed << " fps=" << fps << " f=" << f
                     << " prev=(" << prev.x << "," << prev.z << ") base=" << prevBase << " grd=" << prevGrd
                     << " -> pos=(" << w.pos.x << "," << w.pos.z << ") newBase=" << w.walkableBaseZ << " newGrd=" << w.grounded);
                REQUIRE(w.pos.z <= 1.005f);
            }
            CHECK(w.pos.x < 1.0f + lean);
        }
    }
}

TEST_CASE("shipped Sunwell Plateau Parhelion Plaza stairs climb at 1x and 5x while walls block",
          "[collision][wmo][sunwell]") {
    using namespace wowee::rendering;
    using namespace wowee::rendering::movement;

    const auto tbcDir = findTbcDataDir();
    if (tbcDir.empty()) {
        SKIP("TBC extracted data directory not available");
    }

    const auto wmoDir = tbcDir / "world/wmo/dungeon/qt_sunwell";
    auto model = wowee::pipeline::WMOLoader::load(readBinaryFile(wmoDir / "sunwell.wmo"));
    REQUIRE(model.nGroups >= 52);
    REQUIRE(wowee::pipeline::WMOLoader::loadGroup(readBinaryFile(wmoDir / "sunwell_001.wmo"), model, 1));
    REQUIRE(wowee::pipeline::WMOLoader::loadGroup(readBinaryFile(wmoDir / "sunwell_039.wmo"), model, 39));
    REQUIRE(wowee::pipeline::WMOLoader::loadGroup(readBinaryFile(wmoDir / "sunwell_051.wmo"), model, 51));

    // Verify group 51 ("antiportal", flags 0x9) is detected as an antiportal and skipped.
    CHECK(wmoGroupIsAntiportal(model.groups[51].flags, model.groups[51].name));
    CHECK_FALSE(wmoGroupIsAntiportal(model.groups[1].flags, model.groups[1].name));
    CHECK_FALSE(wmoGroupIsAntiportal(model.groups[39].flags, model.groups[39].name));

    // MODF uid 1993110 in sunwellplateau_30_28.adt: pos=(16078.585938, 40.581085, 15425.220703), rot=(0, 269, 0).
    constexpr float kDeg = wowee::core::coords::PI / 180.0f;
    const glm::mat4 worldMat = placementModelMatrix(
        wowee::core::coords::adtToWorld(16078.585938f, 40.581085f, 15425.220703f),
        glm::vec3(0.0f, 0.0f, (269.0f + 180.0f) * kDeg),
        1.0f);

    std::vector<BodyTriangle> sunwellTris;
    size_t group39Blocking = 0;
    for (uint32_t gi : {1u, 39u, 51u}) {
        const auto& g = model.groups[gi];
        if ((g.flags & 0x80u) != 0 || wmoGroupIsAntiportal(g.flags, g.name)) continue;
        const size_t numTris = g.indices.size() / 3;
        for (size_t t = 0; t < numTris; ++t) {
            const uint8_t mopy = t < g.triFlags.size() ? g.triFlags[t] : 0;
            if (!wmoTriangleBlocksMovement(mopy)) continue;
            if (gi == 39u) ++group39Blocking;
            sunwellTris.push_back({
                glm::vec3(worldMat * glm::vec4(g.vertices[g.indices[t * 3]].position, 1.0f)),
                glm::vec3(worldMat * glm::vec4(g.vertices[g.indices[t * 3 + 1]].position, 1.0f)),
                glm::vec3(worldMat * glm::vec4(g.vertices[g.indices[t * 3 + 2]].position, 1.0f))});
        }
    }
    // sunwell_039 ("wallsplit") is 100% detail (0x24) and must contribute zero blocking triangles.
    CHECK(group39Blocking == 0);
    REQUIRE(sunwellTris.size() > 9000);

    auto runRoute = [&](glm::vec3 serverStart, float serverYaw, float holdSec,
                        float speed, float radius, float height) {
        // serverToCanonical swaps (x, y) and canonicalToRender swaps (x, y) back, so render == server (X, Y, Z).
        const glm::vec3 renderStart = wowee::core::coords::canonicalToRender(
            wowee::core::coords::serverToCanonical(serverStart));
        const glm::vec2 renderFwd(std::cos(serverYaw), std::sin(serverYaw));
        SimulatedWalker w{renderStart, radius, height, true, false, 0.0f, renderStart.z};
        const int frames = static_cast<int>(std::ceil(holdSec * 60.0f));
        for (int f = 0; f < frames; ++f) {
            w.advanceFrame(renderFwd, speed, 1.0f / 60.0f, sunwellTris);
        }
        return wowee::core::coords::canonicalToServer(wowee::core::coords::renderToCanonical(w.pos));
    };

    for (auto [raceName, r, h] : {std::tuple{"Human", 0.305556f, 2.03127f},
                                  std::tuple{"Tauren", 0.972222f, 2.23244f}}) {
        for (auto [speed, scale] : {std::pair{7.0f, 1.0f}, std::pair{35.0f, 0.22f}}) {
            INFO(raceName << " speed=" << speed);

            // 1. Sunken courtyard border curb + multi-step Arch Mage stairs (straight & diagonal)
            const glm::vec3 curbEnd = runRoute({1637.50f, 911.50f, 14.65f}, 1.570796f, 2.6f * scale, speed, r, h);
            CHECK(curbEnd.y > 925.0f);
            CHECK(curbEnd.z > 16.80f);

            const glm::vec3 curbDiag = runRoute({1645.01f, 911.75f, 14.64f}, 2.15f, 2.8f * scale, speed, r, h);
            CHECK(curbDiag.y > 923.0f);
            CHECK(curbDiag.z > 16.80f);

            // 2. Main south staircase (0.808 / 0.761 / 0.940 yd vertical 0x48 risers, straight & diagonal)
            const glm::vec3 mainEnd = runRoute({1719.50f, 873.50f, 15.56f}, 3.141593f, 2.2f * scale, speed, r, h);
            CHECK(mainEnd.x < 1707.0f);
            CHECK(mainEnd.z == Catch::Approx(18.029f).margin(0.08f));

            const glm::vec3 mainDiag = runRoute({1719.50f, 871.50f, 15.56f}, 2.85f, 2.4f * scale, speed, r, h);
            CHECK(mainDiag.x < 1707.0f);
            CHECK(mainDiag.z == Catch::Approx(18.029f).margin(0.08f));

            // 3. 7-step white marble gazebo stairs (previously blocked by sunwell_051 antiportal!)
            const glm::vec3 gazeboEnd = runRoute({1649.14f, 838.709f, 33.94f}, 3.09212f, 2.0f * scale, speed, r, h);
            CHECK(gazeboEnd.x < 1638.0f);
            CHECK(gazeboEnd.z > 35.10f);

            // 4. Real Parhelion Plaza solid wall: must still stop both races at 1x and 5x!
            const glm::vec3 wallEnd = runRoute({1633.00f, 931.80f, 17.09f}, 0.0f, 1.8f * scale, speed, r, h);
            CHECK(wallEnd.x < 1638.5f);
            CHECK(wallEnd.z < 18.15f);
        }
    }
}
