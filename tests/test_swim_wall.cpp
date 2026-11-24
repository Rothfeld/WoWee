// A shoreline is a wall to a swimmer, and was not treated as one.
//
// The swept collision a swimmer gets checks WMO walls and doodads. The
// shoreline is neither: it is the terrain heightmap, which the swim path only
// ever consulted downward, looking for a floor to hold the swimmer above. So
// swimming into a hillside was not refused - and the floor probe will not
// recover it, because it accepts a floor only at or just above the feet and a
// cliff face ahead is far above them. Leave the water inside the hill and you
// fall through the world.
#include <catch_amalgamated.hpp>

#include <optional>

#include "rendering/swim_wall.hpp"

using wowee::rendering::liquidBedTerrain;
using wowee::rendering::SwimStep;
using wowee::rendering::swimStepAgainstTerrain;

namespace {

/// Flat water at height 0 with a cliff wall east of x = 10.
std::optional<float> cliffAtX10(float x, float /*y*/) {
    return x >= 10.0f ? std::optional<float>(40.0f) : std::optional<float>(-5.0f);
}

/// A beach shelving gently up toward +y, never more than a body's float above.
std::optional<float> gentleBeach(float /*x*/, float y) {
    return std::optional<float>(-2.0f + y * 0.05f);
}

}  // namespace

TEST_CASE("a swimmer is stopped at a cliff face", "[swim]") {
    const SwimStep from{.x = 9.0f, .y = 5.0f};
    const SwimStep into{.x = 11.0f, .y = 5.0f};   // straight into the wall
    const SwimStep out = swimStepAgainstTerrain(from, into, 1.0f, cliffAtX10);
    CHECK(out.x == from.x);
    CHECK(out.y == from.y);
}

TEST_CASE("a swimmer slides along the shore rather than sticking to it", "[swim]") {
    // Meeting the wall at an angle: the x half is refused, the y half is not.
    const SwimStep from{.x = 9.0f, .y = 5.0f};
    const SwimStep into{.x = 11.0f, .y = 7.0f};
    const SwimStep out = swimStepAgainstTerrain(from, into, 1.0f, cliffAtX10);
    CHECK(out.x == from.x);
    CHECK(out.y == into.y);   // still travelling along the shore
}

TEST_CASE("a beach under the swimmer stays passable", "[swim]") {
    // Swimming ashore: the ground rises, but never above the float clearance,
    // so nothing is refused and the swimmer reaches the sand.
    const SwimStep from{.x = 0.0f, .y = 10.0f};
    const SwimStep into{.x = 0.0f, .y = 20.0f};   // terrain here is -1.0
    const SwimStep out = swimStepAgainstTerrain(from, into, 1.0f, gentleBeach);
    CHECK(out.x == into.x);
    CHECK(out.y == into.y);
}

TEST_CASE("open water is never refused", "[swim]") {
    const SwimStep from{.x = 0.0f, .y = 0.0f};
    const SwimStep into{.x = 5.0f, .y = 5.0f};
    const SwimStep out = swimStepAgainstTerrain(from, into, 1.0f, cliffAtX10);
    CHECK(out.x == into.x);
    CHECK(out.y == into.y);
}

TEST_CASE("terrain with no height at all blocks nothing", "[swim]") {
    // Off the edge of the loaded map the heightmap has no answer, and a
    // missing answer must not read as a wall - that would freeze a swimmer at
    // a tile boundary still streaming in.
    const auto noHeight = [](float, float) { return std::optional<float>{}; };
    const SwimStep from{.x = 0.0f, .y = 0.0f};
    const SwimStep into{.x = 3.0f, .y = 3.0f};
    const SwimStep out = swimStepAgainstTerrain(from, into, 1.0f, noHeight);
    CHECK(out.x == into.x);
    CHECK(out.y == into.y);
}

