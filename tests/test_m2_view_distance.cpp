#include <catch_amalgamated.hpp>

#include <cmath>

#include "rendering/m2_view_distance.hpp"

using wowee::rendering::m2InstanceMaxDistSq;
using wowee::rendering::tbcDoodadDistanceAlpha;
using wowee::rendering::tbcDoodadFadeBand;
using wowee::rendering::tbcDoodadOpaqueMaxDistance;
using wowee::rendering::tbcSmallCullForEnvironmentDetail;
using wowee::rendering::tbcSmallCullForWorldDetail;
using wowee::rendering::tbcWorldDetailForSmallCull;

namespace {
float distanceOf(float distSq) { return std::sqrt(distSq); }
}

TEST_CASE("an ordinary doodad draws to the scene distance", "[m2][viewdist]") {
    const float base = 1000.0f * 1000.0f;
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 2400.0f))
            == Catch::Approx(1000.0f));
}

TEST_CASE("a large model's widening factor stops at the view distance", "[m2][viewdist]") {
    // The factor is the reason distant trees stood on nothing: it multiplies a
    // squared distance, so a cathedral-sized doodad asked for several times the
    // scene's range while the terrain under it stopped at the slider.
    const float base = 1000.0f * 1000.0f;
    const float factor = 16.0f;  // 4x the distance, unclamped
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, factor, false, 4000.0f))
            == Catch::Approx(4000.0f));
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, factor, false, 1200.0f))
            == Catch::Approx(1200.0f));
}

TEST_CASE("a server object draws to the view distance", "[m2][viewdist]") {
    // Game objects, unit emitters and spell visuals: what the server keeps in
    // range is drawn, however short the ambient doodad distance has become.
    const float base = 100.0f * 100.0f;
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, 1.0f, true, 2400.0f)) == Catch::Approx(2400.0f));
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, 1.0f, true, 400.0f)) == Catch::Approx(400.0f));
    // Ground cover's radius is not theirs.
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, 1.0f, true, 400.0f, true, 70.0f)) == Catch::Approx(400.0f));
}

TEST_CASE("scenery keeps the ambient distance", "[m2][viewdist]") {
    const float base = 100.0f * 100.0f;
    REQUIRE(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 2400.0f))
            == Catch::Approx(100.0f));
}

TEST_CASE("raising the view distance never shortens an instance's range", "[m2][viewdist]") {
    const float base = 1000.0f * 1000.0f;
    float previous = 0.0f;
    for (float viewDistance = 400.0f; viewDistance <= 2400.0f; viewDistance += 100.0f) {
        const float d = m2InstanceMaxDistSq(base, 3.0f, true, viewDistance);
        REQUIRE(d >= previous);
        previous = d;
    }
}

TEST_CASE("Environment detail can thin the clutter but not push it past the ground",
          "[m2][viewdist]") {
    // Environment Detail scales how far doodads are drawn. It only ever takes
    // away: raising it grows the base distance, and the ceiling - the terrain's
    // own view distance - still holds, because a doodad past that is the tree
    // standing on nothing this clamp was added for.
    const float viewDistance = 1200.0f;

    // The base the scale produces, as M2Renderer computes it.
    auto baseFor = [](float detail) {
        const float scale = (1200.0f / 1200.0f) * detail;
        const float dist = scale * 1000.0f;   // the low-density constant
        return dist * dist;
    };

    // Turned down, doodads stop closer than the world does.
    const float thin = m2InstanceMaxDistSq(baseFor(0.5f), 1.0f, false, viewDistance);
    CHECK(distanceOf(thin) == Catch::Approx(500.0f));

    // Turned up, the ceiling is what answers, not the setting.
    const float wide = m2InstanceMaxDistSq(baseFor(1.5f), 4.0f, false, viewDistance);
    CHECK(distanceOf(wide) == Catch::Approx(viewDistance));
}

