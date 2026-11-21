// Corpse fade-out ahead of removal (ledger #75c), rendering/unit_fade.hpp.
#include <catch_amalgamated.hpp>

#include "rendering/unit_fade.hpp"

using wowee::rendering::creatureSpawnFadeDuration;
using wowee::rendering::fadeInOpacity;
using wowee::rendering::fadeOutOpacity;
using wowee::rendering::kUnitDespawnFadeOutSeconds;
using wowee::rendering::kUnitFadeSeconds;
using wowee::rendering::kUnitSpawnFadeInSeconds;

TEST_CASE("a despawning corpse fades from its current opacity to zero", "[unit-fade]") {
    CHECK(fadeOutOpacity(1.0f, 0.0f, kUnitFadeSeconds) == 1.0f);
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.5f));
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    CHECK(fadeOutOpacity(1.0f, 2 * kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    // A corpse still fading in, or a stealth-translucent one, starts lower.
    CHECK(fadeOutOpacity(0.4f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.2f));
    CHECK(fadeOutOpacity(1.0f, 0.1f, 0.0f) == 0.0f);
}

TEST_CASE("a newly spawned unit fades in smoothly to its target display opacity", "[unit-fade]") {
    CHECK(kUnitSpawnFadeInSeconds == 2.0f);
    CHECK(kUnitDespawnFadeOutSeconds == 1.5f);
    CHECK(fadeInOpacity(1.0f, 0.0f, kUnitSpawnFadeInSeconds) == 0.0f);
    CHECK(fadeInOpacity(1.0f, 0.5f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.0625f));
    CHECK(fadeInOpacity(1.0f, 1.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.25f));
    CHECK(fadeInOpacity(1.0f, 1.5f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.5625f));
    CHECK(fadeInOpacity(1.0f, 2.0f, kUnitSpawnFadeInSeconds) == 1.0f);
    CHECK(fadeInOpacity(1.0f, 3.0f, kUnitSpawnFadeInSeconds) == 1.0f);
    // Translucent displays (e.g. spirit models) cap at their authored display alpha.
    CHECK(fadeInOpacity(0.5f, 1.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.125f));
    CHECK(fadeInOpacity(0.5f, 2.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.5f));
    CHECK(fadeInOpacity(0.5f, 0.0f, 0.0f) == Catch::Approx(0.5f));
}

TEST_CASE("fade-out and fade-in share one duration", "[unit-fade]") {
    CHECK(kUnitFadeSeconds == 0.5f);
}

TEST_CASE("in-place creature display swaps do not fade in from zero", "[unit-fade][shapeshift]") {
    // Alive display swap (Serpent Form IN / OUT) and death display revert
    // (Druid of the Fang dying while transformed, even if originally spawned via CREATE_OBJECT2)
    // appear immediately at full display opacity on TBC 2.4.3 build 8606.
    CHECK(creatureSpawnFadeDuration(true, false) == 0.0f);
    CHECK(creatureSpawnFadeDuration(true, true) == 0.0f);
    CHECK(fadeInOpacity(1.0f, 0.0f, creatureSpawnFadeDuration(true, true)) == 1.0f);
    CHECK(fadeInOpacity(0.4f, 0.0f, creatureSpawnFadeDuration(true, false)) == Catch::Approx(0.4f));

    // Initial spawns retain 2.0 s (CREATE_OBJECT2) and 0.5 s (streaming into view).
    CHECK(creatureSpawnFadeDuration(false, true) == kUnitSpawnFadeInSeconds);
    CHECK(creatureSpawnFadeDuration(false, false) == kUnitFadeSeconds);
}
