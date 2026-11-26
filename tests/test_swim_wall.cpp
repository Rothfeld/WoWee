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

TEST_CASE("water entry zeros vertical velocity after the ballistic crossing step", "[swim]") {
    using wowee::rendering::kTerminalFallVelocity;
    using wowee::rendering::shouldBeInWater;
    using wowee::rendering::stepFreeFallVertical;
    using wowee::rendering::waterEntryVerticalVelocity;

    // Native 2.4.3 stops vertical motion on the frame `MSG_MOVE_START_SWIM` fires
    // (`dz = 0.0000` across +19..+950 ms after a 70 yd Karazhan bridge jump).
    CHECK(waterEntryVerticalVelocity(0.0f) == 0.0f);
    CHECK(waterEntryVerticalVelocity(5.0f) == 0.0f);
    CHECK(waterEntryVerticalVelocity(-15.0f) == 0.0f);
    CHECK(waterEntryVerticalVelocity(-52.414f) == 0.0f);

    // Simulate Karazhan high-bridge drop (`z0 = 94.887 -> waterH = 24.836`,
    // `enterDepth = 1.52325`, riverbed `20.80`) at 60 Hz and 30 Hz.
    constexpr float waterH = 24.836f;
    constexpr float enterDepth = 1.52325f;
    constexpr float exitDepth = 0.85f;
    constexpr float floatZ = waterH - enterDepth;
    constexpr float riverFloor = 20.80f;
    constexpr float gravity = -19.29f;

    for (float dt : {1.0f / 60.0f, 1.0f / 30.0f}) {
        float z = 94.887f;
        float vz = 0.0f;
        bool swimming = false;
        int swimEntryFrame = -1;
        float entryZ = z;
        float preEntryVz = 0.0f;

        for (int frame = 0; frame < 300; ++frame) {
            if (!swimming) {
                const auto step = stepFreeFallVertical(z, vz, gravity, dt);
                preEntryVz = step.verticalVelocity;
                z = std::max(step.z, riverFloor);
                vz = step.verticalVelocity;
                if (shouldBeInWater(false, waterH, z, riverFloor, enterDepth, exitDepth)) {
                    swimming = true;
                    vz = waterEntryVerticalVelocity(vz);
                    swimEntryFrame = frame;
                    entryZ = z;
                }
            } else {
                vz *= std::max(0.0f, 1.0f - 6.0f * dt);
                z += vz * dt;
            }
        }

        REQUIRE(swimming);
        CHECK(swimEntryFrame > 0);
        // Crossing frame lands within one free-fall frame below floatZ, well above riverbed.
        CHECK(entryZ <= floatZ);
        CHECK(entryZ >= floatZ + preEntryVz * dt - 1e-3f);
        CHECK(entryZ > 21.5f);
        // Zero post-entry sink after MSG_MOVE_START_SWIM.
        CHECK(z == Catch::Approx(entryZ));
        CHECK(vz == 0.0f);
    }

    // 250 yd high drop caps downward speed at TBC terminal velocity (60.148003 yd/s).
    float z = 274.836f;
    float vz = 0.0f;
    for (int frame = 0; frame < 400; ++frame) {
        const auto step = stepFreeFallVertical(z, vz, gravity, 1.0f / 60.0f);
        z = step.z;
        vz = step.verticalVelocity;
        CHECK(vz >= -kTerminalFallVelocity - 1e-5f);
        if (shouldBeInWater(false, waterH, z, riverFloor, enterDepth, exitDepth)) {
            vz = waterEntryVerticalVelocity(vz);
            break;
        }
    }
    CHECK(vz == 0.0f);
    CHECK(z <= floatZ);
    CHECK(z >= floatZ - kTerminalFallVelocity * (1.0f / 60.0f) - 1e-3f);
}

TEST_CASE("Slow Fall / Feather Fall caps downward speed at TBC 2.4.3 7.0 yd/s terminal velocity", "[swim][featherfall]") {
    using wowee::rendering::kTerminalSavefallVelocity;
    using wowee::rendering::stepFreeFallVertical;

    REQUIRE(kTerminalSavefallVelocity == Catch::Approx(7.0f));

    constexpr float gravity = -19.29f;
    constexpr float dt = 1.0f / 60.0f;
    float z = 80.0f;
    float vz = 0.0f;
    // Accelerate from rest to -7.0 yd/s (~0.363 s), then cruise for 1.0 s total.
    for (int frame = 0; frame < 60; ++frame) {
        const auto step = stepFreeFallVertical(z, vz, gravity, dt, kTerminalSavefallVelocity);
        z = step.z;
        vz = step.verticalVelocity;
        CHECK(vz >= -kTerminalSavefallVelocity - 1e-5f);
    }
    CHECK(vz == Catch::Approx(-7.0f));

    // Over the next 0.50 s (one MSG_MOVE_HEARTBEAT interval) at terminal speed,
    // z drops by exactly 3.50 yd (matching Native TBC 2.4.3 Slow Fall 74.27 -> 70.77 -> 67.27).
    const float zBeforeHeartbeat = z;
    for (int frame = 0; frame < 30; ++frame) {
        const auto step = stepFreeFallVertical(z, vz, gravity, dt, kTerminalSavefallVelocity);
        z = step.z;
        vz = step.verticalVelocity;
    }
    CHECK((zBeforeHeartbeat - z) == Catch::Approx(3.50f).margin(1e-3f));
}