TEST_CASE("Ground cover stops at its own radius, not the doodads'", "[m2][viewdist]") {
    // Grass is drawn between 70 and 140 yards in the original client while
    // doodads run to the horizon, and Ground Clutter Radius names those yards
    // outright - so it is a ceiling on ground detail alone.
    const float base = 1000.0f * 1000.0f;
    const float viewDistance = 2400.0f;

    const float grass = m2InstanceMaxDistSq(base, 1.0f, false, viewDistance,
                                            /*isGroundDetail=*/true, 70.0f);
    CHECK(distanceOf(grass) == Catch::Approx(70.0f));

    // Everything else is untouched by it.
    const float tree = m2InstanceMaxDistSq(base, 1.0f, false, viewDistance,
                                           /*isGroundDetail=*/false, 70.0f);
    CHECK(distanceOf(tree) == Catch::Approx(1000.0f));
}

TEST_CASE("The clutter radius cannot reach past the world either", "[m2][viewdist]") {
    // A radius wider than the view distance is still held by the ceiling: the
    // ground cover has no more ground to sit on than anything else does.
    const float base = 1000.0f * 1000.0f;
    const float grass = m2InstanceMaxDistSq(base, 1.0f, false, /*viewDistance=*/400.0f,
                                            /*isGroundDetail=*/true, 500.0f);
    CHECK(distanceOf(grass) == Catch::Approx(400.0f));
}

TEST_CASE("No clutter radius set leaves ground detail as it was", "[m2][viewdist]") {
    // Zero means "no cap of its own", which is what a client that has never
    // been told the setting should do.
    const float base = 1000.0f * 1000.0f;
    const float grass = m2InstanceMaxDistSq(base, 1.0f, false, 2400.0f,
                                            /*isGroundDetail=*/true, 0.0f);
    CHECK(distanceOf(grass) == Catch::Approx(1000.0f));
}

TEST_CASE("TBC SmallCull maps WorldDetail and Environment Detail onto 0.07 / 0.04 / 0.01",
          "[m2][viewdist][smallcull]") {
    CHECK(tbcSmallCullForWorldDetail(0) == Catch::Approx(0.07f));
    CHECK(tbcSmallCullForWorldDetail(1) == Catch::Approx(0.04f));
    CHECK(tbcSmallCullForWorldDetail(2) == Catch::Approx(0.01f));

    CHECK(tbcWorldDetailForSmallCull(0.07f) == 0);
    CHECK(tbcWorldDetailForSmallCull(0.04f) == 1);
    CHECK(tbcWorldDetailForSmallCull(0.01f) == 2);

    CHECK(tbcSmallCullForEnvironmentDetail(0.5f) == Catch::Approx(0.07f));
    CHECK(tbcSmallCullForEnvironmentDetail(1.0f) == Catch::Approx(0.04f));
    CHECK(tbcSmallCullForEnvironmentDetail(1.5f) == Catch::Approx(0.01f));
}

TEST_CASE("Outdoor ADT MDDF doodads follow TBC SmallCull 2.5*r/smallCull projection law",
          "[m2][viewdist][smallcull]") {
    // Empirically verified on WOWEE_PAIR=3 in Northshire Abbey across smallCull = 0.07, 0.04, 0.01:
    // stein.m2 (vrad=0.209 -> floor 0.22), jar01.m2 (vrad=0.392), sack01.m2 (vrad=0.562),
    // barrel01.m2 (vrad=0.616), stormwindgypsywagon01.m2 (vrad=7.13).
    CHECK(tbcDoodadOpaqueMaxDistance(0.209f, 0.07f) == Catch::Approx(7.85714f).margin(0.01f));
    CHECK(tbcDoodadOpaqueMaxDistance(0.209f, 0.04f) == Catch::Approx(13.75f));
    CHECK(tbcDoodadOpaqueMaxDistance(0.209f, 0.01f) == Catch::Approx(55.0f));

    CHECK(tbcDoodadOpaqueMaxDistance(0.392f, 0.07f) == Catch::Approx(14.0f));
    CHECK(tbcDoodadOpaqueMaxDistance(0.392f, 0.04f) == Catch::Approx(24.5f));

    CHECK(tbcDoodadOpaqueMaxDistance(0.562f, 0.07f) == Catch::Approx(20.0714f).margin(0.01f));
    CHECK(tbcDoodadOpaqueMaxDistance(0.562f, 0.04f) == Catch::Approx(35.125f));

    CHECK(tbcDoodadOpaqueMaxDistance(0.616f, 0.07f) == Catch::Approx(22.0f));
    CHECK(tbcDoodadOpaqueMaxDistance(0.616f, 0.04f) == Catch::Approx(38.5f));

    CHECK(tbcDoodadOpaqueMaxDistance(7.13f, 0.04f) == Catch::Approx(445.625f));
    CHECK(tbcDoodadOpaqueMaxDistance(7.13f, 0.01f, /*distCull=*/500.0f) == Catch::Approx(500.0f));
}

