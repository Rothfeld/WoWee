// When the terrain manager hands freed heap pages back to the system
// (core/heap_release.hpp): once per burst of streaming work, after the work
// has been over for the settle time.

#include <catch_amalgamated.hpp>

#include "core/heap_release.hpp"

using wowee::core::WorkSettledTrigger;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

TEST_CASE("Nothing fires before any work was seen", "[work_settled_trigger]") {
    WorkSettledTrigger trigger(milliseconds(2000));
    const auto t0 = Clock::time_point{};
    for (int ms = 0; ms <= 10000; ms += 500) {
        REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(ms)));
    }
}

TEST_CASE("Fires once, the settle time after the work ended", "[work_settled_trigger]") {
    WorkSettledTrigger trigger(milliseconds(2000));
    const auto t0 = Clock::time_point{};
    REQUIRE_FALSE(trigger.update(true, t0));
    REQUIRE_FALSE(trigger.update(true, t0 + milliseconds(900)));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(1000)));   // idle begins here
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(2999)));
    REQUIRE(trigger.update(false, t0 + milliseconds(3000)));
    for (int ms = 3100; ms <= 20000; ms += 100) {
        REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(ms)));
    }
}

TEST_CASE("Work that resumes restarts the wait", "[work_settled_trigger]") {
    WorkSettledTrigger trigger(milliseconds(2000));
    const auto t0 = Clock::time_point{};
    REQUIRE_FALSE(trigger.update(true, t0));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(100)));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(1900)));
    REQUIRE_FALSE(trigger.update(true, t0 + milliseconds(2000)));    // a tile arrives
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(2100)));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(4099)));
    REQUIRE(trigger.update(false, t0 + milliseconds(4100)));
}

TEST_CASE("Each burst fires again", "[work_settled_trigger]") {
    WorkSettledTrigger trigger(milliseconds(2000));
    const auto t0 = Clock::time_point{};
    int fired = 0;
    for (int burst = 0; burst < 5; ++burst) {
        const auto start = t0 + milliseconds(burst * 10000);
        REQUIRE_FALSE(trigger.update(true, start));
        for (int ms = 100; ms < 10000; ms += 100) {
            if (trigger.update(false, start + milliseconds(ms))) ++fired;
        }
    }
    REQUIRE(fired == 5);
}

TEST_CASE("A late first idle sample still waits the whole settle time", "[work_settled_trigger]") {
    // update() is not called while streaming is disabled; the first idle
    // sample after the work can come long after it ended.
    WorkSettledTrigger trigger(milliseconds(2000));
    const auto t0 = Clock::time_point{};
    REQUIRE_FALSE(trigger.update(true, t0));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(60000)));
    REQUIRE_FALSE(trigger.update(false, t0 + milliseconds(61999)));
    REQUIRE(trigger.update(false, t0 + milliseconds(62000)));
}
