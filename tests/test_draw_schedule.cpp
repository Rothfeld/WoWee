/// Which ticks of the main loop draw a picture (core/draw_schedule.hpp).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/draw_schedule.hpp"
#include "rendering/sun_occlusion_ease.hpp"

using wowee::core::DrawSchedule;

namespace {

std::vector<bool> run(DrawSchedule& schedule, int ticks, bool inWorld) {
    std::vector<bool> drawn;
    for (int i = 0; i < ticks; ++i) drawn.push_back(schedule.beginTick(inWorld));
    return drawn;
}

} // namespace

TEST_CASE("a normal client draws every tick", "[draw-schedule]") {
    DrawSchedule schedule;
    CHECK(schedule.every() == 1);
    CHECK(run(schedule, 5, false) == std::vector<bool>{true, true, true, true, true});
    CHECK(run(schedule, 5, true) == std::vector<bool>{true, true, true, true, true});
    CHECK(schedule.ticks() == 10);
    CHECK(schedule.drawnTicks() == 10);
}

TEST_CASE("one tick in four is drawn in the world, the first of them the first", "[draw-schedule]") {
    DrawSchedule schedule(4);
    CHECK(run(schedule, 9, true) ==
          std::vector<bool>{true, false, false, false, true, false, false, false, true});
    CHECK(schedule.ticks() == 9);
    CHECK(schedule.drawnTicks() == 3);
}

TEST_CASE("outside the world every tick is drawn, whatever the count", "[draw-schedule]") {
    DrawSchedule schedule(4);
    CHECK(run(schedule, 6, false) == std::vector<bool>{true, true, true, true, true, true});
}

TEST_CASE("entering the world again starts the count over", "[draw-schedule]") {
    DrawSchedule schedule(3);
    CHECK(run(schedule, 2, true) == std::vector<bool>{true, false});
    // A loading screen or the character list in between.
    CHECK(run(schedule, 1, false) == std::vector<bool>{true});
    CHECK(run(schedule, 4, true) == std::vector<bool>{true, false, false, true});
}

TEST_CASE("a host's request decides one tick and wins over the count", "[draw-schedule]") {
    DrawSchedule schedule(4);
    schedule.request(false);
    CHECK_FALSE(schedule.beginTick(true));   // tick 0 would have been drawn
    schedule.request(true);
    CHECK(schedule.beginTick(true));         // tick 1 would not
    // Not asked again: the count carries on where it would have been.
    CHECK(run(schedule, 3, true) == std::vector<bool>{false, false, true});
    CHECK(schedule.drawnTicks() == 2);
}

TEST_CASE("a host's request holds outside the world too", "[draw-schedule]") {
    DrawSchedule schedule;
    schedule.request(false);
    CHECK_FALSE(schedule.beginTick(false));
    CHECK(schedule.beginTick(false));
}

TEST_CASE("a later request replaces an earlier one for the same tick", "[draw-schedule]") {
    DrawSchedule schedule;
    schedule.request(false);
    schedule.request(true);
    CHECK(schedule.beginTick(true));
    CHECK(schedule.beginTick(true));
}

TEST_CASE("a host that draws one step in four by request", "[draw-schedule]") {
    // The way a lockstep host uses it: every tick asked for, the count unused.
    DrawSchedule schedule;
    std::vector<bool> drawn;
    for (int step = 0; step < 8; ++step) {
        schedule.request(step % 4 == 3);
        drawn.push_back(schedule.beginTick(true));
    }
    CHECK(drawn == std::vector<bool>{false, false, false, true, false, false, false, true});
}

TEST_CASE("WOWEE_DRAW_EVERY is a whole number from 1 to 1000", "[draw-schedule]") {
    CHECK(DrawSchedule::everyFromEnvironment(nullptr) == 1);
    CHECK(DrawSchedule::everyFromEnvironment("") == 1);
    CHECK(DrawSchedule::everyFromEnvironment("1") == 1);
    CHECK(DrawSchedule::everyFromEnvironment("4") == 4);
    CHECK(DrawSchedule::everyFromEnvironment("1000") == 1000);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment("0"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment("1001"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment("-2"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment("4x"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment("four"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule::everyFromEnvironment(" 4"), std::invalid_argument);
    CHECK_THROWS_AS(DrawSchedule(0), std::invalid_argument);
}

// ── What a drawn tick eases over, after ticks that sampled nothing ──

using wowee::rendering::easeSunOcclusion;

TEST_CASE("easing once over the summed time equals easing tick by tick towards one sample",
          "[draw-schedule][sun-occlusion]") {
    for (const float start : {0.0f, 0.3f, 1.0f}) {
        for (const float sample : {0.0f, 1.0f}) {
            float stepped = start;
            for (int tick = 0; tick < 4; ++tick) stepped = easeSunOcclusion(stepped, sample, 0.05f);
            const float once = easeSunOcclusion(start, sample, 0.2f);
            CHECK(once == Catch::Approx(stepped).margin(1e-6));
        }
    }
}

TEST_CASE("one tick's ease is what the client computed before the sample was skippable",
          "[draw-schedule][sun-occlusion]") {
    const float dt = 0.016f;
    const float before = 0.4f + (1.0f - 0.4f) * std::clamp(1.0f - std::exp(-dt / 0.25f), 0.0f, 1.0f);
    CHECK(easeSunOcclusion(0.4f, 1.0f, dt) == before);
}

TEST_CASE("a sample that changes between two drawn ticks is the whole of the difference",
          "[draw-schedule][sun-occlusion]") {
    // Draw one tick in four at 50 ms. The sun goes behind something on the
    // last of the four: drawn tick by tick, three eases towards 0 and one
    // towards 1; drawn one in four, a single ease towards 1 over 200 ms.
    float stepped = 0.0f;
    for (int tick = 0; tick < 3; ++tick) stepped = easeSunOcclusion(stepped, 0.0f, 0.05f);
    stepped = easeSunOcclusion(stepped, 1.0f, 0.05f);
    const float once = easeSunOcclusion(0.0f, 1.0f, 0.2f);
    // The largest it gets: exp(-0.05 / 0.25) - exp(-0.2 / 0.25) = 0.370 of the range, on that one frame.
    CHECK(once - stepped == Catch::Approx(std::exp(-0.05f / 0.25f) - std::exp(-0.2f / 0.25f)).margin(1e-5));
    // Both ease towards the same sample from then on; the gap closes with the same time constant.
    float a = stepped, b = once;
    for (int tick = 0; tick < 20; ++tick) {  // one second
        a = easeSunOcclusion(a, 1.0f, 0.05f);
        b = easeSunOcclusion(b, 1.0f, 0.05f);
    }
    CHECK(b - a < 0.01f);
}