TEST_CASE("a swimmer under the heightfield is not walled in by it", "[swim]") {
    // C'Thun's stomach: slime at -98 inside a WMO, Ahn'Qiraj's desert at 204.8
    // overhead everywhere. Native build 8606 swims about in the acid.
    const auto desertOverhead = [](float, float) { return std::optional<float>(204.8f); };
    const SwimStep from{.x = 0.0f, .y = 0.0f};
    const SwimStep into{.x = 0.5f, .y = 0.3f};
    const SwimStep out = swimStepAgainstTerrain(from, into, -99.6f + 0.9f, desertOverhead);
    CHECK(out.x == into.x);
    CHECK(out.y == into.y);
}

TEST_CASE("heightfield above the swimmer is not a liquid's bed", "[swim]") {
    // Feet at -99.6 in C'Thun's stomach acid, probed from 2 yd above them.
    CHECK_FALSE(liquidBedTerrain(204.8f, -97.6f).has_value());
    CHECK(liquidBedTerrain(-106.6f, -97.6f) == std::optional<float>(-106.6f));
    CHECK_FALSE(liquidBedTerrain(std::nullopt, -97.6f).has_value());
}

TEST_CASE("falling into deep water waits until feet reach swim entry depth", "[swim]") {
    using wowee::rendering::shouldBeInWater;
    constexpr float waterH = 8.199f;       // Slave Pens pool surface
    constexpr float poolFloor = 3.135f;    // ~5 yd deep pool
    constexpr float enterDepth = 1.523f;   // Human Male swimSurfaceOffset_
    constexpr float exitDepth = 0.85f;

    // Toes barely below water surface while falling: still falling, not swimming yet.
    CHECK_FALSE(shouldBeInWater(false, waterH, waterH - 0.05f, poolFloor, enterDepth, exitDepth));
    CHECK_FALSE(shouldBeInWater(false, waterH, waterH - 1.00f, poolFloor, enterDepth, exitDepth));
    // Once feet cross waterH - enterDepth: enter swimming.
    CHECK(shouldBeInWater(false, waterH, waterH - enterDepth - 0.001f, poolFloor, enterDepth, exitDepth));
    CHECK(shouldBeInWater(false, waterH, waterH - 2.03f, poolFloor, enterDepth, exitDepth));

    // Grounded beach walk-in (feet == floor): enters at exact enterDepth.
    CHECK_FALSE(shouldBeInWater(false, waterH, waterH - 1.20f, waterH - 1.20f, enterDepth, exitDepth));
    CHECK(shouldBeInWater(false, waterH, waterH - enterDepth - 0.001f, waterH - enterDepth - 0.001f, enterDepth, exitDepth));

    // Shallow puddle (< enterDepth): never enters swimming.
    CHECK_FALSE(shouldBeInWater(false, waterH, waterH - 1.60f, waterH - 0.50f, enterDepth, exitDepth));

    // Already swimming: uses exitDepth hysteresis for shore exit.
    CHECK(shouldBeInWater(true, waterH, waterH - 1.00f, waterH - 1.00f, enterDepth, exitDepth));
    CHECK_FALSE(shouldBeInWater(true, waterH, waterH - 0.70f, waterH - 0.70f, enterDepth, exitDepth));
}

TEST_CASE("water entry velocity retains downward plunge proportional to impact speed", "[swim]") {
    using wowee::rendering::waterEntryVerticalVelocity;
    // Gentle walk-in or upward velocity produces zero plunge.
    CHECK(waterEntryVerticalVelocity(0.0f) == 0.0f);
    CHECK(waterEntryVerticalVelocity(5.0f) == 0.0f);

    // Downward impact retains 20% velocity -> under 6/s damping, plunges ~|v|/30 yd.
    CHECK(waterEntryVerticalVelocity(-15.0f) == Catch::Approx(-3.0f));
    CHECK(waterEntryVerticalVelocity(-22.5f) == Catch::Approx(-4.5f));
    // Simulate damping loop at 60 Hz for 0.5 s: total plunge matches ~|v|/30 yd.
    float vz = waterEntryVerticalVelocity(-20.5f);
    float dz = 0.0f;
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) {
        vz *= std::max(0.0f, 1.0f - 6.0f * dt);
        if (std::abs(vz) < 0.05f) vz = 0.0f;
        dz += vz * dt;
    }
    CHECK(dz < -0.50f);
    CHECK(dz > -0.80f);
}