TEST_CASE("WMO interior MODD doodads cull floor skulls at 25 yd with 5 yd objectFade band",
          "[m2][viewdist][smallcull][wmo]") {
    // Molten Core floor skulls (BattleGladeSkull01..04, scaled vertexRadius 0.24..0.38 yd)
    // hit the 0.40-yd WMO MODD radius floor at TBC's fixed 0.04 threshold -> 25.0 yd opaque,
    // invariant to WorldDetail 0..2, while large cavern vents/lava falls draw across the cavern.
    for (float sc : {0.07f, 0.04f, 0.01f}) {
        CHECK(tbcDoodadOpaqueMaxDistance(0.30f, sc, 0.0f, /*wmoDoodad=*/true) == Catch::Approx(25.0f));
    }
    CHECK(tbcDoodadOpaqueMaxDistance(3.00f, 0.04f, 500.0f, /*wmoDoodad=*/true) == Catch::Approx(187.5f));
    CHECK(tbcDoodadOpaqueMaxDistance(38.89f, 0.04f, 500.0f, /*wmoDoodad=*/true) == Catch::Approx(500.0f));

    const float opaqueDist = 25.0f;
    const float fadeBand = tbcDoodadFadeBand(opaqueDist, /*objectFadeEnabled=*/true);
    REQUIRE(fadeBand == Catch::Approx(5.0f));
    CHECK(tbcDoodadFadeBand(opaqueDist, /*objectFadeEnabled=*/false) == Catch::Approx(0.0f));

    CHECK(tbcDoodadDistanceAlpha(23.97f * 23.97f, opaqueDist, fadeBand) == Catch::Approx(1.0f));
    CHECK(tbcDoodadDistanceAlpha(25.0f * 25.0f, opaqueDist, fadeBand) == Catch::Approx(1.0f));
    CHECK(tbcDoodadDistanceAlpha(27.75f * 27.75f, opaqueDist, fadeBand) == Catch::Approx(0.45f));
    CHECK(tbcDoodadDistanceAlpha(30.0f * 30.0f, opaqueDist, fadeBand) == Catch::Approx(0.0f));

    // m2InstanceMaxDistSq honors doodadMaxDistance for placed scenery without
    // affecting server objects or ground clutter, and still clamps to viewDistance.
    const float base = 300.0f * 300.0f;
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 1900.0f, false, 70.0f, 30.0f))
          == Catch::Approx(30.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 1900.0f, false, 70.0f, 470.625f))
          == Catch::Approx(470.625f));
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 400.0f, false, 70.0f, 470.625f))
          == Catch::Approx(400.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 177.0f, false, 70.0f, 470.625f))
          == Catch::Approx(177.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, true, 1900.0f, false, 70.0f, 30.0f))
          == Catch::Approx(1900.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(base, 1.0f, false, 1900.0f, true, 70.0f, 30.0f))
          == Catch::Approx(70.0f));
}
